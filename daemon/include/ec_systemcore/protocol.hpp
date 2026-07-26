#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ec_systemcore::protocol {

inline constexpr std::array<std::uint8_t, 4> kMagic{'E', 'C', 'S', 'C'};
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kEnvelopeSize = 12;
inline constexpr std::size_t kLengthPrefixSize = 4;
inline constexpr std::size_t kMinimumFrameLength = kEnvelopeSize;
inline constexpr std::size_t kMaximumFrameLength = 8192;
inline constexpr std::size_t kMaximumBufferedInput =
    2U * (kLengthPrefixSize + kMaximumFrameLength);
inline constexpr std::size_t kMaximumOutputWrite = 1024;
inline constexpr std::size_t kMaximumPdoChunk = 1024;
inline constexpr std::size_t kMaximumPdoImage = 1024U * 1024U;
inline constexpr std::size_t kMaximumBusInfo = 2048;

enum class MessageType : std::uint8_t {
  Hello = 0x01,
  Heartbeat = 0x02,
  OutputEnable = 0x03,
  OutputWrite = 0x04,
  ClearCounters = 0x05,
  ReleaseControl = 0x06,
  SubscribeInputs = 0x07,
  AdapterUnlock = 0x08,
  AdapterRescan = 0x09,

  HelloAck = 0x80,
  Status = 0x81,
  Ack = 0x82,
  PdoInput = 0x83,
  BusInfo = 0x84,
  Error = 0x85,
  OutputsDisabled = 0x86,
};

enum class ClientRole : std::uint8_t {
  Observer = 0,
  Controller = 1,
};

enum class AckStatus : std::uint16_t {
  Ok = 0,
  Malformed = 1,
  Unauthorized = 2,
  Busy = 3,
  Disabled = 4,
  BadTarget = 5,
  Bounds = 6,
  StaleEpoch = 7,
  Unsupported = 8,
  AdapterLocked = 9,
  TopologyMismatch = 10,
  QueueFull = 11,
  InternalError = 12,
};

enum class DisableReason : std::uint16_t {
  Requested = 1,
  HeartbeatTimeout = 2,
  Disconnect = 3,
  LinkLoss = 4,
  Reinitialize = 5,
  Shutdown = 6,
  EpochChange = 7,
  AdapterMismatch = 8,
  ProcessDataFault = 9,
  OutputCommandTimeout = 10,
};

enum class AdapterLockState : std::uint8_t {
  Unlocked = 0,
  Matched = 1,
  Missing = 2,
  Mismatch = 3,
  RuntimeUnlocked = 4,
};

enum class AdapterLockReason : std::uint8_t {
  None = 0,
  InterfaceMissing = 1,
  IdentityUnavailable = 2,
  IdPathMismatch = 3,
  PermanentMacMismatch = 4,
  UsbSerialMismatch = 5,
  UsbVendorMismatch = 6,
  UsbProductMismatch = 7,
  NonEthernetInterface = 8,
  PathOnlyIdentity = 9,
};

enum class SchedulingMode : std::uint8_t {
  Other = 0,
  Fifo = 1,
};

enum class BusState : std::uint8_t {
  Starting = 0,
  WaitingForLink = 1,
  Initializing = 2,
  SafeOperational = 3,
  Operational = 4,
  Fault = 5,
  Stopping = 6,
};

[[nodiscard]] inline constexpr bool IsKnownMessageType(MessageType type) {
  switch (type) {
    case MessageType::Hello:
    case MessageType::Heartbeat:
    case MessageType::OutputEnable:
    case MessageType::OutputWrite:
    case MessageType::ClearCounters:
    case MessageType::ReleaseControl:
    case MessageType::SubscribeInputs:
    case MessageType::AdapterUnlock:
    case MessageType::AdapterRescan:
    case MessageType::HelloAck:
    case MessageType::Status:
    case MessageType::Ack:
    case MessageType::PdoInput:
    case MessageType::BusInfo:
    case MessageType::Error:
    case MessageType::OutputsDisabled:
      return true;
  }
  return false;
}

[[nodiscard]] inline constexpr bool IsKnownAckStatus(AckStatus status) {
  switch (status) {
    case AckStatus::Ok:
    case AckStatus::Malformed:
    case AckStatus::Unauthorized:
    case AckStatus::Busy:
    case AckStatus::Disabled:
    case AckStatus::BadTarget:
    case AckStatus::Bounds:
    case AckStatus::StaleEpoch:
    case AckStatus::Unsupported:
    case AckStatus::AdapterLocked:
    case AckStatus::TopologyMismatch:
    case AckStatus::QueueFull:
    case AckStatus::InternalError:
      return true;
  }
  return false;
}

[[nodiscard]] inline constexpr bool IsKnownBusState(BusState state) {
  switch (state) {
    case BusState::Starting:
    case BusState::WaitingForLink:
    case BusState::Initializing:
    case BusState::SafeOperational:
    case BusState::Operational:
    case BusState::Fault:
    case BusState::Stopping:
      return true;
  }
  return false;
}

[[nodiscard]] inline constexpr bool IsKnownSchedulingMode(
    SchedulingMode mode) {
  return mode == SchedulingMode::Other ||
         mode == SchedulingMode::Fifo;
}

enum StatusFlag : std::uint8_t {
  StatusOperational = 1U << 0U,
  StatusOutputsEnabled = 1U << 1U,
  StatusControllerConnected = 1U << 2U,
  StatusPreemptRtAvailable = 1U << 3U,
  StatusRealtimeApplied = 1U << 4U,
  StatusRealtimeRequested = 1U << 5U,
  StatusTimingDegraded = 1U << 6U,
  StatusReinitializing = 1U << 7U,
};

enum Capability : std::uint32_t {
  CapabilityOutputs = 1U << 0U,
  CapabilityInputs = 1U << 1U,
  CapabilityAdapterIdentity = 1U << 2U,
  CapabilityDistributedClock = 1U << 3U,
};

struct Frame {
  MessageType type = MessageType::Error;
  std::uint32_t requestId = 0;
  std::vector<std::uint8_t> payload;
};

inline void AppendLe16(std::vector<std::uint8_t>& destination,
                       std::uint16_t value);
inline void AppendLe32(std::vector<std::uint8_t>& destination,
                       std::uint32_t value);
inline void AppendLe64(std::vector<std::uint8_t>& destination,
                       std::uint64_t value);
inline bool ReadLe16(std::span<const std::uint8_t> bytes,
                     std::size_t offset, std::uint16_t* value);
inline bool ReadLe32(std::span<const std::uint8_t> bytes,
                     std::size_t offset, std::uint32_t* value);
inline bool ReadLe64(std::span<const std::uint8_t> bytes,
                     std::size_t offset, std::uint64_t* value);

struct HelloPayload {
  ClientRole role = ClientRole::Observer;
  bool subscribeInputs = false;
  std::uint16_t inputPeriodMs = 100;
  std::uint64_t lastSeenEpoch = 0;
};

inline constexpr std::size_t kHelloPayloadSize = 12;

inline std::vector<std::uint8_t> EncodeHelloPayload(
    const HelloPayload& hello) {
  if ((hello.role != ClientRole::Observer &&
       hello.role != ClientRole::Controller) ||
      hello.inputPeriodMs < 10 || hello.inputPeriodMs > 1000) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(kHelloPayloadSize);
  payload.push_back(static_cast<std::uint8_t>(hello.role));
  payload.push_back(hello.subscribeInputs ? 1U : 0U);
  AppendLe16(payload, hello.inputPeriodMs);
  AppendLe64(payload, hello.lastSeenEpoch);
  return payload;
}

inline bool DecodeHelloPayload(std::span<const std::uint8_t> payload,
                               HelloPayload* hello) {
  std::uint16_t period = 0;
  std::uint64_t epoch = 0;
  if (hello == nullptr || payload.size() != kHelloPayloadSize ||
      payload[0] > static_cast<std::uint8_t>(ClientRole::Controller) ||
      payload[1] > 1 || !ReadLe16(payload, 2, &period) ||
      period < 10 || period > 1000 || !ReadLe64(payload, 4, &epoch)) {
    return false;
  }
  hello->role = static_cast<ClientRole>(payload[0]);
  hello->subscribeInputs = payload[1] != 0;
  hello->inputPeriodMs = period;
  hello->lastSeenEpoch = epoch;
  return true;
}

struct HelloAckPayload {
  ClientRole grantedRole = ClientRole::Observer;
  bool outputsEnabled = false;
  std::uint16_t heartbeatTimeoutMs = 0;
  std::uint64_t epoch = 0;
  std::uint32_t peerPid = 0;
  std::uint32_t capabilities = 0;
};

inline constexpr std::size_t kHelloAckPayloadSize = 20;

inline std::vector<std::uint8_t> EncodeHelloAckPayload(
    const HelloAckPayload& hello) {
  if ((hello.grantedRole != ClientRole::Observer &&
       hello.grantedRole != ClientRole::Controller) ||
      hello.heartbeatTimeoutMs == 0 || hello.epoch == 0) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(kHelloAckPayloadSize);
  payload.push_back(static_cast<std::uint8_t>(hello.grantedRole));
  payload.push_back(hello.outputsEnabled ? 1U : 0U);
  AppendLe16(payload, hello.heartbeatTimeoutMs);
  AppendLe64(payload, hello.epoch);
  AppendLe32(payload, hello.peerPid);
  AppendLe32(payload, hello.capabilities);
  return payload;
}

inline bool DecodeHelloAckPayload(std::span<const std::uint8_t> payload,
                                  HelloAckPayload* hello) {
  if (hello == nullptr || payload.size() != kHelloAckPayloadSize ||
      payload[0] > static_cast<std::uint8_t>(ClientRole::Controller) ||
      payload[1] > 1 ||
      !ReadLe16(payload, 2, &hello->heartbeatTimeoutMs) ||
      hello->heartbeatTimeoutMs == 0 ||
      !ReadLe64(payload, 4, &hello->epoch) || hello->epoch == 0 ||
      !ReadLe32(payload, 12, &hello->peerPid) ||
      !ReadLe32(payload, 16, &hello->capabilities)) {
    return false;
  }
  hello->grantedRole = static_cast<ClientRole>(payload[0]);
  hello->outputsEnabled = payload[1] != 0;
  return true;
}

inline std::vector<std::uint8_t> EncodeEpochPayload(std::uint64_t epoch) {
  if (epoch == 0) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(8);
  AppendLe64(payload, epoch);
  return payload;
}

inline bool DecodeEpochPayload(std::span<const std::uint8_t> payload,
                               std::uint64_t* epoch) {
  return epoch != nullptr && payload.size() == 8 &&
         ReadLe64(payload, 0, epoch) && *epoch != 0;
}

struct OutputEnablePayload {
  std::uint64_t epoch = 0;
  bool enabled = false;
};

inline std::vector<std::uint8_t> EncodeOutputEnablePayload(
    const OutputEnablePayload& enable) {
  std::vector<std::uint8_t> payload = EncodeEpochPayload(enable.epoch);
  if (payload.empty()) {
    return {};
  }
  payload.push_back(enable.enabled ? 1U : 0U);
  return payload;
}

inline bool DecodeOutputEnablePayload(std::span<const std::uint8_t> payload,
                                      OutputEnablePayload* enable) {
  return enable != nullptr && payload.size() == 9 && payload[8] <= 1 &&
         ReadLe64(payload, 0, &enable->epoch) && enable->epoch != 0 &&
         ((enable->enabled = payload[8] != 0), true);
}

struct OutputWritePayload {
  std::uint64_t epoch = 0;
  std::uint16_t busIndex = 0;
  std::uint16_t subDeviceIndex = 0;
  std::uint32_t offset = 0;
  std::vector<std::uint8_t> data;
};

inline std::vector<std::uint8_t> EncodeOutputWritePayload(
    const OutputWritePayload& write) {
  if (write.epoch == 0 || write.subDeviceIndex == 0 ||
      write.data.empty() || write.data.size() > kMaximumOutputWrite) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(20 + write.data.size());
  AppendLe64(payload, write.epoch);
  AppendLe16(payload, write.busIndex);
  AppendLe16(payload, write.subDeviceIndex);
  AppendLe32(payload, write.offset);
  AppendLe16(payload, static_cast<std::uint16_t>(write.data.size()));
  AppendLe16(payload, 0);
  payload.insert(payload.end(), write.data.begin(), write.data.end());
  return payload;
}

inline bool DecodeOutputWritePayload(std::span<const std::uint8_t> payload,
                                     OutputWritePayload* write) {
  std::uint16_t length = 0;
  std::uint16_t reserved = 0;
  if (write == nullptr || payload.size() < 20 ||
      !ReadLe64(payload, 0, &write->epoch) || write->epoch == 0 ||
      !ReadLe16(payload, 8, &write->busIndex) ||
      !ReadLe16(payload, 10, &write->subDeviceIndex) ||
      write->subDeviceIndex == 0 ||
      !ReadLe32(payload, 12, &write->offset) ||
      !ReadLe16(payload, 16, &length) || length == 0 ||
      length > kMaximumOutputWrite ||
      !ReadLe16(payload, 18, &reserved) || reserved != 0 ||
      payload.size() != 20U + static_cast<std::size_t>(length)) {
    return false;
  }
  write->data.assign(payload.begin() + 20, payload.end());
  return true;
}

struct SubscribeInputsPayload {
  bool enabled = false;
  std::uint16_t periodMs = 100;
};

inline std::vector<std::uint8_t> EncodeSubscribeInputsPayload(
    const SubscribeInputsPayload& subscription) {
  if (subscription.periodMs < 10 || subscription.periodMs > 1000) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(4);
  payload.push_back(subscription.enabled ? 1U : 0U);
  payload.push_back(0);
  AppendLe16(payload, subscription.periodMs);
  return payload;
}

inline bool DecodeSubscribeInputsPayload(
    std::span<const std::uint8_t> payload,
    SubscribeInputsPayload* subscription) {
  return subscription != nullptr && payload.size() == 4 &&
         payload[0] <= 1 && payload[1] == 0 &&
         ReadLe16(payload, 2, &subscription->periodMs) &&
         subscription->periodMs >= 10 &&
         subscription->periodMs <= 1000 &&
         ((subscription->enabled = payload[0] != 0), true);
}

struct AdapterUnlockPayload {
  std::uint64_t epoch = 0;
  std::uint16_t busIndex = 0;
};

inline std::vector<std::uint8_t> EncodeAdapterUnlockPayload(
    const AdapterUnlockPayload& unlock) {
  std::vector<std::uint8_t> payload = EncodeEpochPayload(unlock.epoch);
  if (payload.empty()) {
    return {};
  }
  AppendLe16(payload, unlock.busIndex);
  return payload;
}

inline bool DecodeAdapterUnlockPayload(std::span<const std::uint8_t> payload,
                                       AdapterUnlockPayload* unlock) {
  return unlock != nullptr && payload.size() == 10 &&
         ReadLe64(payload, 0, &unlock->epoch) && unlock->epoch != 0 &&
         ReadLe16(payload, 8, &unlock->busIndex);
}

struct OutputsDisabledPayload {
  DisableReason reason = DisableReason::Requested;
  std::uint16_t busIndex = 0xffffU;
  std::uint64_t epoch = 0;
};

inline std::vector<std::uint8_t> EncodeOutputsDisabledPayload(
    const OutputsDisabledPayload& disabled) {
  if (disabled.epoch == 0) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(12);
  AppendLe16(payload, static_cast<std::uint16_t>(disabled.reason));
  AppendLe16(payload, disabled.busIndex);
  AppendLe64(payload, disabled.epoch);
  return payload;
}

inline bool DecodeOutputsDisabledPayload(
    std::span<const std::uint8_t> payload,
    OutputsDisabledPayload* disabled) {
  std::uint16_t reason = 0;
  if (disabled == nullptr || payload.size() != 12 ||
      !ReadLe16(payload, 0, &reason) ||
      reason < static_cast<std::uint16_t>(DisableReason::Requested) ||
      reason >
          static_cast<std::uint16_t>(
              DisableReason::OutputCommandTimeout) ||
      !ReadLe16(payload, 2, &disabled->busIndex) ||
      !ReadLe64(payload, 4, &disabled->epoch) || disabled->epoch == 0) {
    return false;
  }
  disabled->reason = static_cast<DisableReason>(reason);
  return true;
}

inline void AppendLe16(std::vector<std::uint8_t>& destination,
                       std::uint16_t value) {
  destination.push_back(static_cast<std::uint8_t>(value & 0xffU));
  destination.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

inline void AppendLe32(std::vector<std::uint8_t>& destination,
                       std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    destination.push_back(
        static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

inline void AppendLe64(std::vector<std::uint8_t>& destination,
                       std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64U; shift += 8U) {
    destination.push_back(
        static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

inline bool ReadLe16(std::span<const std::uint8_t> bytes, std::size_t offset,
                     std::uint16_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < sizeof(std::uint16_t)) {
    return false;
  }
  *value = static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
  return true;
}

inline bool ReadLe32(std::span<const std::uint8_t> bytes, std::size_t offset,
                     std::uint32_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < sizeof(std::uint32_t)) {
    return false;
  }
  std::uint32_t decoded = 0;
  for (unsigned int index = 0; index < 4U; ++index) {
    decoded |= static_cast<std::uint32_t>(bytes[offset + index])
               << (index * 8U);
  }
  *value = decoded;
  return true;
}

inline bool ReadLe64(std::span<const std::uint8_t> bytes, std::size_t offset,
                     std::uint64_t* value) {
  if (value == nullptr || offset > bytes.size() ||
      bytes.size() - offset < sizeof(std::uint64_t)) {
    return false;
  }
  std::uint64_t decoded = 0;
  for (unsigned int index = 0; index < 8U; ++index) {
    decoded |= static_cast<std::uint64_t>(bytes[offset + index])
               << (index * 8U);
  }
  *value = decoded;
  return true;
}

inline std::vector<std::uint8_t> EncodeFrame(
    MessageType type, std::uint32_t requestId,
    std::span<const std::uint8_t> payload = {}) {
  const std::size_t frameLength = kEnvelopeSize + payload.size();
  if (!IsKnownMessageType(type) || frameLength > kMaximumFrameLength) {
    return {};
  }

  std::vector<std::uint8_t> encoded;
  encoded.reserve(kLengthPrefixSize + frameLength);
  AppendLe32(encoded, static_cast<std::uint32_t>(frameLength));
  encoded.insert(encoded.end(), kMagic.begin(), kMagic.end());
  encoded.push_back(kVersion);
  encoded.push_back(static_cast<std::uint8_t>(type));
  AppendLe16(encoded, 0);
  AppendLe32(encoded, requestId);
  encoded.insert(encoded.end(), payload.begin(), payload.end());
  return encoded;
}

class FrameDecoder final {
 public:
  enum class Error {
    None,
    BufferLimit,
    InvalidLength,
    InvalidMagic,
    UnsupportedVersion,
    ReservedFlags,
    InvalidMessageType,
  };

  bool Feed(std::span<const std::uint8_t> bytes,
            std::vector<Frame>* decodedFrames) {
    if (decodedFrames == nullptr || error_ != Error::None) {
      return false;
    }
    if (bytes.size() > kMaximumBufferedInput ||
        buffer_.size() > kMaximumBufferedInput - bytes.size()) {
      error_ = Error::BufferLimit;
      return false;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    std::size_t parseOffset = 0;
    while (buffer_.size() - parseOffset >= kLengthPrefixSize) {
      std::uint32_t frameLength = 0;
      if (!ReadLe32(buffer_, parseOffset, &frameLength)) {
        error_ = Error::InvalidLength;
        return false;
      }
      if (frameLength < kMinimumFrameLength ||
          frameLength > kMaximumFrameLength) {
        error_ = Error::InvalidLength;
        return false;
      }

      const std::size_t totalLength =
          kLengthPrefixSize + static_cast<std::size_t>(frameLength);
      if (buffer_.size() - parseOffset < totalLength) {
        break;
      }

      if (!std::equal(kMagic.begin(), kMagic.end(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(parseOffset) +
                          static_cast<std::ptrdiff_t>(kLengthPrefixSize))) {
        error_ = Error::InvalidMagic;
        return false;
      }
      if (buffer_[parseOffset + 8U] != kVersion) {
        error_ = Error::UnsupportedVersion;
        return false;
      }
      std::uint16_t flags = 0;
      if (!ReadLe16(buffer_, parseOffset + 10U, &flags) || flags != 0) {
        error_ = Error::ReservedFlags;
        return false;
      }
      std::uint32_t requestId = 0;
      if (!ReadLe32(buffer_, parseOffset + 12U, &requestId)) {
        error_ = Error::InvalidLength;
        return false;
      }

      Frame frame;
      frame.type = static_cast<MessageType>(buffer_[parseOffset + 9U]);
      if (!IsKnownMessageType(frame.type)) {
        error_ = Error::InvalidMessageType;
        return false;
      }
      frame.requestId = requestId;
      frame.payload.assign(
          buffer_.begin() + static_cast<std::ptrdiff_t>(
                                parseOffset + kLengthPrefixSize + kEnvelopeSize),
          buffer_.begin() +
              static_cast<std::ptrdiff_t>(parseOffset + totalLength));
      decodedFrames->emplace_back(std::move(frame));
      parseOffset += totalLength;
    }
    if (parseOffset != 0) {
      buffer_.erase(
          buffer_.begin(),
          buffer_.begin() + static_cast<std::ptrdiff_t>(parseOffset));
    }
    return true;
  }

  [[nodiscard]] Error error() const { return error_; }
  [[nodiscard]] std::size_t bufferedBytes() const { return buffer_.size(); }

  void Reset() {
    buffer_.clear();
    error_ = Error::None;
  }

 private:
  std::vector<std::uint8_t> buffer_;
  Error error_ = Error::None;
};

struct StatusPayload {
  std::uint8_t aggregateState = 0;
  std::uint8_t flags = 0;
  std::uint16_t activeAdapters = 0;
  std::uint16_t subDevices = 0;
  std::uint16_t activeFaults = 0;
  std::uint32_t maximumJitterUs = 0;
  std::uint32_t currentJitterUs = 0;
  std::uint64_t lostFrames = 0;
  std::uint64_t cycleOverruns = 0;
  std::uint64_t epoch = 0;
  std::uint32_t controllerPid = 0;
  std::uint16_t configuredBuses = 0;
  SchedulingMode schedulingMode = SchedulingMode::Other;
  std::uint8_t realtimeErrorBits = 0;
  std::uint64_t monotonicTimestampUs = 0;
};

inline constexpr std::size_t kStatusPayloadSize = 56;

inline std::vector<std::uint8_t> EncodeStatusPayload(
    const StatusPayload& status) {
  if (!IsKnownBusState(
          static_cast<BusState>(status.aggregateState)) ||
      status.epoch == 0 ||
      !IsKnownSchedulingMode(status.schedulingMode) ||
      (status.realtimeErrorBits & 0x80U) != 0) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(kStatusPayloadSize);
  payload.push_back(status.aggregateState);
  payload.push_back(status.flags);
  AppendLe16(payload, status.activeAdapters);
  AppendLe16(payload, status.subDevices);
  AppendLe16(payload, status.activeFaults);
  AppendLe32(payload, status.maximumJitterUs);
  AppendLe32(payload, status.currentJitterUs);
  AppendLe64(payload, status.lostFrames);
  AppendLe64(payload, status.cycleOverruns);
  AppendLe64(payload, status.epoch);
  AppendLe32(payload, status.controllerPid);
  AppendLe16(payload, status.configuredBuses);
  payload.push_back(static_cast<std::uint8_t>(status.schedulingMode));
  payload.push_back(status.realtimeErrorBits);
  AppendLe64(payload, status.monotonicTimestampUs);
  return payload;
}

inline bool DecodeStatusPayload(std::span<const std::uint8_t> payload,
                                StatusPayload* status) {
  if (status == nullptr || payload.size() != kStatusPayloadSize) {
    return false;
  }
  status->aggregateState = payload[0];
  status->flags = payload[1];
  if (!ReadLe16(payload, 2, &status->activeAdapters) ||
      !ReadLe16(payload, 4, &status->subDevices) ||
      !ReadLe16(payload, 6, &status->activeFaults) ||
      !ReadLe32(payload, 8, &status->maximumJitterUs) ||
      !ReadLe32(payload, 12, &status->currentJitterUs) ||
      !ReadLe64(payload, 16, &status->lostFrames) ||
      !ReadLe64(payload, 24, &status->cycleOverruns) ||
      !ReadLe64(payload, 32, &status->epoch) ||
      !ReadLe32(payload, 40, &status->controllerPid) ||
      !ReadLe16(payload, 44, &status->configuredBuses) ||
      !ReadLe64(payload, 48, &status->monotonicTimestampUs)) {
    return false;
  }
  status->schedulingMode = static_cast<SchedulingMode>(payload[46]);
  status->realtimeErrorBits = payload[47];
  return IsKnownBusState(
             static_cast<BusState>(status->aggregateState)) &&
         status->epoch != 0 &&
         IsKnownSchedulingMode(status->schedulingMode) &&
         (status->realtimeErrorBits & 0x80U) == 0;
}

struct BusInfoPayload {
  std::uint16_t busIndex = 0;
  BusState state = BusState::Starting;
  bool linkUp = false;
  AdapterLockState lockState = AdapterLockState::Unlocked;
  AdapterLockReason lockReason = AdapterLockReason::None;
  std::uint16_t subDeviceCount = 0;
  std::string logicalName;
  std::string physicalInterface;
  std::string idPath;
  std::string permanentMac;
  std::string usbSerial;
  std::string usbVendorId;
  std::string usbProductId;
  std::uint64_t epoch = 0;
};

inline constexpr std::size_t kBusInfoHeaderSize = 30;

[[nodiscard]] inline bool IsValidUtf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<std::uint8_t>(text[index]);
    if (first == 0) {
      return false;
    }
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    std::size_t continuationCount = 0;
    std::uint32_t codePoint = 0;
    if ((first & 0xe0U) == 0xc0U) {
      continuationCount = 1;
      codePoint = first & 0x1fU;
    } else if ((first & 0xf0U) == 0xe0U) {
      continuationCount = 2;
      codePoint = first & 0x0fU;
    } else if ((first & 0xf8U) == 0xf0U) {
      continuationCount = 3;
      codePoint = first & 0x07U;
    } else {
      return false;
    }
    if (text.size() - index <= continuationCount) {
      return false;
    }
    for (std::size_t continuation = 1; continuation <= continuationCount;
         ++continuation) {
      const auto byte = static_cast<std::uint8_t>(text[index + continuation]);
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      codePoint = (codePoint << 6U) | (byte & 0x3fU);
    }
    if ((continuationCount == 1 && codePoint < 0x80U) ||
        (continuationCount == 2 && codePoint < 0x800U) ||
        (continuationCount == 3 && codePoint < 0x10000U) ||
        codePoint > 0x10ffffU ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
      return false;
    }
    index += continuationCount + 1U;
  }
  return true;
}

inline std::vector<std::uint8_t> EncodeBusInfoPayload(
    const BusInfoPayload& information) {
  const std::array<std::string_view, 7> strings{
      information.logicalName, information.physicalInterface,
      information.idPath, information.permanentMac, information.usbSerial,
      information.usbVendorId, information.usbProductId};
  std::size_t totalSize = kBusInfoHeaderSize;
  for (const std::string_view value : strings) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max() ||
        !IsValidUtf8(value) ||
        value.size() > kMaximumBusInfo - std::min(totalSize, kMaximumBusInfo)) {
      return {};
    }
    totalSize += value.size();
  }
  if (information.epoch == 0 || totalSize > kMaximumBusInfo ||
      static_cast<std::uint8_t>(information.state) >
          static_cast<std::uint8_t>(BusState::Stopping) ||
      static_cast<std::uint8_t>(information.lockState) >
          static_cast<std::uint8_t>(AdapterLockState::RuntimeUnlocked) ||
      static_cast<std::uint8_t>(information.lockReason) >
          static_cast<std::uint8_t>(AdapterLockReason::PathOnlyIdentity)) {
    return {};
  }

  std::vector<std::uint8_t> payload;
  payload.reserve(totalSize);
  AppendLe16(payload, information.busIndex);
  payload.push_back(static_cast<std::uint8_t>(information.state));
  payload.push_back(information.linkUp ? 1U : 0U);
  payload.push_back(static_cast<std::uint8_t>(information.lockState));
  payload.push_back(static_cast<std::uint8_t>(information.lockReason));
  AppendLe16(payload, information.subDeviceCount);
  for (const std::string_view value : strings) {
    AppendLe16(payload, static_cast<std::uint16_t>(value.size()));
  }
  AppendLe64(payload, information.epoch);
  for (const std::string_view value : strings) {
    payload.insert(payload.end(), value.begin(), value.end());
  }
  return payload;
}

inline bool DecodeBusInfoPayload(std::span<const std::uint8_t> payload,
                                 BusInfoPayload* information) {
  if (information == nullptr || payload.size() < kBusInfoHeaderSize ||
      payload.size() > kMaximumBusInfo ||
      !ReadLe16(payload, 0, &information->busIndex) ||
      payload[2] > static_cast<std::uint8_t>(BusState::Stopping) ||
      payload[3] > 1 ||
      payload[4] >
          static_cast<std::uint8_t>(AdapterLockState::RuntimeUnlocked) ||
      payload[5] >
          static_cast<std::uint8_t>(AdapterLockReason::PathOnlyIdentity) ||
      !ReadLe16(payload, 6, &information->subDeviceCount) ||
      !ReadLe64(payload, 22, &information->epoch) ||
      information->epoch == 0) {
    return false;
  }
  std::array<std::uint16_t, 7> lengths{};
  std::size_t totalSize = kBusInfoHeaderSize;
  for (std::size_t index = 0; index < lengths.size(); ++index) {
    if (!ReadLe16(payload, 8U + (index * 2U), &lengths[index]) ||
        static_cast<std::size_t>(lengths[index]) >
            payload.size() - std::min(totalSize, payload.size())) {
      return false;
    }
    totalSize += lengths[index];
  }
  if (totalSize != payload.size()) {
    return false;
  }

  std::array<std::string*, 7> destinations{
      &information->logicalName, &information->physicalInterface,
      &information->idPath, &information->permanentMac,
      &information->usbSerial, &information->usbVendorId,
      &information->usbProductId};
  std::size_t offset = kBusInfoHeaderSize;
  for (std::size_t index = 0; index < lengths.size(); ++index) {
    destinations[index]->assign(
        reinterpret_cast<const char*>(payload.data() + offset),
        lengths[index]);
    if (!IsValidUtf8(*destinations[index])) {
      return false;
    }
    offset += lengths[index];
  }
  information->state = static_cast<BusState>(payload[2]);
  information->linkUp = payload[3] != 0;
  information->lockState = static_cast<AdapterLockState>(payload[4]);
  information->lockReason = static_cast<AdapterLockReason>(payload[5]);
  return true;
}

inline std::vector<std::uint8_t> EncodeAckPayload(AckStatus status,
                                                  std::uint32_t detail = 0) {
  if (!IsKnownAckStatus(status)) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(8);
  AppendLe16(payload, static_cast<std::uint16_t>(status));
  AppendLe16(payload, 0);
  AppendLe32(payload, detail);
  return payload;
}

inline bool DecodeAckPayload(std::span<const std::uint8_t> payload,
                             AckStatus* status, std::uint32_t* detail) {
  std::uint16_t rawStatus = 0;
  std::uint16_t reserved = 0;
  if (status == nullptr || detail == nullptr || payload.size() != 8 ||
      !ReadLe16(payload, 0, &rawStatus) ||
      !ReadLe16(payload, 2, &reserved) || reserved != 0 ||
      !ReadLe32(payload, 4, detail)) {
    return false;
  }
  const auto decodedStatus = static_cast<AckStatus>(rawStatus);
  if (!IsKnownAckStatus(decodedStatus)) {
    return false;
  }
  *status = decodedStatus;
  return true;
}

struct PdoInputPayload {
  std::uint16_t busIndex = 0;
  std::uint16_t subDeviceIndex = 0;
  std::uint32_t offset = 0;
  std::uint32_t totalSize = 0;
  std::uint64_t epoch = 0;
  std::uint64_t cycleSequence = 0;
  std::vector<std::uint8_t> data;
};

inline std::vector<std::uint8_t> EncodePdoInputPayload(
    const PdoInputPayload& input) {
  if (input.subDeviceIndex == 0 || input.epoch == 0 ||
      input.cycleSequence == 0 || input.totalSize == 0 ||
      input.data.empty() ||
      input.data.size() > kMaximumPdoChunk ||
      input.totalSize > kMaximumPdoImage || input.offset > input.totalSize ||
      input.data.size() >
          static_cast<std::size_t>(input.totalSize - input.offset)) {
    return {};
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(32 + input.data.size());
  AppendLe16(payload, input.busIndex);
  AppendLe16(payload, input.subDeviceIndex);
  AppendLe32(payload, input.offset);
  AppendLe32(payload, input.totalSize);
  AppendLe16(payload, static_cast<std::uint16_t>(input.data.size()));
  AppendLe16(payload, 0);
  AppendLe64(payload, input.epoch);
  AppendLe64(payload, input.cycleSequence);
  payload.insert(payload.end(), input.data.begin(), input.data.end());
  return payload;
}

inline bool DecodePdoInputPayload(std::span<const std::uint8_t> payload,
                                  PdoInputPayload* input,
                                  std::size_t maximumTotalSize =
                                      kMaximumPdoImage) {
  std::uint16_t chunkLength = 0;
  std::uint16_t reserved = 0;
  if (input == nullptr || payload.size() < 32 ||
      !ReadLe16(payload, 0, &input->busIndex) ||
      !ReadLe16(payload, 2, &input->subDeviceIndex) ||
      !ReadLe32(payload, 4, &input->offset) ||
      !ReadLe32(payload, 8, &input->totalSize) ||
      !ReadLe16(payload, 12, &chunkLength) ||
      !ReadLe16(payload, 14, &reserved) || reserved != 0 ||
      !ReadLe64(payload, 16, &input->epoch) ||
      !ReadLe64(payload, 24, &input->cycleSequence) ||
      input->subDeviceIndex == 0 ||
      input->epoch == 0 || input->cycleSequence == 0 ||
      input->totalSize == 0 || chunkLength == 0 ||
      chunkLength > kMaximumPdoChunk ||
      input->totalSize > maximumTotalSize ||
      input->offset > input->totalSize ||
      static_cast<std::size_t>(chunkLength) >
          static_cast<std::size_t>(input->totalSize - input->offset) ||
      payload.size() != 32U + static_cast<std::size_t>(chunkLength)) {
    return false;
  }
  input->data.assign(payload.begin() + 32, payload.end());
  return true;
}

}  // namespace ec_systemcore::protocol
