#pragma once
#include <cstdint>

namespace RoguelikeResistance {
constexpr int kNormalPenalty = 0;
constexpr int kNightmarePenalty = -40;
constexpr int kHellPenalty = -100;

constexpr int RunPenalty(int depth) noexcept {
    return depth >= 60 ? kHellPenalty :
        depth >= 40 ? kNightmarePenalty : kNormalPenalty;
}

constexpr int DifficultyPenalty(int difficulty) noexcept {
    switch (difficulty) {
    case 1: return kNightmarePenalty;
    case 2: return kHellPenalty;
    default: return kNormalPenalty;
    }
}

constexpr int AdditionalPenalty(int depth, int difficulty) noexcept {
    const int target = RunPenalty(depth);
    const int current = DifficultyPenalty(difficulty);
    return target < current ? target - current : 0;
}

constexpr int DecodeMonsterResistanceWord(std::uint16_t value) noexcept {
    return value <= 0x7fff ? static_cast<int>(value) :
        static_cast<int>(value) - 0x10000;
}

constexpr bool InterpolateMonsterResistance(int normal, int nightmare, int hell,
    int depth, int& value) noexcept {
    if (depth < 1 || depth > 99 || normal < -32768 || normal > 32767 ||
        nightmare < -32768 || nightmare > 32767 ||
        hell < -32768 || hell > 32767) return false;

    int start{}, end{}, startDepth{}, endDepth{};
    if (depth <= 40) {
        start = normal;
        end = nightmare;
        startDepth = 1;
        endDepth = 40;
    } else if (depth <= 60) {
        start = nightmare;
        end = hell;
        startDepth = 40;
        endDepth = 60;
    } else {
        value = hell;
        return true;
    }

    const std::int64_t numerator =
        (std::int64_t(end) - start) * (depth - startDepth);
    const std::int64_t denominator = endDepth - startDepth;
    const std::int64_t half = denominator / 2;
    const std::int64_t offset = numerator >= 0 ?
        (numerator + half) / denominator :
        -((-numerator + half) / denominator);
    value = start + static_cast<int>(offset);
    return true;
}
} // namespace RoguelikeResistance
