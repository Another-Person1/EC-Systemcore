#pragma once

#include "ec_systemcore/protocol.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace ec_systemcore {

enum RealtimeError : std::uint8_t {
  RealtimeMlockFailed = 1U << 0U,
  RealtimeSchedulerFailed = 1U << 1U,
  RealtimeAffinityConfigurationInvalid = 1U << 2U,
  RealtimeAffinityApplyFailed = 1U << 3U,
  RealtimeClockError = 1U << 4U,
  RealtimePriorityInvalid = 1U << 5U,
  RealtimeDistributedClockUnlocked = 1U << 6U,
};

struct RealtimeConfig {
  bool requestMemoryLock = true;
  bool requestFifo = true;
  int fifoPriority = 55;
  std::optional<unsigned int> cpuAffinity;
};

struct RealtimeOperations {
  std::function<int()> lockMemory;
  std::function<int(int)> setFifoScheduler;
  std::function<int(unsigned int)> setCpuAffinity;
  std::function<unsigned int()> availableCpuCount;
  std::function<int()> currentSchedulingPolicy;
};

struct RealtimeResult {
  bool requested = false;
  bool memoryLocked = false;
  bool schedulerApplied = false;
  bool affinityApplied = false;
  protocol::SchedulingMode schedulingMode =
      protocol::SchedulingMode::Other;
  std::uint8_t errorBits = 0;

  [[nodiscard]] bool fullyApplied(const RealtimeConfig& config) const {
    return (!config.requestMemoryLock || memoryLocked) &&
           (!config.requestFifo || schedulerApplied) &&
           (!config.cpuAffinity.has_value() || affinityApplied);
  }
};

inline RealtimeResult ApplyRealtimeConfiguration(
    const RealtimeConfig& config, const RealtimeOperations& operations) {
  RealtimeResult result;
  result.requested = config.requestMemoryLock || config.requestFifo ||
                     config.cpuAffinity.has_value();

  if (config.requestMemoryLock) {
    if (operations.lockMemory && operations.lockMemory() == 0) {
      result.memoryLocked = true;
    } else {
      result.errorBits |= RealtimeMlockFailed;
    }
  }

  if (config.requestFifo) {
    if (config.fifoPriority <= 0 || config.fifoPriority >= 100) {
      result.errorBits |= RealtimePriorityInvalid;
    } else if (operations.setFifoScheduler &&
               operations.setFifoScheduler(config.fifoPriority) == 0) {
      result.schedulerApplied = true;
    } else {
      result.errorBits |= RealtimeSchedulerFailed;
    }
  }

  if (config.cpuAffinity.has_value()) {
    const unsigned int available =
        operations.availableCpuCount ? operations.availableCpuCount() : 0;
    if (available == 0 || *config.cpuAffinity >= available) {
      result.errorBits |= RealtimeAffinityConfigurationInvalid;
    } else if (operations.setCpuAffinity &&
               operations.setCpuAffinity(*config.cpuAffinity) == 0) {
      result.affinityApplied = true;
    } else {
      result.errorBits |= RealtimeAffinityApplyFailed;
    }
  }

  const int policy =
      operations.currentSchedulingPolicy
          ? operations.currentSchedulingPolicy()
          : 0;
  if (policy == 1) {
    result.schedulingMode = protocol::SchedulingMode::Fifo;
  }
  if (config.requestFifo &&
      result.schedulingMode != protocol::SchedulingMode::Fifo) {
    result.errorBits |= RealtimeSchedulerFailed;
    result.schedulerApplied = false;
  }
  return result;
}

}  // namespace ec_systemcore
