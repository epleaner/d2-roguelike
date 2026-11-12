#pragma once

#include <cstdint>

namespace MerchantQuality {
constexpr std::uint32_t kBasisPoints = 10000;
enum class Tier : std::uint8_t { White, Magic, Rare, Unique, Set };

struct Distribution {
    std::uint16_t white;
    std::uint16_t magic;
    std::uint16_t rare;
    std::uint16_t unique;
    std::uint16_t set;
};

constexpr Distribution AtLevel(int characterLevel) {
    const unsigned level = characterLevel < 1 ? 1u :
        characterLevel > 99 ? 99u : static_cast<unsigned>(characterLevel);
    const unsigned progress = level - 1;
    const unsigned white = 8200 - 8200 * progress / 98;
    const unsigned rare = 100 + 4900 * progress / 98;
    const unsigned specialProgress = level <= 25 ? 0 : level - 25;
    const unsigned special = 3000 * specialProgress * specialProgress *
        (222 - 2 * specialProgress) / (74 * 74 * 74);
    const unsigned unique = special / 2;
    const unsigned set = special - unique;
    const unsigned magic = kBasisPoints - white - rare - special;
    return {static_cast<std::uint16_t>(white),
        static_cast<std::uint16_t>(magic), static_cast<std::uint16_t>(rare),
        static_cast<std::uint16_t>(unique), static_cast<std::uint16_t>(set)};
}

constexpr Tier Select(int characterLevel, std::uint32_t roll) {
    const Distribution odds = AtLevel(characterLevel);
    const unsigned sample = roll < kBasisPoints ? roll : kBasisPoints - 1;
    if (sample < odds.white) return Tier::White;
    if (sample < odds.white + odds.magic) return Tier::Magic;
    if (sample < odds.white + odds.magic + odds.rare) return Tier::Rare;
    if (sample < odds.white + odds.magic + odds.rare + odds.unique)
        return Tier::Unique;
    return Tier::Set;
}

constexpr int ToItemQuality(Tier tier, int nativeQuality) {
    switch (tier) {
    case Tier::White: return nativeQuality == 3 ? 3 : 2;
    case Tier::Magic: return 4;
    case Tier::Rare: return 6;
    case Tier::Unique: return 7;
    case Tier::Set: return 5;
    }
    return nativeQuality;
}
} // namespace MerchantQuality
