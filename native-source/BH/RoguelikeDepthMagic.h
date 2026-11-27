#pragma once

#include <array>

namespace RoguelikeDepthMagic {
constexpr unsigned kMagicFind = 80;
constexpr unsigned kExperienceGain = 85;
inline constexpr std::array<unsigned, 2> kStats{kMagicFind, kExperienceGain};
constexpr unsigned kSuffix = 748;
constexpr unsigned kExperiencePrefix = 1713;

constexpr unsigned Coefficient(unsigned suffix) {
    return suffix == kSuffix ? 1u : 0u;
}
constexpr unsigned ExperienceCoefficient(unsigned prefix) {
    return prefix == kExperiencePrefix ? 1u : 0u;
}
constexpr unsigned Bonus(unsigned coefficient, unsigned depth) {
    return depth <= 99 ? coefficient * depth : 0;
}
}
