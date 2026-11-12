#pragma once

#include <cstdint>
#include <climits>

namespace RoguelikeMonsterScaling {
constexpr int kHitPointUnit = 256;

struct HitPoints {
    int currentRaw;
    int maximumRaw;
};

constexpr bool ScaleHitPoints(int nativeLevel, int zoneLevel,
    int currentRaw, int maximumRaw, HitPoints& scaled) noexcept {
    if (nativeLevel < 1 || nativeLevel > 65535 || zoneLevel < 1 ||
        zoneLevel > 99 || maximumRaw < kHitPointUnit || currentRaw < 0 ||
        currentRaw > maximumRaw) return false;

    const std::int64_t maximum =
        (std::int64_t(maximumRaw) * zoneLevel + nativeLevel / 2) / nativeLevel;
    const std::int64_t current =
        (std::int64_t(currentRaw) * zoneLevel + nativeLevel / 2) / nativeLevel;
    if (maximum < 0 || maximum > INT_MAX || current < 0 || current > maximum)
        return false;

    const std::int64_t boundedMaximum =
        maximum < kHitPointUnit ? kHitPointUnit : maximum;
    const std::int64_t boundedCurrent = currentRaw == 0 ? 0 :
        (current < kHitPointUnit ? kHitPointUnit :
         current > boundedMaximum ? boundedMaximum : current);
    scaled = {static_cast<int>(boundedCurrent),
              static_cast<int>(boundedMaximum)};
    return true;
}
} // namespace RoguelikeMonsterScaling
