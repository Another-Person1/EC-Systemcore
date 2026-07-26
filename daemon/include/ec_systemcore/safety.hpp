#pragma once

#include "ec_systemcore/protocol.hpp"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace ec_systemcore {

// Output writes are deliberately whole-image transactions. A partial update
// must not refresh the watchdog while old nonzero bytes remain in the same
// SubDevice image.
[[nodiscard]] constexpr protocol::AckStatus ValidateWholeOutputWrite(
    std::uint32_t outputBytes, std::uint32_t offset,
    std::size_t length) {
  if (outputBytes == 0 || outputBytes > protocol::kMaximumOutputWrite ||
      offset != 0 ||
      length != static_cast<std::size_t>(outputBytes)) {
    return protocol::AckStatus::Bounds;
  }
  return protocol::AckStatus::Ok;
}

// SOEM exposes PDO images as a pointer plus a starting bit. SubDevices with
// fewer than eight bits can share the first/last mapped byte with a neighbor,
// so memcpy/memset on ceil(bits / 8) bytes is not safe. IPC images are
// canonical little-endian bit strings: logical bit zero is bit zero of byte
// zero and unused high bits in the last byte are zero.
[[nodiscard]] constexpr std::uint32_t PdoImageByteLength(
    std::uint16_t bitCount) {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint32_t>(bitCount) + 7U) / 8U);
}

[[nodiscard]] constexpr std::uint32_t PdoMappedByteLength(
    std::uint16_t bitCount, std::uint8_t startBit) {
  if (bitCount == 0 || startBit > 7U) {
    return 0;
  }
  return static_cast<std::uint32_t>(
      (static_cast<std::uint32_t>(startBit) +
       static_cast<std::uint32_t>(bitCount) + 7U) /
      8U);
}

[[nodiscard]] constexpr bool IsValidSoemPdoMetadata(
    std::uint16_t bitCount, std::uint32_t byteCount,
    std::uint8_t startBit) {
  if (startBit > 7U) {
    return false;
  }
  if (bitCount == 0U) {
    return byteCount == 0U && startBit == 0U;
  }
  // SOEM keeps byteCount at zero for sub-byte PDOs and rounds up all larger
  // PDOs. Byte-oriented images are aligned by its mapper.
  const std::uint32_t expectedBytes =
      bitCount > 7U ? PdoImageByteLength(bitCount) : 0U;
  return byteCount == expectedBytes &&
         (bitCount <= 7U || startBit == 0U);
}

[[nodiscard]] inline bool HasCanonicalPdoPadding(
    std::uint16_t bitCount,
    std::span<const std::uint8_t> image) {
  if (image.size() != PdoImageByteLength(bitCount)) {
    return false;
  }
  if (bitCount == 0) {
    return image.empty();
  }
  const std::uint8_t usedBits =
      static_cast<std::uint8_t>(bitCount % 8U);
  if (usedBits == 0U) {
    return true;
  }
  const auto unusedMask = static_cast<std::uint8_t>(
      0xffU << usedBits);
  return (image.back() & unusedMask) == 0U;
}

[[nodiscard]] inline bool ReadPackedPdoChunk(
    std::span<const std::uint8_t> mapped, std::uint8_t startBit,
    std::uint16_t bitCount, std::uint32_t logicalByteOffset,
    std::span<std::uint8_t> destination) {
  const std::uint32_t imageBytes = PdoImageByteLength(bitCount);
  const std::uint32_t mappedBytes =
      PdoMappedByteLength(bitCount, startBit);
  if (startBit > 7U || mapped.size() != mappedBytes ||
      logicalByteOffset > imageBytes ||
      destination.size() >
          static_cast<std::size_t>(imageBytes - logicalByteOffset)) {
    return false;
  }
  if (startBit == 0U) {
    std::copy_n(
        mapped.begin() + static_cast<std::ptrdiff_t>(logicalByteOffset),
        destination.size(), destination.begin());
    const std::uint8_t usedBits =
        static_cast<std::uint8_t>(bitCount % 8U);
    if (usedBits != 0U && !destination.empty() &&
        static_cast<std::size_t>(logicalByteOffset) +
                destination.size() ==
            imageBytes) {
      destination.back() &= static_cast<std::uint8_t>(
          (1U << usedBits) - 1U);
    }
    return true;
  }
  std::fill(destination.begin(), destination.end(), 0U);
  const std::uint64_t firstLogicalBit =
      static_cast<std::uint64_t>(logicalByteOffset) * 8U;
  for (std::size_t byteIndex = 0; byteIndex < destination.size();
       ++byteIndex) {
    for (std::uint8_t bit = 0; bit < 8U; ++bit) {
      const std::uint64_t logicalBit =
          firstLogicalBit +
          static_cast<std::uint64_t>(byteIndex) * 8U + bit;
      if (logicalBit >= bitCount) {
        break;
      }
      const std::uint64_t mappedBit =
          static_cast<std::uint64_t>(startBit) + logicalBit;
      const auto sourceMask = static_cast<std::uint8_t>(
          1U << (mappedBit % 8U));
      if ((mapped[static_cast<std::size_t>(mappedBit / 8U)] &
           sourceMask) != 0U) {
        destination[byteIndex] |=
            static_cast<std::uint8_t>(1U << bit);
      }
    }
  }
  return true;
}

[[nodiscard]] inline bool WritePackedPdoImage(
    std::span<std::uint8_t> mapped, std::uint8_t startBit,
    std::uint16_t bitCount,
    std::span<const std::uint8_t> image) {
  const std::uint32_t mappedBytes =
      PdoMappedByteLength(bitCount, startBit);
  if (startBit > 7U || mapped.size() != mappedBytes ||
      !HasCanonicalPdoPadding(bitCount, image)) {
    return false;
  }
  if (startBit == 0U && (bitCount % 8U) == 0U) {
    std::copy(image.begin(), image.end(), mapped.begin());
    return true;
  }
  for (std::uint32_t logicalBit = 0; logicalBit < bitCount;
       ++logicalBit) {
    const std::uint32_t mappedBit =
        static_cast<std::uint32_t>(startBit) + logicalBit;
    const auto destinationMask = static_cast<std::uint8_t>(
        1U << (mappedBit % 8U));
    std::uint8_t& destination =
        mapped[static_cast<std::size_t>(mappedBit / 8U)];
    const auto sourceMask = static_cast<std::uint8_t>(
        1U << (logicalBit % 8U));
    if ((image[static_cast<std::size_t>(logicalBit / 8U)] &
         sourceMask) != 0U) {
      destination |= destinationMask;
    } else {
      destination &= static_cast<std::uint8_t>(~destinationMask);
    }
  }
  return true;
}

inline bool ZeroPackedPdoImage(std::span<std::uint8_t> mapped,
                               std::uint8_t startBit,
                               std::uint16_t bitCount) {
  const std::uint32_t mappedBytes =
      PdoMappedByteLength(bitCount, startBit);
  if (startBit > 7U || mapped.size() != mappedBytes) {
    return false;
  }
  if (startBit == 0U && (bitCount % 8U) == 0U) {
    std::fill(mapped.begin(), mapped.end(), 0U);
    return true;
  }
  for (std::uint32_t logicalBit = 0; logicalBit < bitCount;
       ++logicalBit) {
    const std::uint32_t mappedBit =
        static_cast<std::uint32_t>(startBit) + logicalBit;
    mapped[static_cast<std::size_t>(mappedBit / 8U)] &=
        static_cast<std::uint8_t>(
            ~(1U << (mappedBit % 8U)));
  }
  return true;
}

struct ControllerRateLimit {
  std::uint32_t burstFrames = 500;
  std::uint32_t framesPerSecond = 2000;
};

[[nodiscard]] constexpr ControllerRateLimit CalculateControllerRateLimit(
    std::size_t outputTargetCount,
    std::chrono::milliseconds outputCommandTimeout) {
  constexpr std::uint64_t kMaximumControllerFramesPerSecond = 200000;
  constexpr std::uint64_t kMaximumControllerBurstFrames = 10000;
  const std::uint64_t timeoutMs =
      outputCommandTimeout.count() > 0
          ? static_cast<std::uint64_t>(outputCommandTimeout.count())
          : 1U;
  const std::uint64_t refreshesPerSecond =
      (1000U + timeoutMs - 1U) / timeoutMs;
  const std::uint64_t targets = outputTargetCount;
  const std::uint64_t requiredWrites =
      targets > kMaximumControllerFramesPerSecond / refreshesPerSecond
          ? kMaximumControllerFramesPerSecond
          : targets * refreshesPerSecond;
  const std::uint64_t rateWithHeadroom =
      requiredWrites >
              (kMaximumControllerFramesPerSecond - 100U) / 2U
          ? kMaximumControllerFramesPerSecond
          : requiredWrites * 2U + 100U;
  const std::uint64_t burstWithHeadroom =
      targets >
              (kMaximumControllerBurstFrames - 64U) / 3U
          ? kMaximumControllerBurstFrames
          : targets * 3U + 64U;
  return {
      .burstFrames = static_cast<std::uint32_t>(
          std::clamp<std::uint64_t>(burstWithHeadroom, 500U,
                                    kMaximumControllerBurstFrames)),
      .framesPerSecond = static_cast<std::uint32_t>(
          std::clamp<std::uint64_t>(rateWithHeadroom, 2000U,
                                    kMaximumControllerFramesPerSecond)),
  };
}

template <typename Integer>
constexpr void SaturatingIncrement(Integer& value) {
  if (value != std::numeric_limits<Integer>::max()) {
    ++value;
  }
}

class SafetyGate final {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using ClientId = std::uint64_t;

  struct Result {
    protocol::AckStatus status = protocol::AckStatus::Ok;
    bool outputsWereDisabled = false;
    bool epochChanged = false;
  };

  explicit SafetyGate(
      std::uint64_t initialEpoch,
      std::chrono::milliseconds heartbeatTimeout =
          std::chrono::milliseconds{250},
      std::chrono::milliseconds outputCommandTimeout =
          std::chrono::milliseconds{100})
      : epoch_(initialEpoch == 0 ? 1 : initialEpoch),
        heartbeatTimeout_(heartbeatTimeout),
        outputCommandTimeout_(outputCommandTimeout) {}

  [[nodiscard]] std::uint64_t epoch() const { return epoch_; }
  [[nodiscard]] bool hasController() const {
    return controller_.has_value();
  }
  [[nodiscard]] bool outputsEnabled() const { return outputsEnabled_; }
  [[nodiscard]] std::uint32_t controllerPid() const {
    return controller_.has_value() ? controller_->pid : 0;
  }
  [[nodiscard]] std::optional<ClientId> controllerId() const {
    if (!controller_.has_value()) {
      return std::nullopt;
    }
    return controller_->id;
  }
  [[nodiscard]] std::chrono::milliseconds heartbeatTimeout() const {
    return heartbeatTimeout_;
  }
  [[nodiscard]] std::chrono::milliseconds outputCommandTimeout() const {
    return outputCommandTimeout_;
  }

  Result Acquire(ClientId clientId, std::uint32_t peerPid, TimePoint now) {
    if (clientId == 0) {
      return {.status = protocol::AckStatus::Malformed};
    }
    if (controller_.has_value() && controller_->id != clientId) {
      return {.status = protocol::AckStatus::Busy};
    }
    controller_ = Controller{clientId, peerPid, now};
    const bool wasEnabled = outputsEnabled_;
    outputsEnabled_ = false;
    return {.outputsWereDisabled = wasEnabled};
  }

  Result Heartbeat(ClientId clientId, std::uint64_t commandEpoch,
                   TimePoint now) {
    Result authorization = Authorize(clientId, commandEpoch);
    if (authorization.status != protocol::AckStatus::Ok) {
      return authorization;
    }
    controller_->lastHeartbeat = now;
    return {};
  }

  Result SetOutputEnabled(ClientId clientId, std::uint64_t commandEpoch,
                          bool enabled, bool processDataHealthy,
                          TimePoint now) {
    Result authorization = Authorize(clientId, commandEpoch);
    if (authorization.status != protocol::AckStatus::Ok) {
      return authorization;
    }
    if (enabled && !processDataHealthy) {
      return {.status = protocol::AckStatus::Disabled};
    }
    if (!enabled) {
      return DropControllerAndAdvanceEpoch();
    }
    const bool wasEnabled = outputsEnabled_;
    outputsEnabled_ = true;
    controller_->lastHeartbeat = now;
    lastOutputCommand_ = now;
    return {.outputsWereDisabled = wasEnabled};
  }

  Result AuthorizeWrite(ClientId clientId,
                        std::uint64_t commandEpoch) const {
    Result authorization = Authorize(clientId, commandEpoch);
    if (authorization.status != protocol::AckStatus::Ok) {
      return authorization;
    }
    if (!outputsEnabled_) {
      return {.status = protocol::AckStatus::Disabled};
    }
    return {};
  }

  Result RecordOutputWrite(ClientId clientId,
                           std::uint64_t commandEpoch,
                           TimePoint now) {
    Result authorization = AuthorizeWrite(clientId, commandEpoch);
    if (authorization.status == protocol::AckStatus::Ok) {
      lastOutputCommand_ = now;
    }
    return authorization;
  }

  Result AuthorizeController(ClientId clientId,
                             std::uint64_t commandEpoch) const {
    return Authorize(clientId, commandEpoch);
  }

  Result Release(ClientId clientId, std::uint64_t commandEpoch) {
    Result authorization = Authorize(clientId, commandEpoch);
    if (authorization.status != protocol::AckStatus::Ok) {
      return authorization;
    }
    return DropControllerAndAdvanceEpoch();
  }

  Result Disconnect(ClientId clientId) {
    if (!controller_.has_value() || controller_->id != clientId) {
      return {};
    }
    return DropControllerAndAdvanceEpoch();
  }

  Result Expire(TimePoint now) {
    if (!controller_.has_value() ||
        now - controller_->lastHeartbeat <= heartbeatTimeout_) {
      return {};
    }
    return DropControllerAndAdvanceEpoch();
  }

  Result ExpireOutputCommands(TimePoint now) {
    if (!controller_.has_value() || !outputsEnabled_ ||
        now - lastOutputCommand_ <= outputCommandTimeout_) {
      return {};
    }
    return DropControllerAndAdvanceEpoch();
  }

  Result Invalidate() { return DropControllerAndAdvanceEpoch(); }

 private:
  struct Controller {
    ClientId id = 0;
    std::uint32_t pid = 0;
    TimePoint lastHeartbeat{};
  };

  [[nodiscard]] Result Authorize(ClientId clientId,
                                 std::uint64_t commandEpoch) const {
    if (commandEpoch != epoch_) {
      return {.status = protocol::AckStatus::StaleEpoch};
    }
    if (!controller_.has_value() || controller_->id != clientId) {
      return {.status = protocol::AckStatus::Unauthorized};
    }
    return {};
  }

  Result DropControllerAndAdvanceEpoch() {
    const bool wasEnabled = outputsEnabled_;
    outputsEnabled_ = false;
    controller_.reset();
    if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
      epoch_ = 1;
    } else {
      ++epoch_;
    }
    return {.outputsWereDisabled = wasEnabled, .epochChanged = true};
  }

  std::uint64_t epoch_ = 1;
  std::chrono::milliseconds heartbeatTimeout_{250};
  std::chrono::milliseconds outputCommandTimeout_{100};
  TimePoint lastOutputCommand_{};
  std::optional<Controller> controller_;
  bool outputsEnabled_ = false;
};

}  // namespace ec_systemcore
