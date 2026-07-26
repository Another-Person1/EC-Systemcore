#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace ec_systemcore {

struct DistributedClockAdjustment {
  bool valid = false;
  bool synchronized = false;
  bool fault = false;
  std::int64_t phaseErrorNs = 0;
  std::int64_t correctionNs = 0;
};

class DistributedClockController final {
 public:
  explicit DistributedClockController(std::int64_t cycleNanoseconds)
      : cycleNanoseconds_(cycleNanoseconds) {}

  void Reset() {
    integral_ = 0;
    observations_ = 0;
    stableCycles_ = 0;
    badCyclesAfterLock_ = 0;
    invalidAdvanceCycles_ = 0;
    previousReference_ = 0;
    synchronized_ = false;
  }

  [[nodiscard]] DistributedClockAdjustment Update(
      std::int64_t referenceTimeNanoseconds) {
    DistributedClockAdjustment result;
    if (cycleNanoseconds_ <= 0 || referenceTimeNanoseconds <= 0) {
      if (observations_ != std::numeric_limits<std::uint32_t>::max()) {
        ++observations_;
      }
      result.fault = observations_ >= 500;
      return result;
    }
    result.valid = true;
    if (observations_ != std::numeric_limits<std::uint32_t>::max()) {
      ++observations_;
    }

    bool referenceAdvanced = true;
    if (previousReference_ != 0) {
      const std::int64_t advancement =
          referenceTimeNanoseconds > previousReference_
              ? referenceTimeNanoseconds - previousReference_
              : 0;
      std::int64_t roundedCycles = 0;
      if (advancement > 0) {
        roundedCycles = advancement / cycleNanoseconds_;
        const std::int64_t remainder =
            advancement % cycleNanoseconds_;
        if (remainder >= cycleNanoseconds_ / 2 &&
            roundedCycles != std::numeric_limits<std::int64_t>::max()) {
          ++roundedCycles;
        }
      }
      const std::int64_t advancementTolerance =
          std::max<std::int64_t>(1000, cycleNanoseconds_ / 5);
      referenceAdvanced = roundedCycles >= 1 && roundedCycles <= 4;
      if (referenceAdvanced) {
        if (roundedCycles >
            std::numeric_limits<std::int64_t>::max() /
                cycleNanoseconds_) {
          referenceAdvanced = false;
        } else {
          referenceAdvanced =
              std::abs(advancement -
                       roundedCycles * cycleNanoseconds_) <=
              advancementTolerance;
        }
      }
    } else {
      referenceAdvanced = false;
    }
    previousReference_ = referenceTimeNanoseconds;
    if (!referenceAdvanced) {
      stableCycles_ = 0;
      if (invalidAdvanceCycles_ !=
          std::numeric_limits<std::uint32_t>::max()) {
        ++invalidAdvanceCycles_;
      }
      if (invalidAdvanceCycles_ >= 3) {
        synchronized_ = false;
      }
    } else {
      invalidAdvanceCycles_ = 0;
    }

    std::int64_t phase =
        referenceTimeNanoseconds % cycleNanoseconds_;
    if (phase > cycleNanoseconds_ / 2) {
      phase -= cycleNanoseconds_;
    }
    result.phaseErrorNs = phase;

    const std::int64_t integralLimit =
        cycleNanoseconds_ >
                std::numeric_limits<std::int64_t>::max() / 100
            ? std::numeric_limits<std::int64_t>::max()
            : cycleNanoseconds_ * 100;
    if (referenceAdvanced && phase > 0) {
      integral_ = std::min(integral_ + 1, integralLimit);
    } else if (referenceAdvanced && phase < 0) {
      integral_ = std::max(integral_ - 1, -integralLimit);
    }
    const std::int64_t maximumCorrection =
        std::max<std::int64_t>(1, cycleNanoseconds_ / 10);
    if (referenceAdvanced) {
      result.correctionNs = std::clamp(
          -(phase / 100) - (integral_ / 1000),
          -maximumCorrection, maximumCorrection);
    }

    const std::int64_t tolerance =
        std::max<std::int64_t>(1000, cycleNanoseconds_ / 20);
    if (referenceAdvanced && std::abs(phase) <= tolerance) {
      if (stableCycles_ != std::numeric_limits<std::uint32_t>::max()) {
        ++stableCycles_;
      }
    } else {
      stableCycles_ = 0;
    }
    if (stableCycles_ >= 20) {
      synchronized_ = true;
    }

    if (synchronized_ &&
        std::abs(phase) > cycleNanoseconds_ / 4) {
      if (badCyclesAfterLock_ !=
          std::numeric_limits<std::uint32_t>::max()) {
        ++badCyclesAfterLock_;
      }
    } else {
      badCyclesAfterLock_ = 0;
    }
    result.synchronized = synchronized_;
    result.fault = invalidAdvanceCycles_ >= 3 ||
                   badCyclesAfterLock_ >= 10 ||
                   (!synchronized_ && observations_ >= 500);
    return result;
  }

 private:
  std::int64_t cycleNanoseconds_ = 0;
  std::int64_t integral_ = 0;
  std::uint32_t observations_ = 0;
  std::uint32_t stableCycles_ = 0;
  std::uint32_t badCyclesAfterLock_ = 0;
  std::uint32_t invalidAdvanceCycles_ = 0;
  std::int64_t previousReference_ = 0;
  bool synchronized_ = false;
};

}  // namespace ec_systemcore
