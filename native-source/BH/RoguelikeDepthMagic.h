#pragma once

#include <array>
#include <cstddef>

namespace RoguelikeDepthMagic {
enum class AffixSide { Prefix, Suffix };

struct Effect {
    AffixSide side;
    unsigned affixId;
    unsigned statId;
    unsigned perDepth;
    unsigned maxDepth;
    unsigned equipmentSlots;
    const wchar_t* tooltip;
};

// Slot numbers match the engine's BodyLocation values. New effects also need
// an authenticated, statless TXT affix row and a verified native stat.
constexpr unsigned kJewelryAndBoots =
    (1u << 2) | (1u << 6) | (1u << 7) | (1u << 9);
inline constexpr std::array<Effect, 2> kEffects{{
    {AffixSide::Suffix, 748, 80, 1, 99, kJewelryAndBoots,
        L"+1% Better Chance of Getting Magic Items per Run Depth"},
    {AffixSide::Prefix, 1713, 85, 1, 99, kJewelryAndBoots,
        L"+1% Experience Gained per Run Depth"},
}};

template <std::size_t N>
constexpr bool Valid(const std::array<Effect, N>& effects) {
    for (std::size_t i = 0; i < N; ++i) {
        const Effect& effect = effects[i];
        if (!effect.affixId || effect.affixId > 65535 || effect.statId > 511 ||
            !effect.perDepth || effect.perDepth > 1024 || !effect.maxDepth ||
            effect.maxDepth > 99 || !effect.equipmentSlots ||
            (effect.equipmentSlots & ~0x1ffeu) || !effect.tooltip || !*effect.tooltip)
            return false;
        for (std::size_t j = 0; j < i; ++j)
            if (effects[j].statId == effect.statId ||
                (effects[j].side == effect.side && effects[j].affixId == effect.affixId))
                return false;
    }
    return true;
}
static_assert(Valid(kEffects), "invalid or ambiguous run-depth effect registry");

template <std::size_t N>
constexpr unsigned EquipmentSlots(const std::array<Effect, N>& effects) {
    unsigned slots = 0;
    for (const Effect& effect : effects) slots |= effect.equipmentSlots;
    return slots;
}
inline constexpr unsigned kEquipmentSlots = EquipmentSlots(kEffects);

constexpr unsigned Matches(const Effect& effect, const unsigned short* prefixes,
    const unsigned short* suffixes) {
    const auto* affixes = effect.side == AffixSide::Prefix ? prefixes : suffixes;
    unsigned matches = 0;
    for (unsigned i = 0; i < 3; ++i) matches += affixes[i] == effect.affixId;
    return matches;
}

template <std::size_t N>
constexpr std::array<int, N> Counts(const std::array<Effect, N>& effects,
    const unsigned short* prefixes, const unsigned short* suffixes) {
    std::array<int, N> counts{};
    for (std::size_t i = 0; i < N; ++i)
        counts[i] = static_cast<int>(Matches(effects[i], prefixes, suffixes));
    return counts;
}

constexpr unsigned Bonus(const Effect& effect, unsigned count, unsigned depth) {
    if (depth > 99) return 0;
    return count * effect.perDepth * (depth < effect.maxDepth ? depth : effect.maxDepth);
}
}
