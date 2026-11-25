#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace RoguelikeProgression {
struct VanillaArea {
    unsigned id = 0;
    unsigned selections = 0;
    std::array<std::uint16_t, 3> levels{};
    std::array<std::uint32_t, 3> densities{};
    const std::uint16_t* normalRoster = nullptr;
    const std::uint16_t* nightmareRoster = nullptr;
    unsigned act = 0; // Zero-based campaign Act, not the destination's engine Act.
};

struct EncounterSelection {
    std::array<std::uint16_t, 4> classes{};
    unsigned count = 0;
    unsigned eligibleClasses = 0;
    unsigned areaLevel = 0;
    unsigned density = 0;
    std::array<std::uint8_t, 4> sourceActs{};
};
constexpr unsigned allCampaignActs = 0x1F;
constexpr unsigned ActBit(unsigned act) noexcept {
    return act < 5 ? 1u << act : 0u;
}

struct ArenaSlot {
    int level = 0;
    unsigned act = 0;
    int layout = -1;
};

// Callers shuffle the slots once. Reserving a slot cannot consume any of the
// other slots with the same layout or accidentally offer one destination twice.
class FreshArenas {
public:
    void Add(ArenaSlot arena) {
        if (arena.layout < 0) arena.layout = static_cast<int>(count_);
        slots_[count_++] = arena;
    }
    template <class Random>
    void Shuffle(Random& random) {
        std::shuffle(slots_.begin(), slots_.begin() + count_, random);
    }
    unsigned AvailableActs() const {
        unsigned acts = 0;
        for (std::size_t i = 0; i < count_; ++i)
            if (slots_[i].level) acts |= ActBit(slots_[i].act);
        return acts;
    }
    // The first slot for a chosen layout in the shuffled list is itself uniform.
    template <class Random>
    int TakeLayout(unsigned act, Random& random) {
        std::array<bool, 121> seen{};
        ArenaSlot* selected = nullptr;
        unsigned layouts = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            auto& slot = slots_[i];
            if (!slot.level || slot.act != act || seen[slot.layout]) continue;
            seen[slot.layout] = true;
            if (std::uniform_int_distribution<unsigned>(1, ++layouts)(random) == 1)
                selected = &slot;
        }
        if (!selected) return 0;
        const int level = selected->level;
        selected->level = 0;
        return level;
    }
    int Take(unsigned act) {
        for (std::size_t i = 0; i < count_; ++i) {
            auto& slot = slots_[i];
            if (slot.level && slot.act == act) {
                const int level = slot.level;
                slot.level = 0;
                return level;
            }
        }
        return 0;
    }
private:
    std::array<ArenaSlot, 121> slots_{};
    std::size_t count_ = 0;
};

inline constexpr unsigned maximumMonsterTypes = 4;
constexpr unsigned MaximumMonsterTypesForDepth(unsigned depth) noexcept {
    return depth <= 20 ? 2 : (depth <= 40 ? 3 : maximumMonsterTypes);
}

template <class Random>
unsigned RollMonsterTypeCount(unsigned depth, unsigned availableTypes,
    Random& random) {
    if (!depth || !availableTypes) return 0;
    const unsigned depthMaximum = MaximumMonsterTypesForDepth(depth);
    const unsigned maximum = availableTypes < depthMaximum ?
        availableTypes : depthMaximum;
    return std::uniform_int_distribution<unsigned>(1, maximum)(random);
}
inline constexpr unsigned minimumMobPackSize = 3;

constexpr unsigned MissingMobPackMembers(unsigned livingHostiles,
    unsigned requiredHostiles = minimumMobPackSize) noexcept {
    return livingHostiles < requiredHostiles ?
        requiredHostiles - livingHostiles : 0;
}

// Scan vanilla campaign rows in level order, but sample each class only once.
// Reservoir sampling and the seen bitset keep storage fixed and avoid heap work.
constexpr unsigned classCapacity = 1u << 16;
constexpr unsigned rosterSize = 25;
constexpr unsigned maximumDensity = 100000;
using SeenClasses = std::array<std::uint64_t, classCapacity / 64>;

template <class Random>
void SampleUniqueClass(std::uint16_t classId, unsigned sourceAct, SeenClasses& seen,
    Random& random, EncounterSelection& selection) {
    const std::uint64_t mask = std::uint64_t{1} << (classId % 64);
    std::uint64_t& word = seen[classId / 64];
    if (word & mask) return;
    word |= mask;
    ++selection.eligibleClasses;
    if (selection.count < selection.classes.size()) {
        selection.sourceActs[selection.count] = static_cast<std::uint8_t>(sourceAct);
        selection.classes[selection.count++] = classId;
        return;
    }
    const unsigned replacement = std::uniform_int_distribution<unsigned>(
        1, selection.eligibleClasses)(random);
    if (replacement <= selection.classes.size()) {
        selection.classes[replacement - 1] = classId;
        selection.sourceActs[replacement - 1] = static_cast<std::uint8_t>(sourceAct);
    }
}

template <class Spawnable, class Random>
bool CollectRoster(const std::uint16_t* roster, unsigned sourceAct, unsigned validClasses,
    Spawnable& spawnable, const std::array<std::uint16_t, 2>& excludedClasses,
    unsigned excludedCount, SeenClasses& seen, Random& random,
    EncounterSelection& selection) {
    bool hasSpawnable = false;
    for (unsigned slot = 0; slot < rosterSize; ++slot) {
        const std::uint16_t classId = roster[slot];
        if (slot && !classId) break;
        if (classId == 0xFFFF || classId >= validClasses) break;
        if (!spawnable(classId)) continue;
        hasSpawnable = true;
        bool excluded = false;
        for (unsigned index = 0; index < excludedCount; ++index)
            excluded = excluded || classId == excludedClasses[index];
        if (!excluded && ActBit(sourceAct))
            SampleUniqueClass(classId, sourceAct, seen, random, selection);
    }
    return hasSpawnable;
}

inline void RecordAreaDensity(unsigned areaLevel, unsigned density,
    EncounterSelection& selection) {
    if (areaLevel > selection.areaLevel) {
        selection.areaLevel = areaLevel;
        selection.density = density;
    } else if (areaLevel == selection.areaLevel &&
        density < selection.density) {
        selection.density = density;
    }
}

template <class Spawnable, class Random>
bool SelectVanillaEncounters(const VanillaArea* areas, std::size_t areaCount,
    unsigned depth, unsigned classLimit, Spawnable spawnable, Random& random,
    EncounterSelection& selection,
    std::array<std::uint16_t, 2> excludedClasses = {},
    unsigned excludedCount = 0, unsigned availableActs = allCampaignActs) {
    selection = {};
    if (!areas || !depth || !classLimit || excludedCount > excludedClasses.size())
        return false;

    const unsigned depthLimit = depth < 99 ? depth : 99;
    const unsigned validClasses = classLimit < classCapacity ?
        classLimit : classCapacity;
    SeenClasses seen{};
    for (std::size_t areaIndex = 0; areaIndex < areaCount; ++areaIndex) {
        const VanillaArea& area = areas[areaIndex];
        if (!area.id || area.id == 20 || area.id >= 138 || !area.selections ||
            !area.normalRoster || !area.nightmareRoster)
            continue;
        for (unsigned difficulty = 0; difficulty < 3; ++difficulty) {
            const unsigned areaLevel = area.levels[difficulty];
            const unsigned density = area.densities[difficulty];
            if (!areaLevel || areaLevel > depthLimit || !density ||
                density > maximumDensity)
                continue;
            const std::uint16_t* roster = difficulty == 0 ?
                area.normalRoster : area.nightmareRoster;
            const unsigned act = (availableActs & ActBit(area.act)) ? area.act : 5u;
            if (CollectRoster(roster, act, validClasses, spawnable, excludedClasses,
                    excludedCount, seen, random, selection))
                RecordAreaDensity(areaLevel, density, selection);
        }
    }

    if (selection.count && selection.density) {
        const unsigned candidateCount = selection.count;
        const unsigned rosterCount =
            RollMonsterTypeCount(depth, candidateCount, random);
        for (unsigned index = 0; index < rosterCount; ++index) {
            const unsigned selected = std::uniform_int_distribution<unsigned>(
                index, candidateCount - 1)(random);
            const std::uint16_t current = selection.classes[index];
            selection.classes[index] = selection.classes[selected];
            selection.classes[selected] = current;
            const auto act = selection.sourceActs[index];
            selection.sourceActs[index] = selection.sourceActs[selected];
            selection.sourceActs[selected] = act;
        }
        selection.count = rosterCount;
        return true;
    }
    selection = {};
    return false;
}
enum class OfferKind : std::uint8_t {
    Monster, Market, Sanctuary, Chest, Boss
};

struct OfferWeights {
    std::uint16_t monster = 0;
    std::uint16_t market = 0;
    std::uint16_t sanctuary = 0;
    std::uint16_t chest = 0;
    std::uint16_t boss = 0;
};

inline constexpr unsigned offerBasisPoints = 10000;

constexpr unsigned OfferWeightForDepth(unsigned depth) {
    const unsigned bounded = depth == 0 ? 1 : (depth > 30 ? 30 : depth);
    return 500 + ((bounded - 1) * 1000 + 14) / 29;
}

constexpr OfferWeights OfferWeightsForDepth(unsigned depth, bool allowMarket) {
    const unsigned ramp = OfferWeightForDepth(depth);
    const unsigned market = allowMarket ? ramp : 0;
    const unsigned sanctuary = ramp;
    const unsigned chest = depth < 5 ? 0 : 600;
    const unsigned boss = depth < 5 ? 0 : 1400;
    const unsigned monster = offerBasisPoints - market - sanctuary - chest - boss;
    return {static_cast<std::uint16_t>(monster),
        static_cast<std::uint16_t>(market),
        static_cast<std::uint16_t>(sanctuary),
        static_cast<std::uint16_t>(chest),
        static_cast<std::uint16_t>(boss)};
}

constexpr OfferKind ChooseOfferKind(unsigned depth, unsigned roll,
    bool allowMarket) {
    const OfferWeights weights = OfferWeightsForDepth(depth, allowMarket);
    unsigned remaining = roll % offerBasisPoints;
    if (remaining < weights.monster) return OfferKind::Monster;
    remaining -= weights.monster;
    if (remaining < weights.market) return OfferKind::Market;
    remaining -= weights.market;
    if (remaining < weights.sanctuary) return OfferKind::Sanctuary;
    remaining -= weights.sanctuary;
    if (remaining < weights.chest) return OfferKind::Chest;
    return OfferKind::Boss;
}

constexpr bool CanOfferKind(OfferKind kind, bool allowMarket) {
    return kind != OfferKind::Market || allowMarket;
}

constexpr int SelectFreshRunRoot(bool towerFresh,
    const std::array<bool, 255>& arenaFresh) {
    if (towerFresh) return 20;
    for (unsigned level = 13; level <= 254; ++level) {
        if (level == 17) level = 138;
        if (arenaFresh[level]) return static_cast<int>(level);
    }
    return 0;
}

} // namespace RoguelikeProgression
