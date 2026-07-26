#include "ec_systemcore/client.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ec_systemcore {
namespace {

constexpr std::size_t kMaximumClientQueue = 64U * 1024U;
constexpr std::size_t kMaximumReceivedInputs = 128;
constexpr std::size_t kMaximumReceivedBusInformation = 64;
constexpr std::size_t kMaximumPdoAssemblies = 16;
constexpr std::size_t kMaximumPdoImages = 32;
constexpr std::size_t kMaximumPdoAssemblyStorage = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumPdoImageQueueStorage =
    4U * 1024U * 1024U;
constexpr auto kMaximumClientDuration = std::chrono::seconds{60};

[[nodiscard]] ClientError ErrorForStatus(protocol::AckStatus status) {
  return status == protocol::AckStatus::Ok ? ClientError::None
                                           : ClientError::ServerRejected;
}

}  // namespace

class Client::Implementation final {
  friend class Client;
 public:
  explicit Implementation(ClientOptions options)
      : options_(std::move(options)),
        reconnectBackoff_(options_.initialReconnectBackoff) {
    if (options_.backgroundReconnect) {
      backgroundRunning_.store(true, std::memory_order_release);
      background_ = std::thread([this] { BackgroundLoop(); });
    }
  }

  ~Implementation() { StopAndDisconnect(); }

  [[nodiscard]] bool Connect() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (connected_) {
        return true;
      }
      if (!ValidateOptions()) {
        return Fail(ClientError::InvalidArgument,
                    "invalid client options");
      }
      const auto now = std::chrono::steady_clock::now();
      if (now < nextReconnect_) {
        // A background retry during backoff must not overwrite the concrete
        // transport/timeout error that caused the disconnect.
        return false;
      }
      CloseSocket();

      descriptor_ =
          socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (descriptor_ < 0) {
        ScheduleReconnect();
        return Fail(ClientError::SocketError,
                    "unable to create IPC socket");
      }
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      std::copy(options_.socketPath.begin(), options_.socketPath.end(),
                address.sun_path);
      address.sun_path[options_.socketPath.size()] = '\0';
      const int result =
          connect(descriptor_, reinterpret_cast<const sockaddr*>(&address),
                  sizeof(address));
      if (result != 0 && errno != EINPROGRESS) {
        ScheduleReconnect();
        CloseSocket();
        return Fail(ClientError::SocketError,
                    "unable to connect to ec-systemcore");
      }
      if (result != 0 &&
          !WaitForSocket(POLLOUT, options_.connectTimeout)) {
        ScheduleReconnect();
        CloseSocket();
        return Fail(ClientError::Timeout,
                    "IPC connect timed out");
      }
      int socketError = 0;
      socklen_t errorLength = sizeof(socketError);
      if (getsockopt(descriptor_, SOL_SOCKET, SO_ERROR, &socketError,
                     &errorLength) != 0 ||
          socketError != 0) {
        ScheduleReconnect();
        CloseSocket();
        return Fail(ClientError::SocketError,
                    "IPC connect failed");
      }

      const std::uint32_t requestId = NextRequestId();
      protocol::HelloPayload hello{
          .role = options_.role,
          .subscribeInputs = options_.subscribeInputs,
          .inputPeriodMs = options_.inputPeriodMs,
          .lastSeenEpoch = lastSeenEpoch_,
      };
      if (!Queue(protocol::EncodeFrame(
              protocol::MessageType::Hello, requestId,
              protocol::EncodeHelloPayload(hello)))) {
        CloseSocket();
        return false;
      }
      expectedHelloRequest_ = requestId;
      helloReceived_ = false;
      const auto deadline =
          std::chrono::steady_clock::now() + options_.connectTimeout;
      while (!helloReceived_ &&
             std::chrono::steady_clock::now() < deadline) {
        if (!PumpUntil(deadline)) {
          break;
        }
      }
      if (!helloReceived_) {
        ScheduleReconnect();
        CloseSocket();
        return Fail(ClientError::Timeout,
                    "HELLO acknowledgement timed out");
      }
      if (options_.role == protocol::ClientRole::Controller &&
          !controllerGranted_) {
        ScheduleReconnect();
        CloseSocket();
        return Fail(ClientError::ControllerDenied,
                    "controller role was denied");
      }
      connected_ = true;
      reconnectBackoff_ = options_.initialReconnectBackoff;
      nextReconnect_ = {};
      nextHeartbeat_ =
          std::chrono::steady_clock::now() +
          effectiveHeartbeatPeriod_;
      ClearError();
      return true;
    } catch (...) {
      CloseSocket();
      return Fail(ClientError::OutOfMemory,
                  "client operation failed");
    }
  }

  [[nodiscard]] bool MaintainConnection() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (connected_) {
      return Poll(std::chrono::milliseconds{0});
    }
    return Connect();
  }

  [[nodiscard]] bool Poll(std::chrono::milliseconds timeout) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (!connected_ || descriptor_ < 0) {
        return Fail(ClientError::NotConnected,
                    "client is disconnected");
      }
      const auto now = std::chrono::steady_clock::now();
      if (controllerGranted_ && now >= nextHeartbeat_) {
        if (!SendHeartbeat(false)) {
          return false;
        }
        nextHeartbeat_ = now + effectiveHeartbeatPeriod_;
      }
      pollfd descriptor{
          .fd = descriptor_,
          .events = static_cast<short>(
              POLLIN | (outgoing_.empty() ? 0 : POLLOUT)),
          .revents = 0,
      };
      const int timeoutValue = static_cast<int>(std::clamp<std::int64_t>(
          timeout.count(), 0, std::numeric_limits<int>::max()));
      const int result = poll(&descriptor, 1, timeoutValue);
      if (result < 0) {
        if (errno == EINTR) {
          return true;
        }
        HandleDisconnect();
        return Fail(ClientError::SocketError, "IPC poll failed");
      }
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        HandleDisconnect();
        return Fail(ClientError::NotConnected,
                    "IPC connection closed");
      }
      if ((descriptor.revents & POLLOUT) != 0 && !Flush()) {
        return false;
      }
      if ((descriptor.revents & POLLIN) != 0 && !Read()) {
        return false;
      }
      return true;
    } catch (...) {
      HandleDisconnect();
      return Fail(ClientError::OutOfMemory,
                  "client poll failed");
    }
  }

  void Disconnect(bool graceful) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (graceful && descriptor_ >= 0 && connected_ &&
          controllerGranted_ && epoch_ != 0) {
        const std::uint32_t disableId = NextRequestId();
        static_cast<void>(Queue(protocol::EncodeFrame(
            protocol::MessageType::OutputEnable, disableId,
            protocol::EncodeOutputEnablePayload(
                {.epoch = epoch_, .enabled = false}))));
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds{20};
        while (!outgoing_.empty() &&
               std::chrono::steady_clock::now() < deadline) {
          if (!Flush()) {
            break;
          }
          pollfd descriptor{.fd = descriptor_,
                            .events = POLLOUT,
                            .revents = 0};
          poll(&descriptor, 1, 1);
        }
      }
    } catch (...) {
    }
    CloseSocket();
    connected_ = false;
    controllerGranted_ = false;
    outputsEnabled_ = false;
    epoch_ = 0;
    status_ = {};
  }

  [[nodiscard]] bool Heartbeat() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return SendHeartbeat(true);
  }

  [[nodiscard]] bool EnableOutputs(bool enabled) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (!RequireController()) {
        return false;
      }
      const std::uint32_t requestId = NextRequestId();
      const auto frame = protocol::EncodeFrame(
          protocol::MessageType::OutputEnable, requestId,
          protocol::EncodeOutputEnablePayload(
              {.epoch = epoch_, .enabled = enabled}));
      const protocol::AckStatus status = Request(frame, requestId);
      if (status != protocol::AckStatus::Ok) {
        return FailForRequestStatus(
            status, "output enable request was rejected");
      }
      outputsEnabled_ = enabled;
      ClearError();
      return true;
    } catch (...) {
      return Fail(ClientError::OutOfMemory,
                  "output enable failed");
    }
  }

  [[nodiscard]] bool WriteOutput(
      std::uint16_t busIndex, std::uint16_t subDeviceIndex,
      std::uint32_t offset,
      std::span<const std::uint8_t> data) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (!RequireController() || !outputsEnabled_) {
        return Fail(ClientError::NotConnected,
                    "outputs are not enabled");
      }
      protocol::OutputWritePayload write{
          .epoch = epoch_,
          .busIndex = busIndex,
          .subDeviceIndex = subDeviceIndex,
          .offset = offset,
          .data = std::vector<std::uint8_t>(data.begin(), data.end()),
      };
      const std::vector<std::uint8_t> payload =
          protocol::EncodeOutputWritePayload(write);
      if (payload.empty()) {
        return Fail(ClientError::InvalidArgument,
                    "invalid output write");
      }
      const std::uint32_t requestId = NextRequestId();
      const protocol::AckStatus status = Request(
          protocol::EncodeFrame(protocol::MessageType::OutputWrite,
                                requestId, payload),
          requestId);
      if (status != protocol::AckStatus::Ok) {
        return FailForRequestStatus(status,
                                    "output write was rejected");
      }
      ClearError();
      return true;
    } catch (...) {
      return Fail(ClientError::OutOfMemory,
                  "output write failed");
    }
  }

  [[nodiscard]] bool ReleaseControl() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (!RequireController()) {
        return false;
      }
      const std::uint32_t requestId = NextRequestId();
      const protocol::AckStatus status = Request(
          protocol::EncodeFrame(protocol::MessageType::ReleaseControl,
                                requestId,
                                protocol::EncodeEpochPayload(epoch_)),
          requestId);
      if (status != protocol::AckStatus::Ok) {
        return FailForRequestStatus(status,
                                    "release was rejected");
      }
      controllerGranted_ = false;
      outputsEnabled_ = false;
      ClearError();
      return true;
    } catch (...) {
      return Fail(ClientError::OutOfMemory, "release failed");
    }
  }

  [[nodiscard]] bool SubscribeInputs(bool enabled,
                                     std::uint16_t periodMs) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (!connected_) {
        return Fail(ClientError::NotConnected,
                    "client is disconnected");
      }
      const std::vector<std::uint8_t> payload =
          protocol::EncodeSubscribeInputsPayload(
              {.enabled = enabled, .periodMs = periodMs});
      if (payload.empty()) {
        return Fail(ClientError::InvalidArgument,
                    "invalid input subscription");
      }
      const std::uint32_t requestId = NextRequestId();
      const protocol::AckStatus status = Request(
          protocol::EncodeFrame(protocol::MessageType::SubscribeInputs,
                                requestId, payload),
          requestId);
      if (status != protocol::AckStatus::Ok) {
        return FailForRequestStatus(status,
                                    "subscription was rejected");
      }
      options_.subscribeInputs = enabled;
      options_.inputPeriodMs = periodMs;
      ClearError();
      return true;
    } catch (...) {
      return Fail(ClientError::OutOfMemory,
                  "subscription failed");
    }
  }

  [[nodiscard]] bool EmptyRequest(
      protocol::MessageType type) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (!connected_) {
        return Fail(ClientError::NotConnected,
                    "client is disconnected");
      }
      const std::uint32_t requestId = NextRequestId();
      const protocol::AckStatus status =
          Request(protocol::EncodeFrame(type, requestId), requestId);
      if (status != protocol::AckStatus::Ok) {
        return FailForRequestStatus(status,
                                    "request was rejected");
      }
      ClearError();
      return true;
    } catch (...) {
      return Fail(ClientError::OutOfMemory, "request failed");
    }
  }

  [[nodiscard]] bool RuntimeUnlockAdapter(
      std::uint16_t busIndex) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (!RequireController()) {
        return false;
      }
      const std::uint32_t requestId = NextRequestId();
      const protocol::AckStatus status = Request(
          protocol::EncodeFrame(
              protocol::MessageType::AdapterUnlock, requestId,
              protocol::EncodeAdapterUnlockPayload(
                  {.epoch = epoch_, .busIndex = busIndex})),
          requestId);
      if (status != protocol::AckStatus::Ok) {
        return FailForRequestStatus(
            status, "runtime unlock was rejected");
      }
      outputsEnabled_ = false;
      ClearError();
      return true;
    } catch (...) {
      return Fail(ClientError::OutOfMemory,
                  "runtime unlock failed");
    }
  }

  [[nodiscard]] bool PopInput(
      protocol::PdoInputPayload* input) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (input == nullptr || inputs_.empty()) {
        return false;
      }
      *input = std::move(inputs_.front());
      inputs_.pop_front();
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] bool PopInputImage(PdoImage* image) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (image == nullptr || inputImages_.empty()) {
        return false;
      }
      inputImageStorage_ -= inputImages_.front().data.size();
      *image = std::move(inputImages_.front());
      inputImages_.pop_front();
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] bool PopBusInfo(
      protocol::BusInfoPayload* information) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    try {
      if (information == nullptr || busInformation_.empty()) {
        return false;
      }
      *information = std::move(busInformation_.front());
      busInformation_.pop_front();
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] bool connected() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return connected_;
  }
  [[nodiscard]] bool controllerGranted() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return controllerGranted_;
  }
  [[nodiscard]] bool outputsEnabled() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return outputsEnabled_;
  }
  [[nodiscard]] std::uint64_t epoch() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return epoch_;
  }
  [[nodiscard]] protocol::StatusPayload status() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return status_;
  }
  [[nodiscard]] ClientError lastError() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return lastError_;
  }
  [[nodiscard]] protocol::AckStatus lastServerStatus() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return lastServerStatus_;
  }
  [[nodiscard]] ClientErrorSnapshot errorSnapshot() const noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return {.error = lastError_,
            .serverStatus = lastServerStatus_,
            .message = lastErrorMessage_};
  }

 private:
  struct Acknowledgement {
    std::uint32_t requestId = 0;
    protocol::AckStatus status = protocol::AckStatus::InternalError;
  };

  struct PdoAssembly {
    std::uint16_t busIndex = 0;
    std::uint16_t subDeviceIndex = 0;
    std::uint64_t epoch = 0;
    std::uint64_t cycleSequence = 0;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> received;
    std::size_t receivedBytes = 0;
  };

  [[nodiscard]] bool ValidateOptions() const {
    return !options_.socketPath.empty() &&
           options_.socketPath.front() == '/' &&
           options_.socketPath.size() < sizeof(sockaddr_un::sun_path) &&
           options_.inputPeriodMs >= 10 &&
           options_.inputPeriodMs <= 1000 &&
           options_.connectTimeout.count() > 0 &&
           options_.connectTimeout <= kMaximumClientDuration &&
           options_.requestTimeout.count() > 0 &&
           options_.requestTimeout <= kMaximumClientDuration &&
           options_.initialReconnectBackoff.count() > 0 &&
           options_.initialReconnectBackoff <=
               kMaximumClientDuration &&
           options_.maximumReconnectBackoff >=
               options_.initialReconnectBackoff &&
           options_.maximumReconnectBackoff <=
               kMaximumClientDuration &&
           options_.heartbeatPeriod.count() > 0 &&
           options_.heartbeatPeriod <= kMaximumClientDuration &&
           options_.socketPath.find('\0') == std::string::npos;
  }

  [[nodiscard]] bool RequireController() {
    if (!connected_ || !controllerGranted_ || epoch_ == 0) {
      return Fail(ClientError::NotConnected,
                  "controller session is unavailable");
    }
    return true;
  }

  [[nodiscard]] bool SendHeartbeat(bool waitForAcknowledgement) noexcept {
    try {
      if (!RequireController()) {
        return false;
      }
      const std::uint32_t requestId = NextRequestId();
      const std::vector<std::uint8_t> frame = protocol::EncodeFrame(
          protocol::MessageType::Heartbeat, requestId,
          protocol::EncodeEpochPayload(epoch_));
      if (!waitForAcknowledgement) {
        if (heartbeatRequests_.size() >= 16) {
          InvalidateSession(epoch_);
          return Fail(ClientError::Timeout,
                      "heartbeat acknowledgements stalled");
        }
        if (!Queue(frame)) {
          return false;
        }
        heartbeatRequests_.push_back(requestId);
        return true;
      }
      const protocol::AckStatus status = Request(frame, requestId);
      if (status != protocol::AckStatus::Ok) {
        return FailForRequestStatus(status,
                                    "heartbeat was rejected");
      }
      ClearError();
      return true;
    } catch (...) {
      return Fail(ClientError::OutOfMemory, "heartbeat failed");
    }
  }

  [[nodiscard]] protocol::AckStatus Request(
      const std::vector<std::uint8_t>& frame,
      std::uint32_t requestId) {
    if (!Queue(frame)) {
      return protocol::AckStatus::InternalError;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + options_.requestTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto acknowledgement =
          std::find_if(acknowledgements_.begin(),
                       acknowledgements_.end(),
                       [requestId](const Acknowledgement& candidate) {
                         return candidate.requestId == requestId;
                       });
      if (acknowledgement != acknowledgements_.end()) {
        const protocol::AckStatus status = acknowledgement->status;
        acknowledgements_.erase(acknowledgement);
        return status;
      }
      if (!PumpUntil(deadline)) {
        break;
      }
    }
    // Once a request times out, its execution state is unknowable. Closing
    // the session discards any unsent bytes and makes the daemon advance its
    // safety epoch if it did receive a controller command. Never allow a
    // timed-out output command to execute later from this queue.
    HandleDisconnect();
    Fail(ClientError::Timeout, "request timed out");
    return protocol::AckStatus::InternalError;
  }

  [[nodiscard]] bool PumpUntil(
      std::chrono::steady_clock::time_point deadline) {
    if (!Flush()) {
      return false;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds{0}) {
      return false;
    }
    pollfd descriptor{
        .fd = descriptor_,
        .events = static_cast<short>(
            POLLIN | (outgoing_.empty() ? 0 : POLLOUT)),
        .revents = 0,
    };
    const int timeout = static_cast<int>(std::clamp<std::int64_t>(
        remaining.count(), 1, std::numeric_limits<int>::max()));
    const int result = poll(&descriptor, 1, timeout);
    if (result <= 0) {
      if (result < 0 && errno == EINTR) {
        return true;
      }
      return false;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      HandleDisconnect();
      return false;
    }
    if ((descriptor.revents & POLLOUT) != 0 && !Flush()) {
      return false;
    }
    return (descriptor.revents & POLLIN) == 0 || Read();
  }

  [[nodiscard]] bool Queue(
      const std::vector<std::uint8_t>& frame) {
    if (descriptor_ < 0 || frame.empty() ||
        frame.size() >
            kMaximumClientQueue -
                std::min(outgoingBytes_, kMaximumClientQueue)) {
      return Fail(ClientError::OutOfMemory,
                  "client output queue is full");
    }
    outgoing_.push_back(frame);
    outgoingBytes_ += frame.size();
    return true;
  }

  [[nodiscard]] bool Flush() {
    while (descriptor_ >= 0 && !outgoing_.empty()) {
      std::vector<std::uint8_t>& frame = outgoing_.front();
      const ssize_t count =
          send(descriptor_, frame.data() + outgoingOffset_,
               frame.size() - outgoingOffset_, MSG_NOSIGNAL);
      if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return true;
        }
        if (errno == EINTR) {
          continue;
        }
        HandleDisconnect();
        return Fail(ClientError::SocketError, "IPC send failed");
      }
      if (count == 0) {
        HandleDisconnect();
        return Fail(ClientError::NotConnected,
                    "IPC connection closed");
      }
      outgoingOffset_ += static_cast<std::size_t>(count);
      outgoingBytes_ -= static_cast<std::size_t>(count);
      if (outgoingOffset_ == frame.size()) {
        outgoing_.pop_front();
        outgoingOffset_ = 0;
      }
    }
    return descriptor_ >= 0;
  }

  [[nodiscard]] bool Read() {
    std::array<std::uint8_t,
               protocol::kLengthPrefixSize +
                   protocol::kMaximumFrameLength>
        buffer{};
    for (;;) {
      const ssize_t count =
          recv(descriptor_, buffer.data(), buffer.size(), 0);
      if (count == 0) {
        HandleDisconnect();
        return Fail(ClientError::NotConnected,
                    "IPC connection closed");
      }
      if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return true;
        }
        if (errno == EINTR) {
          continue;
        }
        HandleDisconnect();
        return Fail(ClientError::SocketError, "IPC receive failed");
      }
      std::vector<protocol::Frame> frames;
      if (!decoder_.Feed(
              std::span<const std::uint8_t>{
                  buffer.data(), static_cast<std::size_t>(count)},
              &frames)) {
        HandleDisconnect();
        return Fail(ClientError::ProtocolError,
                    "invalid IPC frame");
      }
      for (const protocol::Frame& frame : frames) {
        if (!Process(frame)) {
          HandleDisconnect();
          return false;
        }
      }
    }
  }

  [[nodiscard]] bool Process(const protocol::Frame& frame) {
    switch (frame.type) {
      case protocol::MessageType::HelloAck: {
        protocol::HelloAckPayload acknowledgement;
        if (frame.requestId != expectedHelloRequest_ ||
            !protocol::DecodeHelloAckPayload(frame.payload,
                                             &acknowledgement)) {
          return Fail(ClientError::ProtocolError,
                      "invalid HELLO acknowledgement");
        }
        if (acknowledgement.outputsEnabled) {
          return Fail(ClientError::ProtocolError,
                      "server resumed outputs during HELLO");
        }
        const auto heartbeatTimeout = std::chrono::milliseconds{
            acknowledgement.heartbeatTimeoutMs};
        if (heartbeatTimeout <= std::chrono::milliseconds{20}) {
          return Fail(ClientError::ProtocolError,
                      "server heartbeat timeout is unsafe");
        }
        effectiveHeartbeatPeriod_ =
            std::min(options_.heartbeatPeriod, heartbeatTimeout / 3);
        effectiveHeartbeatPeriod_ =
            std::max(effectiveHeartbeatPeriod_,
                     std::chrono::milliseconds{10});
        lastSeenEpoch_ = acknowledgement.epoch;
        epoch_ = acknowledgement.epoch;
        controllerGranted_ =
            acknowledgement.grantedRole ==
            protocol::ClientRole::Controller;
        outputsEnabled_ = false;
        helloReceived_ = true;
        return true;
      }
      case protocol::MessageType::Status: {
        protocol::StatusPayload decoded;
        if (frame.requestId != 0 ||
            !protocol::DecodeStatusPayload(frame.payload, &decoded)) {
          return Fail(ClientError::ProtocolError,
                      "invalid STATUS payload");
        }
        if (epoch_ != 0 && decoded.epoch != epoch_) {
          InvalidateSession(decoded.epoch);
          status_ = decoded;
          return false;
        }
        status_ = decoded;
        return true;
      }
      case protocol::MessageType::Ack:
      case protocol::MessageType::Error: {
        protocol::AckStatus status;
        std::uint32_t detail = 0;
        if (!protocol::DecodeAckPayload(frame.payload, &status, &detail)) {
          return Fail(ClientError::ProtocolError,
                      "invalid ACK payload");
        }
        const auto heartbeat =
            std::find(heartbeatRequests_.begin(),
                      heartbeatRequests_.end(), frame.requestId);
        if (heartbeat != heartbeatRequests_.end()) {
          heartbeatRequests_.erase(heartbeat);
          if (status != protocol::AckStatus::Ok) {
            InvalidateSession(epoch_);
            return Fail(ClientError::ServerRejected,
                        "heartbeat was rejected", status);
          }
          return true;
        }
        if (acknowledgements_.size() >= 128) {
          acknowledgements_.pop_front();
        }
        acknowledgements_.push_back({frame.requestId, status});
        lastServerStatus_ = status;
        return true;
      }
      case protocol::MessageType::PdoInput: {
        protocol::PdoInputPayload input;
        if (frame.requestId != 0 ||
            !protocol::DecodePdoInputPayload(frame.payload, &input)) {
          return Fail(ClientError::ProtocolError,
                      "invalid PDO input payload");
        }
        if (input.epoch != epoch_) {
          return true;
        }
        AssembleInput(input);
        if (inputs_.size() >= kMaximumReceivedInputs) {
          inputs_.pop_front();
        }
        inputs_.push_back(std::move(input));
        return true;
      }
      case protocol::MessageType::BusInfo: {
        protocol::BusInfoPayload information;
        if (frame.requestId != 0 ||
            !protocol::DecodeBusInfoPayload(frame.payload,
                                            &information)) {
          return Fail(ClientError::ProtocolError,
                      "invalid BUS_INFO payload");
        }
        if (information.epoch != epoch_) {
          return true;
        }
        if (busInformation_.size() >=
            kMaximumReceivedBusInformation) {
          busInformation_.pop_front();
        }
        busInformation_.push_back(std::move(information));
        return true;
      }
      case protocol::MessageType::OutputsDisabled: {
        protocol::OutputsDisabledPayload disabled;
        if (frame.requestId != 0 ||
            !protocol::DecodeOutputsDisabledPayload(frame.payload,
                                                    &disabled)) {
          return Fail(ClientError::ProtocolError,
                      "invalid outputs-disabled payload");
        }
        outputsEnabled_ = false;
        if (disabled.epoch != epoch_ ||
            disabled.reason != protocol::DisableReason::Requested) {
          InvalidateSession(disabled.epoch);
          return false;
        }
        return true;
      }
      default:
        return Fail(ClientError::ProtocolError,
                    "unexpected server frame type");
    }
  }

  void InvalidateSession(std::uint64_t replacementEpoch) {
    lastSeenEpoch_ = replacementEpoch;
    epoch_ = 0;
    connected_ = false;
    outputsEnabled_ = false;
    controllerGranted_ = false;
    status_ = {};
    outgoing_.clear();
    outgoingOffset_ = 0;
    outgoingBytes_ = 0;
    acknowledgements_.clear();
    heartbeatRequests_.clear();
    inputs_.clear();
    busInformation_.clear();
    assemblies_.clear();
    assemblyStorage_ = 0;
    inputImages_.clear();
    inputImageStorage_ = 0;
  }

  void HandleDisconnect() {
    if (epoch_ != 0) {
      lastSeenEpoch_ = epoch_;
    }
    CloseSocket();
    connected_ = false;
    controllerGranted_ = false;
    outputsEnabled_ = false;
    epoch_ = 0;
    status_ = {};
    inputs_.clear();
    busInformation_.clear();
    assemblies_.clear();
    assemblyStorage_ = 0;
    inputImages_.clear();
    inputImageStorage_ = 0;
    ScheduleReconnect();
  }

  void CloseSocket() {
    if (descriptor_ >= 0) {
      close(descriptor_);
      descriptor_ = -1;
    }
    outgoing_.clear();
    outgoingOffset_ = 0;
    outgoingBytes_ = 0;
    decoder_.Reset();
    acknowledgements_.clear();
    expectedHelloRequest_ = 0;
    helloReceived_ = false;
    heartbeatRequests_.clear();
  }

  void ScheduleReconnect() {
    nextReconnect_ =
        std::chrono::steady_clock::now() + reconnectBackoff_;
    const auto halfMaximum =
        options_.maximumReconnectBackoff / 2;
    reconnectBackoff_ =
        reconnectBackoff_ > halfMaximum
            ? options_.maximumReconnectBackoff
            : std::min(options_.maximumReconnectBackoff,
                       reconnectBackoff_ * 2);
  }

  bool FailForRequestStatus(protocol::AckStatus status,
                            std::string_view message) {
    if (lastError_ == ClientError::Timeout ||
        lastError_ == ClientError::NotConnected ||
        lastError_ == ClientError::SocketError ||
        lastError_ == ClientError::ProtocolError ||
        lastError_ == ClientError::OutOfMemory) {
      return false;
    }
    return Fail(ErrorForStatus(status), message, status);
  }

  void AssembleInput(const protocol::PdoInputPayload& input) {
    const auto stale =
        std::remove_if(assemblies_.begin(), assemblies_.end(),
                       [&input](const PdoAssembly& assembly) {
                         return assembly.busIndex == input.busIndex &&
                                assembly.subDeviceIndex ==
                                    input.subDeviceIndex &&
                                (assembly.epoch != input.epoch ||
                                 assembly.cycleSequence !=
                                     input.cycleSequence ||
                                 assembly.data.size() !=
                                     input.totalSize);
                       });
    assemblies_.erase(stale, assemblies_.end());
    assemblyStorage_ = 0;
    for (const PdoAssembly& retained : assemblies_) {
      assemblyStorage_ +=
          retained.data.size() + retained.received.size();
    }

    auto assembly =
        std::find_if(assemblies_.begin(), assemblies_.end(),
                     [&input](const PdoAssembly& candidate) {
                       return candidate.busIndex == input.busIndex &&
                              candidate.subDeviceIndex ==
                                  input.subDeviceIndex &&
                              candidate.epoch == input.epoch &&
                              candidate.cycleSequence ==
                                  input.cycleSequence &&
                              candidate.data.size() ==
                                  input.totalSize;
                     });
    if (assembly == assemblies_.end()) {
      const std::size_t required =
          static_cast<std::size_t>(input.totalSize) * 2U;
      while (!assemblies_.empty() &&
             (assemblies_.size() >= kMaximumPdoAssemblies ||
              required >
                  kMaximumPdoAssemblyStorage -
                      std::min(assemblyStorage_,
                               kMaximumPdoAssemblyStorage))) {
        assemblyStorage_ -= assemblies_.front().data.size() +
                            assemblies_.front().received.size();
        assemblies_.pop_front();
      }
      if (required > kMaximumPdoAssemblyStorage -
                         std::min(assemblyStorage_,
                                  kMaximumPdoAssemblyStorage)) {
        return;
      }
      PdoAssembly created;
      created.busIndex = input.busIndex;
      created.subDeviceIndex = input.subDeviceIndex;
      created.epoch = input.epoch;
      created.cycleSequence = input.cycleSequence;
      created.data.resize(input.totalSize);
      created.received.resize(input.totalSize, 0);
      assemblyStorage_ += required;
      assemblies_.push_back(std::move(created));
      assembly = std::prev(assemblies_.end());
    }

    for (std::size_t index = 0; index < input.data.size(); ++index) {
      const std::size_t destination =
          static_cast<std::size_t>(input.offset) + index;
      if (assembly->received[destination] != 0 &&
          assembly->data[destination] != input.data[index]) {
        assemblyStorage_ -= assembly->data.size() +
                            assembly->received.size();
        assemblies_.erase(assembly);
        return;
      }
      assembly->data[destination] = input.data[index];
      if (assembly->received[destination] == 0) {
        assembly->received[destination] = 1;
        ++assembly->receivedBytes;
      }
    }
    if (assembly->receivedBytes != assembly->data.size()) {
      return;
    }

    const std::size_t completedBytes = assembly->data.size();
    while (!inputImages_.empty() &&
           (inputImages_.size() >= kMaximumPdoImages ||
            completedBytes >
                kMaximumPdoImageQueueStorage -
                    std::min(inputImageStorage_,
                             kMaximumPdoImageQueueStorage))) {
      inputImageStorage_ -= inputImages_.front().data.size();
      inputImages_.pop_front();
    }
    if (completedBytes >
        kMaximumPdoImageQueueStorage -
            std::min(inputImageStorage_,
                     kMaximumPdoImageQueueStorage)) {
      assemblyStorage_ -= assembly->data.size() +
                          assembly->received.size();
      assemblies_.erase(assembly);
      return;
    }
    inputImages_.push_back(
        {.busIndex = assembly->busIndex,
         .subDeviceIndex = assembly->subDeviceIndex,
         .epoch = assembly->epoch,
         .cycleSequence = assembly->cycleSequence,
         .data = std::move(assembly->data)});
    inputImageStorage_ += completedBytes;
    assemblyStorage_ -=
        inputImages_.back().data.size() + assembly->received.size();
    assemblies_.erase(assembly);
  }

  [[nodiscard]] bool WaitForSocket(
      short events, std::chrono::milliseconds timeout) {
    pollfd descriptor{
        .fd = descriptor_, .events = events, .revents = 0};
    const int timeoutValue = static_cast<int>(std::clamp<std::int64_t>(
        timeout.count(), 1, std::numeric_limits<int>::max()));
    for (;;) {
      const int result = poll(&descriptor, 1, timeoutValue);
      if (result > 0) {
        return (descriptor.revents & events) != 0 &&
               (descriptor.revents &
                (POLLERR | POLLHUP | POLLNVAL)) == 0;
      }
      if (result < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
  }

  [[nodiscard]] std::uint32_t NextRequestId() {
    if (++nextRequestId_ == 0) {
      ++nextRequestId_;
    }
    return nextRequestId_;
  }

  bool Fail(ClientError error, std::string_view message,
            protocol::AckStatus serverStatus =
                protocol::AckStatus::InternalError) {
    lastError_ = error;
    lastServerStatus_ = serverStatus;
    const std::size_t length =
        std::min(message.size(), lastErrorMessage_.size() - 1U);
    std::copy_n(message.begin(), length, lastErrorMessage_.begin());
    lastErrorMessage_[length] = '\0';
    lastErrorLength_ = length;
    return false;
  }

  void ClearError() {
    lastError_ = ClientError::None;
    lastServerStatus_ = protocol::AckStatus::Ok;
    lastErrorMessage_[0] = '\0';
    lastErrorLength_ = 0;
  }

  void BackgroundLoop() noexcept {
    while (backgroundRunning_.load(std::memory_order_acquire)) {
      if (connected()) {
        static_cast<void>(Poll(std::chrono::milliseconds{10}));
      } else {
        static_cast<void>(Connect());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
  }

  void StopAndDisconnect() noexcept {
    backgroundRunning_.store(false, std::memory_order_release);
    if (background_.joinable() &&
        background_.get_id() != std::this_thread::get_id()) {
      background_.join();
    }
    Disconnect(true);
  }

  ClientOptions options_;
  int descriptor_ = -1;
  bool connected_ = false;
  bool controllerGranted_ = false;
  bool outputsEnabled_ = false;
  bool helloReceived_ = false;
  std::uint32_t expectedHelloRequest_ = 0;
  std::uint32_t nextRequestId_ = 0;
  std::uint64_t epoch_ = 0;
  std::uint64_t lastSeenEpoch_ = 0;
  protocol::StatusPayload status_{};
  protocol::FrameDecoder decoder_;
  std::deque<std::vector<std::uint8_t>> outgoing_;
  std::size_t outgoingOffset_ = 0;
  std::size_t outgoingBytes_ = 0;
  std::deque<Acknowledgement> acknowledgements_;
  std::deque<protocol::PdoInputPayload> inputs_;
  std::deque<PdoAssembly> assemblies_;
  std::size_t assemblyStorage_ = 0;
  std::deque<PdoImage> inputImages_;
  std::size_t inputImageStorage_ = 0;
  std::deque<protocol::BusInfoPayload> busInformation_;
  std::deque<std::uint32_t> heartbeatRequests_;
  std::chrono::steady_clock::time_point nextHeartbeat_{};
  std::chrono::steady_clock::time_point nextReconnect_{};
  std::chrono::milliseconds reconnectBackoff_{50};
  std::chrono::milliseconds effectiveHeartbeatPeriod_{50};
  ClientError lastError_ = ClientError::None;
  protocol::AckStatus lastServerStatus_ = protocol::AckStatus::Ok;
  std::array<char, 256> lastErrorMessage_{};
  std::size_t lastErrorLength_ = 0;
  mutable std::recursive_mutex mutex_;
  std::atomic_bool backgroundRunning_{false};
  std::thread background_;
};

Client::Client(ClientOptions options) noexcept {
  try {
    implementation_ =
        new (std::nothrow) Implementation(std::move(options));
  } catch (...) {
    implementation_ = nullptr;
  }
}

Client::~Client() { delete implementation_; }

Client::Client(Client&& other) noexcept
    : implementation_(std::exchange(other.implementation_, nullptr)) {}

Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    delete implementation_;
    implementation_ =
        std::exchange(other.implementation_, nullptr);
  }
  return *this;
}

bool Client::Connect() noexcept {
  return implementation_ != nullptr && implementation_->Connect();
}
bool Client::MaintainConnection() noexcept {
  return implementation_ != nullptr &&
         implementation_->MaintainConnection();
}
bool Client::Poll(std::chrono::milliseconds timeout) noexcept {
  return implementation_ != nullptr && implementation_->Poll(timeout);
}
void Client::Disconnect() noexcept {
  if (implementation_ != nullptr) {
    implementation_->StopAndDisconnect();
  }
}
bool Client::Heartbeat() noexcept {
  return implementation_ != nullptr &&
         implementation_->Heartbeat();
}
bool Client::EnableOutputs(bool enabled) noexcept {
  return implementation_ != nullptr &&
         implementation_->EnableOutputs(enabled);
}
bool Client::WriteOutput(
    std::uint16_t busIndex, std::uint16_t subDeviceIndex,
    std::uint32_t offset,
    std::span<const std::uint8_t> data) noexcept {
  return implementation_ != nullptr &&
         implementation_->WriteOutput(busIndex, subDeviceIndex, offset,
                                      data);
}
bool Client::ReleaseControl() noexcept {
  return implementation_ != nullptr &&
         implementation_->ReleaseControl();
}
bool Client::SubscribeInputs(bool enabled,
                             std::uint16_t periodMs) noexcept {
  return implementation_ != nullptr &&
         implementation_->SubscribeInputs(enabled, periodMs);
}
bool Client::ClearCounters() noexcept {
  return implementation_ != nullptr &&
         implementation_->EmptyRequest(
             protocol::MessageType::ClearCounters);
}
bool Client::RescanAdapters() noexcept {
  return implementation_ != nullptr &&
         implementation_->EmptyRequest(
             protocol::MessageType::AdapterRescan);
}
bool Client::RuntimeUnlockAdapter(
    std::uint16_t busIndex) noexcept {
  return implementation_ != nullptr &&
         implementation_->RuntimeUnlockAdapter(busIndex);
}
bool Client::PopInput(
    protocol::PdoInputPayload* input) noexcept {
  return implementation_ != nullptr &&
         implementation_->PopInput(input);
}
bool Client::PopInputImage(PdoImage* image) noexcept {
  return implementation_ != nullptr &&
         implementation_->PopInputImage(image);
}
bool Client::PopBusInfo(
    protocol::BusInfoPayload* information) noexcept {
  return implementation_ != nullptr &&
         implementation_->PopBusInfo(information);
}
bool Client::connected() const noexcept {
  return implementation_ != nullptr &&
         implementation_->connected();
}
bool Client::controllerGranted() const noexcept {
  return implementation_ != nullptr &&
         implementation_->controllerGranted();
}
bool Client::outputsEnabled() const noexcept {
  return implementation_ != nullptr &&
         implementation_->outputsEnabled();
}
std::uint64_t Client::epoch() const noexcept {
  return implementation_ != nullptr ? implementation_->epoch() : 0;
}
protocol::StatusPayload Client::status() const noexcept {
  return implementation_ != nullptr ? implementation_->status()
                                    : protocol::StatusPayload{};
}
ClientError Client::lastError() const noexcept {
  return implementation_ != nullptr
             ? implementation_->lastError()
             : ClientError::OutOfMemory;
}
protocol::AckStatus Client::lastServerStatus() const noexcept {
  return implementation_ != nullptr
             ? implementation_->lastServerStatus()
             : protocol::AckStatus::InternalError;
}
ClientErrorSnapshot Client::errorSnapshot() const noexcept {
  return implementation_ != nullptr
             ? implementation_->errorSnapshot()
             : ClientErrorSnapshot{
                   .error = ClientError::OutOfMemory,
                   .serverStatus =
                       protocol::AckStatus::InternalError,
                   .message = {'c', 'l', 'i', 'e', 'n', 't', ' ',
                               'a', 'l', 'l', 'o', 'c', 'a', 't',
                               'i', 'o', 'n', ' ', 'f', 'a', 'i',
                               'l', 'e', 'd', '\0'}};
}
}  // namespace ec_systemcore
