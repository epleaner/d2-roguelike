#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
// A run level is progression depth, not a DRLG area ID or a map item's ilvl.
// Map property IDs retain their S13 meanings; selected arenas feed the
// pinned native map-world and map-monster consumers.
namespace RoguelikeAffixes {
enum class Quality { Normal, Magic, Rare };
struct QualityWeights {
    unsigned normal = 0;
    unsigned magic = 0;
    unsigned rare = 0;
};

inline QualityWeights QualityAtDepth(unsigned depth) {
    const unsigned boundedDepth = depth < 1 ? 1 : depth > 99 ? 99 : depth;
    const unsigned progress = boundedDepth - 1;
    const unsigned normal = 9000 - 8900 * progress / 98;
    const unsigned rare = 100 + 8900 * progress * progress / (98 * 98);
    return {normal, 10000 - normal - rare, rare};
}

inline Quality QualityFromRoll(unsigned depth, unsigned roll) {
    const QualityWeights weights = QualityAtDepth(depth);
    if (roll < weights.normal) return Quality::Normal;
    if (roll < weights.normal + weights.magic) return Quality::Magic;
    return Quality::Rare;
}

template <class Random>
Quality RollQuality(unsigned depth, Random& random) {
    return QualityFromRoll(
        depth, std::uniform_int_distribution<unsigned>(0, 9999)(random));
}


// Tier mask bits correspond to t1m, t2m, t3m, and t4m respectively.
// The caller supplies spawnable stock map-affix rows (and any zone-only row).
struct Candidate {
    unsigned id;
    unsigned group;
    unsigned tierMask;
    unsigned weight;
    bool rareAllowed;
    bool prefix;
};

struct Modifier { unsigned id = 0; };
struct NativeStat {
    unsigned mapStat = 0;
    unsigned layer = 0;
    unsigned category = 0;
    int value = 0;
};
struct Roll {
    Quality quality = Quality::Normal;
    std::array<Modifier, 3> modifiers{};
    unsigned count = 0;
    std::array<NativeStat, 48> stats{};
    unsigned statCount = 0;
    int areaLevel = 0;
    int densityPercent = 0;
    int magicFind = 0;
    int monsterHealthPercent = 0;
};
// S13 Warlord of Blood's native map-global corruption properties. These
// enable PD2's own boss-drop corruption roll; they are not item modifiers.
inline constexpr NativeStat kWarlordMapCorruption{503, 0, 2006, 100};
inline constexpr NativeStat kWarlordBossCorruptedUnique{187, 0, 2008, 1};

inline bool EnsureWarlordBossCorruption(Roll& roll) noexcept {
    if (roll.statCount > roll.stats.size()) return false;
    unsigned mapCorruptionCount = 0, bossUniqueCount = 0;
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const NativeStat& stat = roll.stats[i];
        if (stat.mapStat == kWarlordMapCorruption.mapStat) {
            if (stat.layer != kWarlordMapCorruption.layer ||
                stat.category != kWarlordMapCorruption.category ||
                stat.value != kWarlordMapCorruption.value) return false;
            ++mapCorruptionCount;
        } else if (stat.mapStat == kWarlordBossCorruptedUnique.mapStat) {
            if (stat.layer != kWarlordBossCorruptedUnique.layer ||
                stat.category != kWarlordBossCorruptedUnique.category ||
                stat.value != kWarlordBossCorruptedUnique.value) return false;
            ++bossUniqueCount;
        }
    }
    if (mapCorruptionCount == 1 && bossUniqueCount == 1) return true;
    if (mapCorruptionCount || bossUniqueCount ||
        roll.stats.size() - roll.statCount < 2) return false;
    roll.stats[roll.statCount++] = kWarlordMapCorruption;
    roll.stats[roll.statCount++] = kWarlordBossCorruptedUnique;
    return true;
}


// Zone depth starts at one in the authored first arena. The rolled area bonus
// changes native monster/area level, while the stock map-item affix evaluator
// keeps its own independent item level.
inline int ZoneLevel(unsigned depth, int areaBonus) {
    const int level = static_cast<int>(depth) + areaBonus;
    return level > 99 ? 99 : level;
}

// PD2's map-world density consumer multiplies single-precision values and
// truncates to an integer. Integer percentage math can disagree by one (17%).
inline int NativeDensity(int baseline, int percent) {
    const float multiplier = 1.0f + static_cast<float>(percent) / 100.0f;
    return static_cast<int>(static_cast<float>(baseline) * multiplier);
}

inline const char* Name(Quality quality) {
    switch (quality) {
    case Quality::Normal: return "Normal";
    case Quality::Magic: return "Magic";
    case Quality::Rare: return "Rare";
    }
    return "Unknown";
}

template <class Random>
Roll Generate(unsigned depth, const Candidate* pool, std::size_t poolCount, Random& random) {
    Roll result;
    if (depth == 0 || pool == nullptr || poolCount == 0) return result;
    const unsigned tier =
        depth <= 15 ? 0u : depth <= 30 ? 1u : depth <= 50 ? 2u : 3u;
    const unsigned tierBit = 1u << tier;
    result.quality = RollQuality(depth, random);
    if (result.quality == Quality::Normal) return result;
    const unsigned target = result.quality == Quality::Magic ?
        static_cast<unsigned>(std::uniform_int_distribution<int>(1, 2)(random)) :
        static_cast<unsigned>(std::uniform_int_distribution<int>(2, 3)(random));
    std::array<const Candidate*, 3> selected{};
    unsigned prefixCount = 0, suffixCount = 0;
    while (result.count < target) {
        const auto eligible = [&](const Candidate& candidate) {
            if (!(candidate.tierMask & tierBit) || !candidate.weight ||
                (result.quality == Quality::Rare && !candidate.rareAllowed) ||
                (result.quality == Quality::Magic &&
                 (candidate.prefix ? prefixCount : suffixCount))) return false;
            for (unsigned i = 0; i < result.count; ++i)
                if (selected[i]->group == candidate.group) return false;
            return true;
        };
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < poolCount; ++i)
            if (eligible(pool[i])) total += pool[i].weight;
        if (!total) break;
        std::uint64_t choice = std::uniform_int_distribution<std::uint64_t>(1, total)(random);
        for (std::size_t i = 0; i < poolCount; ++i) {
            const Candidate& candidate = pool[i];
            if (!eligible(candidate)) continue;
            if (choice > candidate.weight) {
                choice -= candidate.weight;
                continue;
            }
            selected[result.count] = &candidate;
            result.modifiers[result.count++] = {candidate.id};
            if (candidate.prefix) ++prefixCount;
            else ++suffixCount;
            break;
        }
    }
    return result;
}
} // namespace RoguelikeAffixes
