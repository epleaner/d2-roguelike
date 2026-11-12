#pragma once

#include <cstdint>
#include <array>
#include <climits>

namespace RoguelikeLoot {
constexpr std::uint32_t kBasisPoints = 10000;
constexpr std::uint32_t kDropFlagNeverEthereal = 0x02;
constexpr std::uint32_t kDropFlagNoSockets = 0x08;
constexpr std::uint32_t kDropFlagAlwaysSockets = 0x10;
constexpr std::uint32_t DropGenerationFlags(bool forceSockets) noexcept {
    return kDropFlagNeverEthereal |
        (forceSockets ? kDropFlagAlwaysSockets : kDropFlagNoSockets);
}
constexpr std::uint32_t kNormalSocketDifficultyCap = 3;
constexpr unsigned ScaledTier(int level, unsigned tierCount,
    std::uint32_t roll) noexcept {
    if (!tierCount) return 0;
    const unsigned clamped = level < 1 ? 1u :
        level > 99 ? 99u : static_cast<unsigned>(level);
    const std::uint64_t progress = std::uint64_t(clamped - 1) *
        (tierCount - 1) * kBasisPoints / 98u;
    const unsigned lower = static_cast<unsigned>(progress / kBasisPoints);
    const unsigned upperWeight = static_cast<unsigned>(progress % kBasisPoints);
    const unsigned sample = roll < kBasisPoints ? roll : kBasisPoints - 1;
    return lower + (lower + 1 < tierCount && sample < upperWeight ? 1u : 0u);
}
constexpr unsigned CharmSizeForRewardIndex(unsigned index) noexcept {
    return index % 3u + 1u;
}
constexpr unsigned SocketTypeMaximum(int itemLevel, unsigned maxAt1,
    unsigned maxAt25, unsigned maxAt40) noexcept {
    return itemLevel <= 25 ? maxAt1 : itemLevel <= 40 ? maxAt25 : maxAt40;
}
constexpr unsigned MaximumSockets(unsigned baseMaximum,
    unsigned itemTypeMaximum, unsigned difficultyMaximum) noexcept {
    const unsigned typeLimit = baseMaximum < itemTypeMaximum ?
        baseMaximum : itemTypeMaximum;
    return typeLimit < difficultyMaximum ? typeLimit : difficultyMaximum;
}
constexpr std::uint32_t SeedForMaximumSockets(std::uint32_t seed,
    unsigned maximum) noexcept {
    if (!maximum) return seed;
    const unsigned delta = maximum - 1u - seed % maximum;
    const std::uint32_t maxSeed = ~std::uint32_t(0);
    return seed > maxSeed - delta ? seed - (maximum - delta) : seed + delta;
}
constexpr int RewardItemLevel(int monsterLevel, int zoneDepth,
    bool testZone) noexcept {
    return testZone && zoneDepth >= 1 && zoneDepth <= 99 ?
        zoneDepth : monsterLevel;
}
// Felloak is the lowest available unique; keep chest drops unique at low depth.
constexpr int kChestMinimumUniqueItemLevel = 4;
constexpr int ChestUniqueItemLevel(int itemLevel) noexcept {
    return itemLevel >= 1 && itemLevel < kChestMinimumUniqueItemLevel ?
        kChestMinimumUniqueItemLevel : itemLevel;
}
constexpr int kChestUniqueSearchBonus = 12;
constexpr unsigned ChestRewardCount(std::uint32_t roll) noexcept {
    return 2u + roll % 3u;
}
constexpr unsigned BossCorruptedUniqueCount() noexcept { return 3; }
constexpr bool ChestNaturalDropAllowed(int quality, bool potion) noexcept {
    return quality == 7 || potion;
}
constexpr bool InsideRoomMargin(int x, int y, int minX, int minY,
    int maxX, int maxY, int margin) noexcept {
    return margin >= 0 && minX <= maxX && minY <= maxY &&
        std::int64_t(x) - minX >= margin &&
        std::int64_t(maxX) - x >= margin &&
        std::int64_t(y) - minY >= margin &&
        std::int64_t(maxY) - y >= margin;
}
// Drop tiers follow zone item level; starter rewards must remain equippable.
constexpr bool RewardBaseLevelsEligible(int baseLevel, int requiredLevel,
    int itemLevel, int playerLevel, bool starter,
    bool ignoreBaseLevel = false) noexcept {
    return (ignoreBaseLevel || baseLevel <= itemLevel) &&
        (!starter || requiredLevel <= playerLevel);
}
// Rune tier is run-depth scaled even when its vanilla base level is higher.
constexpr bool MaterialBaseLevelEligible(int baseLevel, int itemLevel,
    bool rune) noexcept {
    return rune || baseLevel <= itemLevel;
}
// Preserve the preferred 10-level band without exceeding available base tiers.
constexpr int RewardBaseLevelFloor(int itemLevel,
    int highestEligibleBaseLevel, bool equipment, bool starter) noexcept {
    if (!equipment || starter || itemLevel <= 11) return 0;
    const int preferred = itemLevel - 10;
    return highestEligibleBaseLevel >= 0 &&
            highestEligibleBaseLevel < preferred ?
        highestEligibleBaseLevel : preferred;
}
constexpr bool IsValidWssCorruptionMarker(int marker) noexcept {
    return marker >= 2 && marker <= 92;
}
constexpr bool IsCertainWssPropertyChance(int chance) noexcept {
    return chance == 0 || chance >= 100;
}
constexpr int kWssUseItemOutputType = 254;
constexpr int kWssItemOutputType = 252;
constexpr int kWssCorruptorValue = 1001;
constexpr bool IsWssBaseCode(const char (&code)[4]) noexcept {
    return code[0] == 'w' && code[1] == 's' && code[2] == 's' &&
        (code[3] == ' ' || code[3] == '\0');
}
constexpr bool IsWssCorruptedUnique(int quality, int corrupted,
    int corruptor, int expectedMarker) noexcept {
    return quality == 7 && IsValidWssCorruptionMarker(expectedMarker) &&
        corrupted == expectedMarker && corruptor == kWssCorruptorValue;
}
constexpr bool IsValidWssCorruptionOutput(int outputType, int quality,
    int marker, int corruptor, int effectCount,
    bool allChancesCertain) noexcept {
    return outputType == kWssUseItemOutputType && quality == 0 &&
        IsValidWssCorruptionMarker(marker) &&
        corruptor == kWssCorruptorValue && effectCount > 0 &&
        allChancesCertain;
}
// The WSS pre-roll uses a ranged corruptnum; 1001 is applied by its outcome.
constexpr bool IsValidWssCorruptnumOutput(int outputType, int quality,
    int propertyCount, int markerCount, int corruptorCount,
    int effectCount, int corruptor, int minimum, int maximum,
    bool allChancesCertain) noexcept {
    return outputType == kWssUseItemOutputType && quality == 0 &&
        propertyCount == 1 && markerCount == 0 && corruptorCount == 1 &&
        effectCount == 0 && corruptor == -1 && minimum >= 1 &&
        maximum <= 1000 && minimum <= maximum && allChancesCertain;
}
constexpr bool IsValidWssRandomizerItemReturn(int outputType, int itemId,
    int quality, int expectedWssId, int quantity,
    int thirdOutputType) noexcept {
    return outputType == kWssItemOutputType && expectedWssId >= 0 &&
        itemId == expectedWssId && quality == 0 && quantity == 1 &&
        thirdOutputType == 0;
}
struct WssInputFacts {
    std::uint16_t flags;
    std::uint8_t requiredQuality;
    std::uint8_t actualQuality;
    bool codeMatches;
    bool typeMatches;
    bool uniqueIdMatches;
    bool quantityMatches;
    bool socketed;
    bool ethereal;
    bool runeword;
    bool normalTier;
    bool exceptionalTier;
    bool eliteTier;
    bool upgradedCodeMatches;
};
constexpr bool WssInputMatches(const WssInputFacts& input) noexcept {
    if (input.flags & 0xf000) return false;
    if ((input.flags & 0x001) && !input.codeMatches) return false;
    if ((input.flags & 0x002) && !input.typeMatches) return false;
    if (input.requiredQuality &&
        input.actualQuality != input.requiredQuality) return false;
    if (!input.quantityMatches) return false;
    if ((input.flags & 0x004) && input.socketed) return false;
    if ((input.flags & 0x008) && !input.socketed) return false;
    if ((input.flags & 0x010) && !input.ethereal) return false;
    if ((input.flags & 0x020) && input.ethereal) return false;
    if ((input.flags & 0x040) && !input.uniqueIdMatches) return false;
    if ((input.flags & 0x080) && !input.upgradedCodeMatches) return false;
    if ((input.flags & 0x100) && !input.normalTier) return false;
    if ((input.flags & 0x200) && !input.exceptionalTier) return false;
    if ((input.flags & 0x400) && !input.eliteTier) return false;
    if ((input.flags & 0x800) && input.runeword) return false;
    return true;
}
constexpr unsigned WssThresholdWeight(int previousThreshold, int threshold,
    int rollMinimum, int rollMaximum) noexcept {
    if (rollMinimum > rollMaximum) return 0;
    const int first = previousThreshold + 1 > rollMinimum ?
        previousThreshold + 1 : rollMinimum;
    const int last = threshold < rollMaximum ? threshold : rollMaximum;
    return last < first ? 0u :
        static_cast<unsigned>(last - first + 1);
}
constexpr int WssThresholdCoverage(int previousThreshold,
    int threshold) noexcept {
    return threshold > previousThreshold ? threshold : previousThreshold;
}
constexpr int WssUniqueSearchLevel(int itemLevel, int bonus) noexcept {
    if (itemLevel < 1) return 0;
    if (itemLevel >= 99 || bonus >= 99 - itemLevel) return 99;
    return itemLevel + (bonus > 0 ? bonus : 0);
}
struct NativeUniqueOutput {
    int id = -1;
    int idCount = 0;
    int baseId = -1;
    int expectedBaseId = -1;
    int level = 0;
    int requiredLevel = 0;
    bool enabled = false;
};
constexpr bool IsNativeUniqueOutputEligible(
    const NativeUniqueOutput& output, int itemLevel, int playerLevel,
    int bonus) noexcept {
    return output.enabled && output.id >= 0 && output.id < output.idCount &&
        output.baseId >= 0 && output.baseId == output.expectedBaseId &&
        itemLevel >= 1 && itemLevel <= 99 &&
        playerLevel >= 1 && playerLevel <= 99 &&
        output.level <= WssUniqueSearchLevel(itemLevel, bonus) &&
        output.requiredLevel <= playerLevel;
}
constexpr bool WssUniqueIdExcluded(int uniqueId, const int* excluded,
    unsigned count) noexcept {
    if (!excluded) return false;
    for (unsigned index = 0; index < count; ++index)
        if (excluded[index] == uniqueId) return true;
    return false;
}
constexpr std::uint32_t kIdentifiedItemFlag = 0x10;
constexpr bool IsIdentified(std::uint32_t itemFlags) noexcept {
    return (itemFlags & kIdentifiedItemFlag) != 0;
}
constexpr bool ShouldIdentifyDrop(int quality) noexcept {
    return quality < 4 || quality > 7;
}
constexpr unsigned StartingDurability(unsigned maximum,
    std::uint32_t roll) noexcept {
    const unsigned minimum = maximum / 2u + maximum % 2u;
    return minimum + roll % (maximum - minimum + 1u);
}

constexpr std::uint32_t RuneDropChance(int characterLevel) {
    const std::uint32_t level = characterLevel < 1 ? 1u :
        characterLevel > 99 ? 99u : static_cast<std::uint32_t>(characterLevel);
    return 2500u + 7500u * (level - 1u) / 98u;
}

constexpr bool DropsRune(int characterLevel, std::uint32_t roll) {
    return roll < RuneDropChance(characterLevel);
}

enum class RerollEffect : std::uint8_t {
    None = 0, Random, BossZone, MerchantZone, ShrineZone, ChestZone,
    EquipmentReward, SocketablesReward, UniqueReward, SetReward,
    JewelryReward, CharmsReward
};
constexpr RerollEffect RerollEffectForCode(const char* code) noexcept {
    if (!code || (code[3] != ' ' && code[3] != '\0'))
        return RerollEffect::None;
    if (code[0] == 'r' && code[1] == 'r' && code[2] == 'l')
        return RerollEffect::Random;
    if (code[0] == 'q' && code[1] == 'b' && code[2] == 's')
        return RerollEffect::BossZone;
    if (code[0] == 'q' && code[1] == 'm' && code[2] == 't')
        return RerollEffect::MerchantZone;
    if (code[0] == 'q' && code[1] == 's' && code[2] == 'h')
        return RerollEffect::ShrineZone;
    if (code[0] == 'q' && code[1] == 'c' && code[2] == 'h')
        return RerollEffect::ChestZone;
    if (code[0] == 'q' && code[1] == 'e' && code[2] == 'q')
        return RerollEffect::EquipmentReward;
    if (code[0] == 'q' && code[1] == 's' && code[2] == 'o')
        return RerollEffect::SocketablesReward;
    if (code[0] == 'q' && code[1] == 'u' && code[2] == 'n')
        return RerollEffect::UniqueReward;
    if (code[0] == 'q' && code[1] == 's' && code[2] == 'e')
        return RerollEffect::SetReward;
    if (code[0] == 'q' && code[1] == 'j' && code[2] == 'w')
        return RerollEffect::JewelryReward;
    if (code[0] == 'q' && code[1] == 'c' && code[2] == 'm')
        return RerollEffect::CharmsReward;
    return RerollEffect::None;
}

enum class RewardFocus : std::uint8_t {
    Equipment = 0, Socketables = 1, Unique = 2, Set = 3,
    Jewelry = 4, Charms = 5
};
constexpr unsigned RewardFocusIndex(unsigned count,
    std::uint32_t roll) noexcept {
    if (!count || count > 6) return 0;
    const unsigned sample = roll < kBasisPoints ?
        roll : kBasisPoints - 1;
    return static_cast<unsigned>(
        std::uint64_t(sample) * count / kBasisPoints);
}
constexpr RewardFocus RewardFocusForRoll(const RewardFocus* available,
    unsigned count, std::uint32_t roll) noexcept {
    if (!available || !count || count > 6) return RewardFocus::Equipment;
    return available[RewardFocusIndex(count, roll)];
}


constexpr bool TypeAllowsQuality(int quality, bool alwaysNormal,
    bool alwaysMagic, bool canRare) noexcept {
    switch (quality) {
    case 2: return !alwaysMagic;
    case 4: return !alwaysNormal;
    case 6: return canRare;
    default: return true;
    }
}
constexpr int RewardQualityAttempt(int requested, unsigned attempt,
    bool whiteOnly) noexcept {
    if (requested != 2 && requested != 4 && requested != 6) return -1;
    if (whiteOnly)
        return attempt == 0 && requested == 2 ? 2 : -1;
    if (attempt == 0) return requested;
    if (requested == 2) return attempt == 1 ? 4 : -1;
    if (requested == 4) return attempt == 1 ? 2 : -1;
    return attempt == 1 ? 4 : attempt == 2 ? 2 : -1;
}
enum class BaseKind : std::uint8_t { Equipment, Jewelry, Charm, Jewel };

constexpr bool RewardSpecialBaseCode(const char* code,
    BaseKind kind, int charmSize) noexcept {
    if (!code || (code[3] != '\0' && code[3] != ' ')) return false;
    switch (kind) {
    case BaseKind::Jewelry:
        return (code[0] == 'r' && code[1] == 'i' && code[2] == 'n') ||
            (code[0] == 'a' && code[1] == 'm' && code[2] == 'u');
    case BaseKind::Charm:
        return code[0] == 'c' && code[1] == 'm' &&
            code[2] >= '1' && code[2] <= '3' &&
            (!charmSize || code[2] == '0' + charmSize);
    case BaseKind::Jewel:
        return code[0] == 'j' && code[1] == 'e' && code[2] == 'w';
    case BaseKind::Equipment:
        return false;
    }
    return false;
}
enum class DropKind : std::uint8_t {
    None, Equipment, SocketedEquipment, Gem, Rune, Unique, Set,
    Socketable, Jewelry, Charms
};

struct DropGroup {
    DropKind kind;
    std::uint8_t count;
};

struct MonsterDropPlan {
    DropGroup primary;
    DropGroup secondary;
};

constexpr unsigned FourItemChanceAtDepth(int depth) noexcept {
    const unsigned boundedDepth = depth < 1 ? 1u :
        depth > 99 ? 99u : static_cast<unsigned>(depth);
    return 2500u + 2500u * (boundedDepth - 1u) / 98u;
}

constexpr unsigned MonsterDropCountAtDepth(int depth,
    std::uint32_t roll) noexcept {
    const unsigned sample = roll < kBasisPoints ?
        roll : kBasisPoints - 1;
    const unsigned fourChance = FourItemChanceAtDepth(depth);
    const unsigned otherChance = kBasisPoints - fourChance;
    if (sample >= otherChance) return 4;
    return 1u + static_cast<unsigned>(
        std::uint64_t(sample) * 3u / otherChance);
}

constexpr MonsterDropPlan PlanMonsterDrops(RewardFocus focus,
    std::uint8_t primaryCount) noexcept {
    if (!primaryCount || primaryCount > 4)
        return {{DropKind::None, 0}, {DropKind::None, 0}};
    constexpr std::uint8_t three = 3;
    switch (focus) {
    case RewardFocus::Equipment:
        return {{DropKind::Equipment, primaryCount}, {DropKind::None, 0}};
    case RewardFocus::Socketables:
        return {{DropKind::SocketedEquipment, primaryCount},
            {DropKind::Socketable, three}};
    case RewardFocus::Unique:
        return {{DropKind::Unique, primaryCount}, {DropKind::None, 0}};
    case RewardFocus::Set:
        return {{DropKind::Set, primaryCount}, {DropKind::None, 0}};
    case RewardFocus::Jewelry:
        return {{DropKind::Jewelry, primaryCount}, {DropKind::None, 0}};
    case RewardFocus::Charms:
        return {{DropKind::Charms, primaryCount}, {DropKind::None, 0}};
    }
    return {{DropKind::None, 0}, {DropKind::None, 0}};
}
} // namespace RoguelikeLoot
