#pragma once

#include "RoguelikeAffixes.h"
#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace RoguelikePortalLabels {
enum class Kind { Monster, Boss, Merchant, Shrine, Chest, TownEntry, Resume };
enum class Unit { None, Number, Percent };

struct Request {
    Kind kind = Kind::Monster;
    RoguelikeAffixes::Quality quality = RoguelikeAffixes::Quality::Normal;
    const wchar_t* monsterName = nullptr;
    const wchar_t* monsterPrefix = nullptr;
    const wchar_t* monsterSuffix = nullptr;
    int areaLevel = 0;
    int density = 0;
    const RoguelikeAffixes::NativeStat* stats = nullptr;
    unsigned statCount = 0;
    const wchar_t* reward = nullptr;
    int activeLevel = 0;
};

struct StatEffect {
    unsigned mapStat;
    const wchar_t* prefix;
    const wchar_t* suffix;
    Unit unit;
};

// These are the map-stat IDs emitted by the authenticated S13 affix pool.
// Values are the resolved native stat integers; categorical properties have
// no numeric display value and retain their native effect wording.
inline constexpr StatEffect kStatEffects[] = {
    {370, L"Magic Find ", L"", Unit::Percent},
    {371, L"Gold Find ", L"", Unit::Percent},
    {372, L"Monster Density ", L"", Unit::Percent},
    {373, L"Experience ", L"", Unit::Percent},
    {374, L"Area level ", L"", Unit::Number},
    {375, L"Monster Rarity ", L"", Unit::Percent},
    {388, L"Monsters have ", L" to Fire Skill Damage", Unit::Percent},
    {389, L"Monsters have ", L" to Lightning Skill Damage", Unit::Percent},
    {390, L"Monsters have ", L" to Cold Skill Damage", Unit::Percent},
    {391, L"Monsters have ", L" to Poison Skill Damage", Unit::Percent},
    {392, L"Monsters have ", L" Increased Attack Speed", Unit::Percent},
    {393, L"Monsters have ", L" Faster Cast Rate", Unit::Percent},
    {394, L"Monsters have ", L" Attack Rating", Unit::Percent},
    {395, L"Monsters have ", L" Defense", Unit::Percent},
    {396, L"Monsters have ", L" Cold Absorb", Unit::Percent},
    {397, L"Monsters have ", L" Magic Absorb", Unit::Percent},
    {398, L"Monsters have ", L" Lightning Absorb", Unit::Percent},
    {399, L"Monsters have ", L" Fire Absorb", Unit::Percent},
    {400, L"Monsters have ", L" Reduced Physical Damage", Unit::Number},
    {401, L"Monsters have ", L" increased Velocity", Unit::Percent},
    {402, L"Monsters have ", L" Life Regeneration", Unit::Number},
    {403, L"Monsters have ", L" Life Leech", Unit::Percent},
    {404, L"Monsters have ", L" Faster Hit Recovery", Unit::Percent},
    {405, L"Monsters have ", L" Increased Maximum Life", Unit::Percent},
    {406, L"Monsters have ", L" Chance to Pierce", Unit::Percent},
    {407, L"Monsters have ", L" Chance of Open Wounds", Unit::Percent},
    {408, L"Monsters have ", L" Chance of Crushing Blow", Unit::Percent},
    {409, L"Monsters have ", L" Curse Duration Reduction", Unit::Percent},
    {410, L"Players have ", L" Defense", Unit::Percent},
    {411, L"Players have ", L" Faster Hit Recovery", Unit::Percent},
    {412, L"Players have ", L" Chance to Block", Unit::Percent},
    {413, L"Players have ", L" Life Regeneration", Unit::Number},
    {426, L"Monsters have ", L" Enhanced Physical Damage", Unit::Percent},
    {427, L"Melee Attacks Deal Splash Damage", L"", Unit::None},
    {428, L"Players have ", L" Fire Resistance", Unit::Percent},
    {429, L"Players have ", L" Lightning Resistance", Unit::Percent},
    {430, L"Players have ", L" Cold Resistance", Unit::Percent},
    {431, L"Players have ", L" Poison Resistance", Unit::Percent},
    {432, L"Monsters deal ", L" of Physical Damage as Extra Lightning Damage", Unit::Percent},
    {433, L"Monsters deal ", L" of Physical Damage as Extra Cold Damage", Unit::Percent},
    {434, L"Monsters deal ", L" of Physical Damage as Extra Fire Damage", Unit::Percent},
    {435, L"Monsters deal ", L" of Physical Damage as Extra Poison Damage over 5 Seconds", Unit::Percent},
    {436, L"Monsters deal ", L" of Physical Damage as Extra Magic Damage", Unit::Percent},
    {437, L"Map contains Stygian Dolls", L"", Unit::None},
    {438, L"Map contains Succubus Witches", L"", Unit::None},
    {439, L"Map contains Vampire Lords", L"", Unit::None},
    {440, L"Map contains Hell Bovines", L"", Unit::None},
    {441, L"Map contains Reanimated Horde", L"", Unit::None},
    {442, L"Map contains Ghosts", L"", Unit::None},
    {470, L"Map contains Burning Souls", L"", Unit::None},
    {471, L"Map contains Fetishes", L"", Unit::None},
    {494, L"Monsters have ", L" chance of dropping additional jewelry and charms", Unit::Percent},
    {495, L"Monsters have ", L" chance of dropping additional weapons", Unit::Percent},
    {496, L"Monsters have ", L" chance of dropping additional armor", Unit::Percent},
    {497, L"Monsters have ", L" chance of dropping additional crafting materials", Unit::Percent},
    {498, L"Map contains an additional boss", L"", Unit::None},
    {499, L"Map contains Minions of Destruction", L"", Unit::None},
};

inline const StatEffect* FindStatEffect(unsigned mapStat) {
    for (const StatEffect& effect : kStatEffects)
        if (effect.mapStat == mapStat) return &effect;
    return nullptr;
}

inline bool AppendText(wchar_t* output, std::size_t capacity,
    std::size_t& length, const wchar_t* text) {
    if (!text) return false;
    while (*text) {
        if (length + 1 >= capacity) return false;
        output[length++] = *text++;
    }
    output[length] = L'\0';
    return true;
}

inline bool AppendChar(wchar_t* output, std::size_t capacity,
    std::size_t& length, wchar_t value) {
    if (length + 1 >= capacity) return false;
    output[length++] = value;
    output[length] = L'\0';
    return true;
}

inline bool AppendColor(wchar_t* output, std::size_t capacity,
    std::size_t& length, wchar_t color) {
    const wchar_t token[] = {static_cast<wchar_t>(0x00ff), L'c', color, L'\0'};
    return AppendText(output, capacity, length, token);
}

inline bool IsColorCode(wchar_t value) {
    return (value >= L'0' && value <= L'9') || value == L';';
}

inline bool AppendPlainName(wchar_t* output, std::size_t capacity,
    std::size_t& length, const wchar_t* name) {
    if (!name) return false;
    for (std::size_t index = 0; name[index];) {
        if (name[index] == static_cast<wchar_t>(0x00ff) &&
            name[index + 1] == L'c' && IsColorCode(name[index + 2])) {
            index += 3;
            continue;
        }
        if (!AppendChar(output, capacity, length, name[index++])) return false;
    }
    return true;
}
inline bool AppendOptionalMonsterPrefix(wchar_t* output,
    std::size_t capacity, std::size_t& length, const wchar_t* prefix) {
    if (!prefix || !prefix[0]) return true;
    return AppendPlainName(output, capacity, length, prefix) &&
        AppendChar(output, capacity, length, L' ');
}

inline bool StartsWithOfPhrase(const wchar_t* suffix) {
    if (!suffix || !suffix[0]) return false;
    if (suffix[0] != L'o' && suffix[0] != L'O') return false;
    if (suffix[1] != L'f' && suffix[1] != L'F') return false;
    return !suffix[2] || suffix[2] == L' ' || suffix[2] == L'\t';
}

inline bool AppendOptionalMonsterSuffix(wchar_t* output,
    std::size_t capacity, std::size_t& length, const wchar_t* suffix) {
    if (!suffix || !suffix[0]) return true;
    const wchar_t* separator = StartsWithOfPhrase(suffix) ? L" " : L" of ";
    if (!AppendText(output, capacity, length, separator)) return false;
    return AppendPlainName(output, capacity, length, suffix);
}

inline bool AppendMonsterName(wchar_t* output, std::size_t capacity,
    std::size_t& length, const Request& request) {
    const bool showAffixes = request.kind == Kind::Monster &&
        request.quality != RoguelikeAffixes::Quality::Normal;
    if (showAffixes && !AppendOptionalMonsterPrefix(
            output, capacity, length, request.monsterPrefix)) return false;
    if (!AppendPlainName(output, capacity, length, request.monsterName)) return false;
    if (showAffixes && !AppendOptionalMonsterSuffix(
            output, capacity, length, request.monsterSuffix)) return false;
    return true;
}

inline bool AppendInteger(wchar_t* output, std::size_t capacity,
    std::size_t& length, int value) {
    if (length >= capacity) return false;
    const int written = std::swprintf(output + length, capacity - length,
        L"%d", value);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity - length)
        return false;
    length += static_cast<std::size_t>(written);
    return true;
}

inline bool AppendSignedValue(wchar_t* output, std::size_t capacity,
    std::size_t& length, int value, Unit unit) {
    if (length >= capacity || unit == Unit::None) return false;
    const wchar_t* format = unit == Unit::Percent ? L"%+d%%" : L"%+d";
    const int written = std::swprintf(output + length, capacity - length,
        format, value);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity - length)
        return false;
    length += static_cast<std::size_t>(written);
    return true;
}

inline bool AppendEffect(wchar_t* output, std::size_t capacity,
    std::size_t& length, const RoguelikeAffixes::NativeStat& stat) {
    const StatEffect* effect = FindStatEffect(stat.mapStat);
    if (!effect || !AppendChar(output, capacity, length, L'\n') ||
        !AppendText(output, capacity, length, effect->prefix)) return false;
    if (effect->unit != Unit::None &&
        !AppendSignedValue(output, capacity, length, stat.value, effect->unit))
        return false;
    return AppendText(output, capacity, length, effect->suffix);
}

inline wchar_t QualityColor(RoguelikeAffixes::Quality quality) {
    switch (quality) {
    case RoguelikeAffixes::Quality::Normal: return L'0';
    case RoguelikeAffixes::Quality::Magic: return L'3';
    case RoguelikeAffixes::Quality::Rare: return L'9';
    }
    return L'\0';
}

inline bool AppendColoredLabel(wchar_t* output, std::size_t capacity,
    std::size_t& length, wchar_t color, const wchar_t* label) {
    return AppendColor(output, capacity, length, color) &&
        AppendText(output, capacity, length, label) &&
        AppendColor(output, capacity, length, L'0');
}

inline bool Format(const Request& request, wchar_t* output,
    std::size_t capacity) {
    if (!output || !capacity) return false;
    output[0] = L'\0';
    std::size_t length = 0;
    const auto fail = [&]() {
        output[0] = L'\0';
        return false;
    };
    switch (request.kind) {
    case Kind::Merchant:
        return AppendColoredLabel(output, capacity, length, L'2', L"Market") || fail();
    case Kind::Shrine:
        return AppendColoredLabel(output, capacity, length, L';', L"Sanctuary") || fail();
    case Kind::Chest:
        return AppendColoredLabel(output, capacity, length, L'8', L"Treasure") || fail();
    case Kind::TownEntry:
        return AppendColoredLabel(output, capacity, length, L'1', L"Begin descent") || fail();
    case Kind::Resume:
        if (!AppendColor(output, capacity, length, L'1') ||
            !AppendText(output, capacity, length, L"Resume descent (")) return fail();
        if (!AppendInteger(output, capacity, length, request.activeLevel) ||
            !AppendText(output, capacity, length, L")") ||
            !AppendColor(output, capacity, length, L'0')) return fail();
        return true;
    case Kind::Monster:
    case Kind::Boss:
        break;
    default:
        return fail();
    }

    const wchar_t monsterColor = request.kind == Kind::Boss ? L'4' :
        QualityColor(request.quality);
    if (!monsterColor || !request.reward ||
        (request.kind == Kind::Monster &&
         ((request.statCount && !request.stats) || request.statCount > 48)))
        return fail();

    // The client renders newline-separated object-label lines bottom-to-top.
    if (!AppendText(output, capacity, length, L"Reward: ") ||
        !AppendText(output, capacity, length, request.reward)) return fail();
    if (request.kind == Kind::Monster) {
        for (unsigned index = request.statCount; index > 0; --index)
            if (!AppendEffect(output, capacity, length, request.stats[index - 1]))
                return fail();
        if (!AppendText(output, capacity, length, L"\nDensity ") ||
            !AppendInteger(output, capacity, length, request.density)) return fail();
    }
    if (!AppendText(output, capacity, length, L"\nArea level ") ||
        !AppendInteger(output, capacity, length, request.areaLevel) ||
        !AppendChar(output, capacity, length, L'\n') ||
        !AppendColor(output, capacity, length, monsterColor) ||
        !AppendMonsterName(output, capacity, length, request) ||
        !AppendColor(output, capacity, length, L'0')) return fail();
    return true;
}
} // namespace RoguelikePortalLabels
