#include "ec_systemcore/client.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace ec_systemcore;
using namespace ec_systemcore::protocol;

class Connection {
 public:
  explicit Connection(int descriptor) : descriptor_(descriptor) {}
  ~Connection() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  Frame Receive(std::chrono::milliseconds timeout) {
    if (!pending_.empty()) {
      Frame frame = std::move(pending_.front());
      pending_.pop_front();
      return frame;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd descriptor{
          .fd = descriptor_, .events = POLLIN, .revents = 0};
      const int wait = static_cast<int>(
          std::max<std::int64_t>(
              1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - std::chrono::steady_clock::now())
                  .count()));
      const int result = poll(&descriptor, 1, wait);
      if (result <= 0) {
        continue;
      }
      if ((descriptor.revents &
           (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("connection closed");
      }
      std::array<std::uint8_t,
                 kLengthPrefixSize + kMaximumFrameLength>
          bytes{};
      const ssize_t count =
          recv(descriptor_, bytes.data(), bytes.size(), 0);
      if (count <= 0) {
        throw std::runtime_error("connection closed");
      }
      std::vector<Frame> frames;
      if (!decoder_.Feed(
              std::span<const std::uint8_t>{
                  bytes.data(), static_cast<std::size_t>(count)},
              &frames)) {
        throw std::runtime_error("invalid client frame");
      }
      for (Frame& frame : frames) {
        pending_.push_back(std::move(frame));
      }
      if (!pending_.empty()) {
        Frame frame = std::move(pending_.front());
        pending_.pop_front();
        return frame;
      }
    }
    throw std::runtime_error("receive timed out");
  }

  bool TryReceive(Frame* frame, std::chrono::milliseconds timeout) {
    try {
      *frame = Receive(timeout);
      return true;
    } catch (const std::runtime_error& error) {
      if (std::string{error.what()} == "receive timed out") {
        return false;
      }
      throw;
    }
  }

  void Send(const std::vector<std::uint8_t>& frame) {
    std::size_t offset = 0;
    while (offset < frame.size()) {
      const ssize_t count =
          send(descriptor_, frame.data() + offset,
               frame.size() - offset, MSG_NOSIGNAL);
      if (count <= 0) {
        throw std::runtime_error("send failed");
      }
      offset += static_cast<std::size_t>(count);
    }
  }

  void Ack(const Frame& request,
           AckStatus status = AckStatus::Ok) {
    Send(EncodeFrame(MessageType::Ack, request.requestId,
                     EncodeAckPayload(status)));
  }

  bool WaitForClose(std::chrono::milliseconds timeout) {
    pollfd descriptor{
        .fd = descriptor_,
        .events = static_cast<short>(POLLIN | POLLHUP),
        .revents = 0};
    const int result =
        poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (result <= 0) {
      return false;
    }
    if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
      return true;
    }
    std::uint8_t byte = 0;
    return recv(descriptor_, &byte, 1, MSG_DONTWAIT) == 0;
  }

 private:
  int descriptor_ = -1;
  FrameDecoder decoder_;
  std::deque<Frame> pending_;
};

int Accept(int listener, std::chrono::milliseconds timeout) {
  pollfd descriptor{.fd = listener, .events = POLLIN, .revents = 0};
  if (poll(&descriptor, 1, static_cast<int>(timeout.count())) <= 0) {
    throw std::runtime_error("accept timed out");
  }
  const int accepted = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
  if (accepted < 0) {
    throw std::runtime_error("accept failed");
  }
  return accepted;
}

template <typename Predicate>
bool WaitUntil(Predicate&& predicate,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

}  // namespace

int main() {
  std::filesystem::path socketPath =
      "/tmp/ec-systemcore-client-test-" +
      std::to_string(getpid()) + ".sock";
  int listener = -1;
  std::thread server;
  try {
    listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
      throw std::runtime_error("socket failed");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = socketPath.string();
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    unlink(path.c_str());
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
      throw std::runtime_error("listen failed");
    }

    std::atomic_bool firstWriteSeen{false};
    std::atomic_bool secondHelloSeen{false};
    std::atomic_bool allowFreshCommands{false};
    std::atomic_bool secondWriteSeen{false};
    std::atomic_bool serverFailed{false};
    std::string serverError;

    server = std::thread([&] {
      try {
        {
          Connection connection{Accept(listener, 2s)};
          const Frame helloFrame = connection.Receive(1s);
          if (helloFrame.type != MessageType::Hello) {
            throw std::runtime_error("first frame was not HELLO");
          }
          HelloPayload hello;
          if (!DecodeHelloPayload(helloFrame.payload, &hello) ||
              hello.lastSeenEpoch != 0) {
            throw std::runtime_error("invalid first HELLO");
          }
          connection.Send(EncodeFrame(
              MessageType::HelloAck, helloFrame.requestId,
              EncodeHelloAckPayload(
                  {.grantedRole = ClientRole::Controller,
                   .outputsEnabled = false,
                   .heartbeatTimeoutMs = 200,
                   .epoch = 100,
                   .peerPid = 1,
                   .capabilities = CapabilityOutputs |
                                   CapabilityInputs})));
          for (;;) {
            const Frame request = connection.Receive(1s);
            if (request.type == MessageType::Heartbeat) {
              connection.Ack(request);
            } else if (request.type == MessageType::OutputEnable) {
              connection.Ack(request);
            } else if (request.type == MessageType::OutputWrite) {
              OutputWritePayload write;
              if (!DecodeOutputWritePayload(request.payload, &write) ||
                  write.data !=
                      std::vector<std::uint8_t>({1, 2, 3})) {
                throw std::runtime_error("unexpected first write");
              }
              connection.Ack(request);
              firstWriteSeen.store(true, std::memory_order_release);
              // Let the nonblocking client consume the ACK before simulating
              // an abrupt transport loss.
              std::this_thread::sleep_for(50ms);
              break;
            } else {
              throw std::runtime_error("unexpected first-session frame");
            }
          }
        }

        Connection connection{Accept(listener, 3s)};
        const Frame helloFrame = connection.Receive(1s);
        HelloPayload hello;
        if (helloFrame.type != MessageType::Hello ||
            !DecodeHelloPayload(helloFrame.payload, &hello) ||
            hello.lastSeenEpoch != 100) {
          throw std::runtime_error(
              "reconnect did not report the prior epoch");
        }
        connection.Send(EncodeFrame(
            MessageType::HelloAck, helloFrame.requestId,
            EncodeHelloAckPayload(
                {.grantedRole = ClientRole::Controller,
                 .outputsEnabled = false,
                 .heartbeatTimeoutMs = 200,
                 .epoch = 200,
                 .peerPid = 1,
                 .capabilities =
                     CapabilityOutputs | CapabilityInputs})));
        secondHelloSeen.store(true, std::memory_order_release);

        while (!secondWriteSeen.load(std::memory_order_acquire)) {
          const Frame request = connection.Receive(1s);
          if (request.type == MessageType::Heartbeat) {
            connection.Ack(request);
            continue;
          }
          if (!allowFreshCommands.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "stale command replayed after reconnect");
          }
          if (request.type == MessageType::OutputEnable) {
            OutputEnablePayload enable;
            if (!DecodeOutputEnablePayload(request.payload, &enable) ||
                enable.epoch != 200 || !enable.enabled) {
              throw std::runtime_error("invalid fresh enable");
            }
            connection.Ack(request);
          } else if (request.type == MessageType::OutputWrite) {
            OutputWritePayload write;
            if (!DecodeOutputWritePayload(request.payload, &write) ||
                write.epoch != 200 ||
                write.data !=
                    std::vector<std::uint8_t>({9, 8, 7})) {
              throw std::runtime_error("invalid fresh write");
            }
            connection.Ack(request);
            secondWriteSeen.store(true, std::memory_order_release);

            connection.Send(EncodeFrame(
                MessageType::PdoInput, 0,
                EncodePdoInputPayload(
                    {.busIndex = 0,
                     .subDeviceIndex = 1,
                     .offset = 0,
                     .totalSize = 6,
                     .epoch = 200,
                     .cycleSequence = 10,
                     .data = {'a', 'b', 'c'}})));
            connection.Send(EncodeFrame(
                MessageType::PdoInput, 0,
                EncodePdoInputPayload(
                    {.busIndex = 0,
                     .subDeviceIndex = 1,
                     .offset = 3,
                     .totalSize = 6,
                     .epoch = 200,
                     .cycleSequence = 10,
                     .data = {'d', 'e', 'f'}})));
          } else {
            throw std::runtime_error(
                "unexpected second-session frame");
          }
        }
        for (;;) {
          const Frame request = connection.Receive(1s);
          if (request.type == MessageType::Heartbeat) {
            connection.Ack(request);
            continue;
          }
          if (request.type != MessageType::ClearCounters ||
              !request.payload.empty()) {
            throw std::runtime_error(
                "unexpected request before timeout test");
          }
          // Deliberately leave a state-changing request unacknowledged.
          // The client must close the uncertain session at its deadline,
          // rather than retaining or replaying the request later.
          if (!connection.WaitForClose(1s)) {
            throw std::runtime_error(
                "timed-out request did not close the session");
          }
          break;
        }
      } catch (const std::exception& error) {
        serverError = error.what();
        serverFailed.store(true, std::memory_order_release);
      }
    });

    Client client{ClientOptions{
        .socketPath = path,
        .role = ClientRole::Controller,
        .subscribeInputs = true,
        .inputPeriodMs = 10,
        .connectTimeout = 200ms,
        .requestTimeout = 500ms,
        .initialReconnectBackoff = 10ms,
        .maximumReconnectBackoff = 50ms,
        .heartbeatPeriod = 25ms,
        .backgroundReconnect = true,
    }};
    if (!WaitUntil(
            [&] {
              return client.connected() &&
                     client.controllerGranted() &&
                     client.epoch() == 100;
            },
            2s)) {
      throw std::runtime_error("client did not establish first session");
    }
    if (!client.EnableOutputs(true) ||
        !client.WriteOutput(0, 1, 0,
                            std::array<std::uint8_t, 3>{1, 2, 3})) {
      throw std::runtime_error("first output sequence failed");
    }
    if (!WaitUntil(
            [&] {
              return firstWriteSeen.load(std::memory_order_acquire);
            },
            1s)) {
      throw std::runtime_error("server did not receive first write");
    }
    if (!WaitUntil(
            [&] {
              return secondHelloSeen.load(std::memory_order_acquire) &&
                     client.connected() &&
                     client.controllerGranted() &&
                     client.epoch() == 200;
            },
            3s)) {
      throw std::runtime_error("client did not reconnect");
    }
    if (client.outputsEnabled()) {
      throw std::runtime_error("outputs resumed across reconnect");
    }
    std::this_thread::sleep_for(100ms);
    allowFreshCommands.store(true, std::memory_order_release);
    if (!client.EnableOutputs(true) ||
        !client.WriteOutput(0, 1, 0,
                            std::array<std::uint8_t, 3>{9, 8, 7})) {
      throw std::runtime_error("fresh output sequence failed");
    }
    if (!WaitUntil(
            [&] {
              return secondWriteSeen.load(std::memory_order_acquire);
            },
            1s)) {
      throw std::runtime_error("server did not receive fresh write");
    }
    PdoImage image;
    if (!WaitUntil([&] { return client.PopInputImage(&image); }, 1s) ||
        image.data !=
            std::vector<std::uint8_t>({'a', 'b', 'c', 'd', 'e', 'f'})) {
      throw std::runtime_error("PDO reassembly failed");
    }
    const bool clearSucceeded = client.ClearCounters();
    const ClientError timeoutError = client.lastError();
    const bool connectedAfterTimeout = client.connected();
    if (clearSucceeded || timeoutError != ClientError::Timeout ||
        connectedAfterTimeout) {
      throw std::runtime_error(
          "uncertain timed-out request was not invalidated: success=" +
          std::to_string(clearSucceeded) + ", error=" +
          std::to_string(static_cast<int>(timeoutError)) +
          ", connected=" + std::to_string(connectedAfterTimeout));
    }
    client.Disconnect();
    if (server.joinable()) {
      server.join();
    }
    close(listener);
    unlink(path.c_str());
    if (serverFailed.load(std::memory_order_acquire)) {
      throw std::runtime_error(serverError);
    }
    std::cout << "client reconnect test passed\n";
    return 0;
  } catch (const std::exception& error) {
    if (listener >= 0) {
      close(listener);
    }
    unlink(socketPath.c_str());
    if (server.joinable()) {
      server.join();
    }
    std::cerr << error.what() << '\n';
    return 1;
  }
}
