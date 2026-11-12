#define NOMINMAX
#include "CharacterState.h"

#include "Constants.h"
#include "D2Helpers.h"
#include "D2Ptrs.h"
#include "LabProfile.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <cstring>

namespace {
using json = nlohmann::json;

// These offsets/strides are used by S13 D2Common exports 10044 (RVA 4E550),
// 10587 (38C20), 10973 (38B70), 10306 (505C0). The experience compiler
// descriptor at 5C4E0 installs a 0x20-byte row table at table offset C78.
static_assert(sizeof(void*) == 4, "S13 is an x86 game");
static_assert(offsetof(sgptDataTable, pPlayerSkillCount) == 0xBA4, "skill count ABI");
static_assert(offsetof(sgptDataTable, pPlayerSkillList) == 0xBAC, "skill slot ABI");
static_assert(offsetof(sgptDataTable, pExperience) == 0xC78, "experience ABI");
static_assert(offsetof(sgptDataTable, pCharStatsTxt) == 0xBC4, "class ABI");
static_assert(sizeof(SkillsTxt) == 0x23C, "skill row ABI");
static_assert(offsetof(SkillsTxt, wReqLevel) == 0x174, "skill requirement ABI");
static_assert(offsetof(SkillsTxt, wSkillDesc) == 0x194, "skill description ABI");
static_assert(offsetof(Skill, skillLevel) == 0x28, "skill instance ABI");
static_assert(offsetof(CharStatsTxt, bStatPerLevel) == 0x50, "class growth ABI");
static_assert(sizeof(CharStatsTxt) == 0xC4, "class row ABI");
static_assert(offsetof(CharStatsTxt, bStr) == 0x30, "class starting attributes ABI");

bool Readable(const void* pointer, size_t bytes) {
    if (!pointer || !bytes) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start > std::numeric_limits<uintptr_t>::max() - bytes) return false;
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(pointer, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    return end >= start && bytes <= end - start;
}
template <typename T> bool Valid(const T* pointer) { return Readable(pointer, sizeof(T)); }
json Error(const char* message) { return {{"ok", false}, {"error", message}}; }

sgptDataTable* Tables() {
    if (!Valid(p_D2COMMON_sgptDataTable)) return nullptr;
    sgptDataTable* tables = *p_D2COMMON_sgptDataTable;
    return Valid(tables) ? tables : nullptr;
}
bool ClassId(const json& args, unsigned& id) {
    if (!args.is_object() || !args.contains("class_id") || args.size() != 1 ||
        !args["class_id"].is_number_integer()) return false;
    const json& value = args["class_id"];
    if (value.is_number_unsigned()) {
        uint64_t number = value.get<uint64_t>();
        if (number > 6) return false;
        id = static_cast<unsigned>(number);
    } else {
        int64_t number = value.get<int64_t>();
        if (number < 0 || number > 6) return false;
        id = static_cast<unsigned>(number);
    }
    return true;
}
std::string ClassName(const CharStatsTxt& row) {
    size_t size = 0;
    while (size < sizeof(row.szClass) && row.szClass[size]) ++size;
    return std::string(row.szClass, size);
}
std::string SkillName(const sgptDataTable* tables, const SkillsTxt& skill) {
    if (skill.wSkillDesc >= tables->dwSkillDescRecs ||
        !Readable(tables->pSkillDescTxt + skill.wSkillDesc, sizeof(SkillDescTxt))) return {};
    const WORD index = tables->pSkillDescTxt[skill.wSkillDesc].wStrName;
    if (!index) return {};
    const wchar_t* wide = GetTblEntryByIndex(index, TBLOFFSET_STRING);
    if (!Readable(wide, sizeof(wchar_t))) return {};
    size_t count = 0;
    while (count < 128 && Readable(wide + count, sizeof(wchar_t)) && wide[count]) ++count;
    if (count == 128 || !Readable(wide + count, sizeof(wchar_t))) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    if (length <= 0 || length > 512) return {};
    std::string name(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(count), &name[0], length, nullptr, nullptr);
    return name;
}
// Compiled Experience.txt has a 0x20-byte max-level header followed by
// 0x20-byte level records; columns 0..6 are the seven native class IDs.
struct ExperienceRow { DWORD classThreshold[7]; DWORD ratio; };

bool BoundCode(const BYTE* address, const BYTE* signature, size_t size, HMODULE module) {
    MEMORY_BASIC_INFORMATION region = {};
    return module && Readable(address, size) && VirtualQuery(address, &region, sizeof(region)) &&
        region.AllocationBase == module &&
        (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
        !std::memcmp(address, signature, size);
}
bool ProgressionBindingsReady(HMODULE& server, HMODULE& common) {
    std::string error;
    if (!LabProfile::VerifyServer(error) || !CharacterState::BindingsReady()) return false;
    server = GetModuleHandleA("D2GAME.dll");
    common = GetModuleHandleA("D2Common.dll");
    if (!server || !common) return false;
    static const BYTE levelUp[] = {0x83,0xEC,0x08,0x85,0xF6,0x53,0x55,0x57,0x75,0x09,0x83,0xCF,0xFF,0x89,0x7C,0x24};
    static const BYTE refresh[] = {0x83,0xEC,0x20,0x85,0xDB,0x74,0x76,0x57,0x8B,0x7B,0x60,0x85,0xFF,0x74,0x6D,0x56};
    static const BYTE broadcast[] = {0x83,0xEC,0x30,0x55,0x8B,0x6C,0x24,0x3C,0x85,0xED,0x0F,0x84,0xA8,0x03,0x00,0x00};
    static const BYTE statUpdate[] = {0x85,0xDB,0x55,0x8B,0x6C,0x24,0x08,0x75,0x06,0x33,0xC0,0x5D,0xC2,0x10,0x00,0x56};
    BYTE* const gameBase = reinterpret_cast<BYTE*>(server);
    BYTE* const commonBase = reinterpret_cast<BYTE*>(common);
    const BYTE* const nativeLevelUp = gameBase + 0xEB090;
    const BYTE* const setUnitStat = commonBase + 0x3A740;
    const BYTE* const addUnitStat = commonBase + 0x3A710;
    const BYTE* const levelFromExperience = commonBase + 0x608D0;
    const BYTE* const mark = commonBase + 0x30DC0;
    const BYTE* const statUpdater = gameBase + 0x37030;
    if (!BoundCode(nativeLevelUp, levelUp, sizeof(levelUp), server) ||
        !BoundCode(gameBase + 0xD24B0, refresh, sizeof(refresh), server) ||
        !BoundCode(gameBase + 0xD51A0, broadcast, sizeof(broadcast), server) ||
        !BoundCode(statUpdater, statUpdate, sizeof(statUpdate), server)) return false;
    if (GetProcAddress(common, MAKEINTRESOURCEA(10887)) != reinterpret_cast<FARPROC>(setUnitStat) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10551)) != reinterpret_cast<FARPROC>(addUnitStat) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10670)) != reinterpret_cast<FARPROC>(levelFromExperience) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10201)) != reinterpret_cast<FARPROC>(mark)) return false;
    return true;
}
bool SaveLimit(const sgptDataTable* tables, unsigned stat, uint32_t& maximum) {
    if (!tables || !tables->dwItemStatCostRecs || tables->dwItemStatCostRecs > 4096) {
        return false;
    }
    if (stat >= tables->dwItemStatCostRecs) {
        return false;
    }
    if (!Readable(tables->pItemStatCostTxt,
        size_t(tables->dwItemStatCostRecs) * sizeof(ItemStatCostTxt))) {
        return false;
    }
    const ItemStatCostTxt& cost = tables->pItemStatCostTxt[stat];
    if (cost.wStat != stat) {
        return false;
    }
    if (!cost.bItemStatFlags.bSaved) {
        return false;
    }
    if (!cost.bCsvBits || cost.bCsvBits > 32) {
        return false;
    }
    maximum = static_cast<uint32_t>((uint64_t(1) << cost.bCsvBits) - 1);
    return true;
}
void SynchronizePointPool(void* game, UnitAny* player, HMODULE server, HMODULE common) {
    using Broadcast = void (__stdcall*)(void*, UnitAny*, int);
    using Mark = void (__stdcall*)(UnitAny*, int);
    auto* refresh = reinterpret_cast<BYTE*>(server) + 0xD24B0;
    auto broadcast = reinterpret_cast<Broadcast>(reinterpret_cast<BYTE*>(server) + 0xD51A0);
    auto mark = reinterpret_cast<Mark>(GetProcAddress(common, MAKEINTRESOURCEA(10201)));
    __asm {
        push ebx
        mov ebx, player
        mov eax, refresh
        call eax
        pop ebx
    }
    broadcast(game, player, 0);
    mark(player, 1);
}
void NotifyStatUpdate(void* game, UnitAny* player, DWORD stat, HMODULE server) {
    auto* update = reinterpret_cast<BYTE*>(server) + 0x37030;
    __asm {
        push ebx
        mov ebx, player
        push 0
        push 0
        push stat
        push game
        mov eax, update
        call eax
        pop ebx
    }
}
bool ApplyProgression(void* game, UnitAny* player,
    CharacterState::ProgressionField field, uint32_t requested, bool preserveExperience = false) {
    if (!game || !Readable(game, 0xAC) || !Valid(player) || player->dwType != UNIT_PLAYER ||
        player->dwTxtFileNo > 6 || !Valid(player->pStats) || !Valid(player->pInfo)) return false;
    HMODULE server = nullptr, common = nullptr;
    if (!ProgressionBindingsReady(server, common)) return false;
    sgptDataTable* tables = Tables();
    if (!tables) return false;
    using Setter = void (__stdcall*)(UnitAny*, DWORD, int, WORD);
    using Adder = void (__stdcall*)(UnitAny*, DWORD, int, WORD);
    using LevelFromExperience = DWORD (__stdcall*)(DWORD, DWORD);
    auto setStat = reinterpret_cast<Setter>(GetProcAddress(common, MAKEINTRESOURCEA(10887)));
    auto addStat = reinterpret_cast<Adder>(GetProcAddress(common, MAKEINTRESOURCEA(10551)));
    auto levelFromXp = reinterpret_cast<LevelFromExperience>(GetProcAddress(common, MAKEINTRESOURCEA(10670)));
    if (!setStat || !addStat || !levelFromXp) return false;
    if (field == CharacterState::ProgressionField::Level) {
        if (requested < 1 || !Readable(tables->pExperience, sizeof(ExperienceRow) * 101))
            return false;
        const auto* experience = reinterpret_cast<const ExperienceRow*>(tables->pExperience);
        const unsigned classId = player->dwTxtFileNo;
        const unsigned maximumLevel = experience[0].classThreshold[classId];
        if (maximumLevel < 1 || maximumLevel > 99 || requested > maximumLevel) return false;
        uint32_t maxExperience = 0;
        if (!SaveLimit(tables, STAT_EXP, maxExperience)) return false;
        uint32_t previous = 0;
        for (unsigned level = 1; level <= maximumLevel; ++level) {
            const uint32_t threshold = experience[level].classThreshold[classId];
            if (threshold < previous) return false;
            previous = threshold;
        }
        const DWORD currentLevel = D2COMMON_GetUnitStat(player, STAT_LEVEL, 0);
        if (currentLevel > maximumLevel) return false;
        uint32_t targetExperience = experience[requested].classThreshold[classId];
        if (preserveExperience) {
            if (currentLevel < 1 || requested != currentLevel + 1) return false;
            const uint32_t currentExperience = D2COMMON_GetUnitStat(player, STAT_EXP, 0);
            const uint32_t floor = experience[currentLevel].classThreshold[classId];
            if (currentExperience < floor || currentExperience >= targetExperience) return false;
            const uint64_t advanced = uint64_t(targetExperience) + currentExperience - floor;
            if (advanced > maxExperience) return false;
            targetExperience = static_cast<uint32_t>(advanced);
        }
        if (targetExperience > maxExperience || levelFromXp(classId, targetExperience) != requested)
            return false;
        if (requested > currentLevel) {
            if (tables->dwCharsStatsRecs < 7 || tables->dwCharsStatsRecs > 64 ||
                classId >= tables->dwCharsStatsRecs ||
                !Readable(tables->pCharStatsTxt,
                    size_t(tables->dwCharsStatsRecs) * sizeof(CharStatsTxt))) return false;
            uint32_t maxStatPoints = 0, maxSkillPoints = 0;
            if (!SaveLimit(tables, STAT_STATPOINTSLEFT, maxStatPoints) ||
                !SaveLimit(tables, STAT_NEWSKILLS, maxSkillPoints)) return false;
            const uint64_t levelsGained = requested - currentLevel;
            const uint64_t nextStatPoints =
                D2COMMON_GetUnitStat(player, STAT_STATPOINTSLEFT, 0) +
                levelsGained * tables->pCharStatsTxt[classId].bStatPerLevel;
            const uint64_t nextSkillPoints =
                D2COMMON_GetUnitStat(player, STAT_NEWSKILLS, 0) + levelsGained;
            if (nextStatPoints > maxStatPoints || nextSkillPoints > maxSkillPoints) return false;
        }
        int signedExperience = 0;
        std::memcpy(&signedExperience, &targetExperience, sizeof(targetExperience));
        setStat(player, STAT_EXP, signedExperience, 0);
        if (D2COMMON_GetUnitStat(player, STAT_EXP, 0) != targetExperience) return false;
        auto* nativeLevelUp = reinterpret_cast<BYTE*>(server) + 0xEB090;
        __asm {
            push esi
            mov esi, player
            push game
            mov eax, nativeLevelUp
            call eax
            pop esi
        }
        NotifyStatUpdate(game, player, STAT_EXP, server);
        NotifyStatUpdate(game, player, STAT_LEVEL, server);
        const bool updated = D2COMMON_GetUnitStat(player, STAT_LEVEL, 0) == requested &&
            D2COMMON_GetUnitStat(player, STAT_EXP, 0) == targetExperience;
        return updated;
    }
    const DWORD stat = field == CharacterState::ProgressionField::StatPoints ?
        STAT_STATPOINTSLEFT : STAT_NEWSKILLS;
    uint32_t maximum = 0;
    if (!SaveLimit(tables, stat, maximum) || requested > maximum) return false;
    const DWORD current = D2COMMON_GetUnitStat(player, stat, 0);
    if (current > INT32_MAX) return false;
    const int64_t difference = int64_t(requested) - current;
    if (difference < INT32_MIN || difference > INT32_MAX) return false;
    if (difference) addStat(player, stat, static_cast<int>(difference), 0);
    if (D2COMMON_GetUnitStat(player, stat, 0) != requested) return false;
    SynchronizePointPool(game, player, server, common);
    const bool updated = D2COMMON_GetUnitStat(player, stat, 0) == requested;
    return updated;
}
bool SkillChainValid(UnitAny* player) {
    if (!Valid(player) || !Valid(player->pInfo)) return false;
    Skill* current = player->pInfo->pFirstSkill;
    for (unsigned visited = 0; current; ++visited) {
        if (visited >= 4096 || !Valid(current)) return false;
        current = current->pNextSkill;
    }
    return true;
}
bool ResetHardSkillRanks(UnitAny* player) {
    for (Skill* current = player->pInfo->pFirstSkill; current; current = current->pNextSkill) {
        if (current->itemGUID == -1) current->skillLevel = 0;
    }
    for (Skill* current = player->pInfo->pFirstSkill; current; current = current->pNextSkill) {
        if (current->itemGUID == -1 && current->skillLevel) return false;
    }
    return true;
}
bool ResetBaseAttributes(void* game, UnitAny* player, const CharStatsTxt& cls,
    HMODULE server, HMODULE common) {
    using Setter = void (__stdcall*)(UnitAny*, DWORD, int, WORD);
    auto setStat = reinterpret_cast<Setter>(GetProcAddress(common, MAKEINTRESOURCEA(10887)));
    if (!setStat) return false;
    const DWORD stats[] = {STAT_STRENGTH, STAT_DEXTERITY, STAT_VITALITY, STAT_ENERGY};
    const int values[] = {cls.bStr, cls.bDex, cls.bVita, cls.bEne};
    for (size_t i = 0; i < 4; ++i) {
        setStat(player, stats[i], values[i], 0);
        if (D2COMMON_GetBaseStatSigned(player, stats[i], 0) != values[i]) return false;
        NotifyStatUpdate(game, player, stats[i], server);
    }
    return true;
}
struct ProgressionVitals {
    uint32_t level, vitality, energy;
    uint32_t maxLife, maxMana, maxStamina;
};
bool ResetProgressionVitals(void* game, UnitAny* player, const CharStatsTxt& cls,
    const ProgressionVitals& previous, HMODULE server, HMODULE common) {
    using Setter = void (__stdcall*)(UnitAny*, DWORD, int, WORD);
    auto setStat = reinterpret_cast<Setter>(GetProcAddress(common, MAKEINTRESOURCEA(10887)));
    if (!setStat || previous.level < 1 || previous.level > 99) return false;
    struct Resource {
        DWORD current, maximum, attribute;
        int baseResource, baseAttribute, levelGrowth, attributeGrowth;
        uint32_t previousAttribute, previousMaximum;
    };
    // CharStatsTxt growth is quarter-point; unit life/mana/stamina use 1/256.
    const Resource resources[] = {
        {STAT_HP, STAT_MAXHP, STAT_VITALITY, cls.bHpAdd + cls.bVita, cls.bVita,
            cls.bLifePerLevel, cls.bLifePerVitality, previous.vitality, previous.maxLife},
        {STAT_MANA, STAT_MAXMANA, STAT_ENERGY, cls.bEne, cls.bEne,
            cls.bManaPerLevel, cls.bManaPerMagic, previous.energy, previous.maxMana},
        {STAT_STAMINA, STAT_MAXSTAMINA, STAT_VITALITY, cls.bStamina, cls.bVita,
            cls.bStaminaPerLevel, cls.bStaminaPerVitality,
            previous.vitality, previous.maxStamina}};
    for (const Resource& resource : resources) {
        const int64_t baseAttribute = resource.baseAttribute;
        const int64_t oldAttribute = resource.previousAttribute;
        if (oldAttribute < baseAttribute) return false;
        const int64_t oldNatural =
            int64_t(resource.baseResource) * 256 +
            int64_t(previous.level - 1) * resource.levelGrowth * 64 +
            (oldAttribute - baseAttribute) * resource.attributeGrowth * 64;
        const int64_t bonus = int64_t(resource.previousMaximum) - oldNatural;
        const int64_t newAttribute = D2COMMON_GetUnitStat(player, resource.attribute, 0);
        if (newAttribute < baseAttribute) return false;
        const int64_t target = int64_t(resource.baseResource) * 256 +
            (newAttribute - baseAttribute) * resource.attributeGrowth * 64 + bonus;
        if (target <= 0 || target > INT32_MAX) return false;
        setStat(player, resource.maximum, static_cast<int>(target), 0);
        if (D2COMMON_GetBaseStatSigned(player, resource.maximum, 0) != target) return false;
        NotifyStatUpdate(game, player, resource.maximum, server);
        setStat(player, resource.current, static_cast<int>(target), 0);
        if (D2COMMON_GetBaseStatSigned(player, resource.current, 0) != target) return false;
        NotifyStatUpdate(game, player, resource.current, server);
    }
    return true;
}
static_assert(sizeof(ExperienceRow) == 0x20, "experience row ABI");

json Stats(UnitAny* player) {
    json base = json::object(), effective = json::object();
    struct Field { const char* name; int id; bool shifted; };
    constexpr Field fields[] = {
        {"strength", STAT_STRENGTH, false}, {"energy", STAT_ENERGY, false},
        {"dexterity", STAT_DEXTERITY, false}, {"vitality", STAT_VITALITY, false},
        {"level", STAT_LEVEL, false}, {"experience", STAT_EXP, false},
        {"stat_points", STAT_STATPOINTSLEFT, false}, {"skill_points", STAT_NEWSKILLS, false},
        {"life", STAT_HP, true}, {"max_life", STAT_MAXHP, true},
        {"mana", STAT_MANA, true}, {"max_mana", STAT_MAXMANA, true},
        {"stamina", STAT_STAMINA, true}, {"max_stamina", STAT_MAXSTAMINA, true},
        {"attack_rating", STAT_ATTACKRATING, false}, {"defense", STAT_DEFENSE, false},
        {"fire_resistance", STAT_FIRERESIST, false},
        {"cold_resistance", STAT_COLDRESIST, false},
        {"lightning_resistance", STAT_LIGHTNINGRESIST, false},
        {"poison_resistance", STAT_POISONRESIST, false},
        {"faster_cast_rate", STAT_FASTERCAST, false}
    };
    for (const Field& field : fields) {
        const int hard = D2COMMON_GetBaseStatSigned(player, field.id, 0);
        const int total = static_cast<int>(D2COMMON_GetUnitStat(player, field.id, 0));
        if (field.id == STAT_EXP) {
            base[field.name] = static_cast<uint32_t>(hard);
            effective[field.name] = static_cast<uint32_t>(total);
        } else if (field.shifted) {
            base[field.name] = hard / 256.0;
            effective[field.name] = total / 256.0;
        } else {
            base[field.name] = hard;
            effective[field.name] = total;
        }
    }
    return {{"base", std::move(base)}, {"effective", std::move(effective)}};
}
} // namespace

namespace CharacterState {
bool BindingsReady() {
    HMODULE common = GetModuleHandleA("D2Common.dll");
    if (!common || D2Version::GetGameVersionID() != VERSION_113c) return false;
    static const BYTE skill[] = {0x56,0x8b,0x74,0x24,0x08,0x85,0xf6,0x74,
        0x3d,0x8b,0x44,0x24,0x0c,0x85,0xc0,0x74,0x35,0x8b,0x4c,0x24,0x10,0x85,0xc9,0x57};
    static const BYTE state[] = {0x8b,0x44,0x24,0x04,0x85,0xc0,0x74,0x2e,
        0x8b,0x08,0x85,0xc9,0x7c,0x28,0x83,0xf9,0x01,0x7e,0x05,0x83,0xf9,0x03,0x75,0x1e};
    static const BYTE base[] = {0x8b,0x54,0x24,0x04,0x85,0xd2,0x75,0x05,
        0x33,0xc0,0xc2,0x0c,0x00,0x53,0x57,0x8b,0x7c,0x24,0x10,0x85,0xff,0x7c,0x27,0x8b};
    static const BYTE total[] = {0x8b,0x44,0x24,0x04,0x85,0xc0,0x75,0x03,
        0xc2,0x0c,0x00,0x53,0x8b,0x58,0x5c,0x85,0xdb,0x75,0x06,0x33,0xc0,0x5b,0xc2,0x0c};
    static const BYTE use[] = {0x57,0x8b,0x7c,0x24,0x0c,0x85,0xff,0x75,0x1f,0x68,
        0xea,0x08,0x00,0x00,0xe8,0xc9,0x6e,0xfb,0xff,0x50};
    static const BYTE mode[] = {0x8b,0x44,0x24,0x04,0x85,0xc0,0x74,0x0e,0x8b,0x44,
        0x24,0x08,0x85,0xc0,0x74,0x06,0x8b,0x40,0x08,0xc2};
    struct Site { unsigned ordinal; DWORD rva; const BYTE* bytes; size_t size; };
    static const Site sites[] = {
        {10306,0x505c0,skill,sizeof(skill)}, {10494,0x33cd0,state,sizeof(state)},
        {10587,0x38c20,base,sizeof(base)}, {10973,0x38b70,total,sizeof(total)},
        {10787,0x52340,use,sizeof(use)}, {10952,0x4e460,mode,sizeof(mode)}};
    for (const Site& site : sites) {
        const BYTE* address = reinterpret_cast<const BYTE*>(common) + site.rva;
        if (GetProcAddress(common, MAKEINTRESOURCEA(site.ordinal)) != reinterpret_cast<FARPROC>(address) ||
            !Readable(address, site.size) || std::memcmp(address, site.bytes, site.size))
            return false;
    }
    return true;
}
bool SetProgression(void* game, UnitAny* player, ProgressionField field, uint32_t value) {
    return ApplyProgression(game, player, field, value);
}
DeathMode GetDeathMode(UnitAny* player) {
    if (!Valid(player) || player->dwType != UNIT_PLAYER ||
        player->dwTxtFileNo > 6 || !Valid(p_D2LAUNCH_BnData)) return DeathMode::Unknown;
    BnetData* profile = *p_D2LAUNCH_BnData;
    if (!Valid(profile) || profile->nCharClass != player->dwTxtFileNo)
        return DeathMode::Unknown;
    return profile->nCharFlags & PLAYER_TYPE_HARDCORE ?
        DeathMode::Hardcore : DeathMode::Softcore;
}
struct ProgressionResetContext {
    const CharStatsTxt* classStats{};
    const ExperienceRow* experience{};
    HMODULE server{}, common{};
};
bool ResolveProgressionResetContext(unsigned classId,
    ProgressionResetContext& context, std::string& error) {
    if (!ProgressionBindingsReady(context.server, context.common)) {
        error = "native progression bindings unavailable";
        return false;
    }
    sgptDataTable* tables = Tables();
    if (!tables || tables->dwCharsStatsRecs < 7 ||
        classId >= tables->dwCharsStatsRecs ||
        !Readable(tables->pCharStatsTxt,
            size_t(tables->dwCharsStatsRecs) * sizeof(CharStatsTxt)) ||
        !Readable(tables->pExperience, sizeof(ExperienceRow) * 101)) {
        error = "native class or experience tables unavailable";
        return false;
    }
    context.classStats = &tables->pCharStatsTxt[classId];
    context.experience = reinterpret_cast<const ExperienceRow*>(tables->pExperience);
    return true;
}
bool ResetProgressionState(void* game, UnitAny* player,
    const ProgressionResetContext& context, const ProgressionVitals& previous,
    std::string& error) {
    const unsigned classId = player->dwTxtFileNo;
    if (!SkillChainValid(player)) {
        error = "player skill chain invalid";
        return false;
    }
    if (!SetProgression(game, player, ProgressionField::Level, 1)) {
        error = "native level reset did not verify";
        return false;
    }
    if (!ResetBaseAttributes(game, player, *context.classStats,
        context.server, context.common)) {
        error = "class starting attributes did not verify";
        return false;
    }
    if (!ResetHardSkillRanks(player)) {
        error = "hard skill reset did not verify";
        return false;
    }
    if (!SetProgression(game, player, ProgressionField::SkillPoints, 0) ||
        !SetProgression(game, player, ProgressionField::StatPoints, 0)) {
        error = "unspent point reset did not verify";
        return false;
    }
    if (!ResetProgressionVitals(game, player, *context.classStats, previous,
        context.server, context.common)) {
        error = "level-one resource stats did not verify";
        return false;
    }
    if (D2COMMON_GetUnitStat(player, STAT_LEVEL, 0) != 1 ||
        D2COMMON_GetUnitStat(player, STAT_EXP, 0) !=
            context.experience[1].classThreshold[classId]) {
        error = "level-one progression did not verify";
        return false;
    }
    return true;
}
bool ResetToLevelOne(void* game, UnitAny* player, std::string& error) {
    error.clear();
    if (!game || !Readable(game, 0xAC) || !Valid(player) ||
        player->dwType != UNIT_PLAYER || player->dwTxtFileNo > 6 ||
        !Valid(player->pStats) || !Valid(player->pInfo)) {
        error = "invalid death reset context";
        return false;
    }
    if (GetDeathMode(player) != DeathMode::Softcore) {
        error = "character is not confirmed Softcore";
        return false;
    }
    ProgressionResetContext context;
    if (!ResolveProgressionResetContext(player->dwTxtFileNo, context, error))
        return false;
    const ProgressionVitals previous{
        D2COMMON_GetUnitStat(player, STAT_LEVEL, 0),
        D2COMMON_GetUnitStat(player, STAT_VITALITY, 0),
        D2COMMON_GetUnitStat(player, STAT_ENERGY, 0),
        D2COMMON_GetUnitStat(player, STAT_MAXHP, 0),
        D2COMMON_GetUnitStat(player, STAT_MAXMANA, 0),
        D2COMMON_GetUnitStat(player, STAT_MAXSTAMINA, 0)};
    if (previous.level < 1 || previous.level > 99) {
        error = "invalid pre-death progression snapshot";
        return false;
    }
    if (!ResetProgressionState(game, player, context, previous, error))
        return false;
    error.clear();
    return true;
}
bool ValidLifeRestoreContext(void* game, UnitAny* player) {
    return game && Readable(game, 0xAC) && Valid(player) &&
        player->dwType == UNIT_PLAYER && player->dwTxtFileNo <= 6 &&
        Valid(player->pStats);
}
bool RestoreLevelOneLife(void* game, UnitAny* player, std::string& error) {
    error.clear();
    if (!ValidLifeRestoreContext(game, player)) {
        error = "invalid run-start life context";
        return false;
    }
    if (GetDeathMode(player) != DeathMode::Softcore ||
        D2COMMON_GetUnitStat(player, STAT_LEVEL, 0) != 1 ||
        D2COMMON_GetUnitStat(player, STAT_HP, 0) != 0) return true;
    const DWORD maximumLife = D2COMMON_GetUnitStat(player, STAT_MAXHP, 0);
    if (!maximumLife || maximumLife > INT32_MAX) {
        error = "level-one maximum life is invalid";
        return false;
    }
    HMODULE server = nullptr, common = nullptr;
    if (!ProgressionBindingsReady(server, common)) {
        error = "native life bindings unavailable";
        return false;
    }
    using Setter = void (__stdcall*)(UnitAny*, DWORD, int, WORD);
    auto setStat = reinterpret_cast<Setter>(
        GetProcAddress(common, MAKEINTRESOURCEA(10887)));
    if (!setStat) {
        error = "native life setter unavailable";
        return false;
    }
    setStat(player, STAT_HP, static_cast<int>(maximumLife), 0);
    NotifyStatUpdate(game, player, STAT_HP, server);
    if (D2COMMON_GetBaseStatSigned(player, STAT_HP, 0) != maximumLife ||
        D2COMMON_GetUnitStat(player, STAT_HP, 0) != maximumLife) {
        error = "level-one life restoration did not verify";
        return false;
    }
    return true;
}
bool AwardClearLevel(void* game, UnitAny* player) {
    if (!game || !Valid(player) || player->dwType != UNIT_PLAYER ||
        !Valid(player->pStats)) return false;
    const DWORD level = D2COMMON_GetUnitStat(player, STAT_LEVEL, 0);
    if (level == 99) return true;
    return level >= 1 && level < 99 &&
        ApplyProgression(game, player, ProgressionField::Level, level + 1, true);
}

json Catalog(const json& args) {
    unsigned id;
    if (!ClassId(args, id)) return Error("class_id must be an integer from 0 to 6");
    if (!BindingsReady()) return Error("S13 character bindings changed");
    sgptDataTable* tables = Tables();
    if (!tables || tables->dwCharsStatsRecs < 7 || tables->dwCharsStatsRecs > 64 ||
        !Readable(tables->pCharStatsTxt, sizeof(CharStatsTxt) * 7) ||
        !tables->pPlayerSkillCount || !Readable(tables->pPlayerSkillCount, 7 * sizeof(DWORD)) ||
        !tables->dwPlayerSkillCount || tables->dwPlayerSkillCount > 128 ||
        !tables->pPlayerSkillList || !Readable(tables->pPlayerSkillList,
            7 * tables->dwPlayerSkillCount * sizeof(WORD)) ||
        !tables->dwSkillsRecs || tables->dwSkillsRecs > 4096 ||
        !Readable(tables->pSkillsTxt, tables->dwSkillsRecs * sizeof(SkillsTxt)) ||
        !tables->dwSkillDescRecs || tables->dwSkillDescRecs > 4096 ||
        !Readable(tables->pSkillDescTxt, tables->dwSkillDescRecs * sizeof(SkillDescTxt)) ||
        !Readable(tables->pExperience, sizeof(ExperienceRow) * 101))
        return Error("native class, skill, or experience tables unavailable");
    const DWORD count = reinterpret_cast<const DWORD*>(tables->pPlayerSkillCount)[id];
    if (!count || count > 128 || count > tables->dwPlayerSkillCount)
        return Error("native class skill count is invalid");
    const CharStatsTxt& cls = tables->pCharStatsTxt[id];
    const ExperienceRow* exp = reinterpret_cast<const ExperienceRow*>(tables->pExperience);
    if (exp[0].classThreshold[id] < 99 || exp[0].classThreshold[id] > 127 ||
        exp[2].classThreshold[id] < exp[1].classThreshold[id])
        return Error("native experience table layout is invalid");
    json thresholds = json::array();
    for (unsigned level = 0; level <= 99; ++level) {
        if (level > 1 && exp[level].classThreshold[id] < exp[level - 1].classThreshold[id])
            return Error("native experience thresholds are not monotonic");
        thresholds.push_back(level ? exp[level].classThreshold[id] : 0);
    }
    json ids = json::array(), skills = json::array();
    const auto* slots = reinterpret_cast<const int16_t*>(tables->pPlayerSkillList);
    for (DWORD slot = 0; slot < count; ++slot) {
        const int skillId = slots[id * tables->dwPlayerSkillCount + slot];
        if (skillId < 0 || static_cast<DWORD>(skillId) >= tables->dwSkillsRecs ||
            tables->pSkillsTxt[skillId].wSkillId != static_cast<DWORD>(skillId))
            return Error("native class skill slot does not match the skill table");
        const SkillsTxt& skill = tables->pSkillsTxt[skillId];
        json prereqs = json::array();
        for (WORD prerequisite : {skill.wReqSkill1, skill.wReqSkill2, skill.wReqSkill3})
            if (prerequisite != 0xFFFF && prerequisite != 0) prereqs.push_back(prerequisite);
        std::string name = SkillName(tables, skill);
        if (name.empty()) return Error("native skill name is unavailable");
        ids.push_back(skillId);
        skills.push_back({{"id", skillId}, {"name", std::move(name)},
            {"required_level", skill.wReqLevel}, {"max_level", skill.wMaxLvl},
            {"prerequisites", std::move(prereqs)}, {"range", skill.bRange},
            {"usable_in_town", skill.dwFlags.bInTown != 0},
            {"passive", skill.dwFlags.bPassive != 0},
            {"mana", {{"base", static_cast<int16_t>(skill.wMana)},
                      {"per_level", static_cast<int16_t>(skill.wLevelMana)},
                      {"minimum", skill.wMinMana},
                      {"start", static_cast<int16_t>(skill.wStartMana)},
                      {"shift", skill.wManaShift & 0xFF}}},
            {"delay_formula", skill.dwDelay}, {"periodic_delay_formula", skill.dwPerDelay},
            {"calculations", {skill.dwCalc1, skill.dwCalc2, skill.dwCalc3, skill.dwCalc4}}});
    }
    return {{"ok", true}, {"class", {{"id", id}, {"name", ClassName(cls)},
        {"base_attributes", {{"strength", cls.bStr}, {"energy", cls.bEne},
                             {"dexterity", cls.bDex}, {"vitality", cls.bVita}}},
        {"base_life", cls.bHpAdd + cls.bVita}, {"base_mana", cls.bEne},
        {"base_stamina", cls.bStamina},
        {"life_per_level", cls.bLifePerLevel / 4.0},
        {"mana_per_level", cls.bManaPerLevel / 4.0},
        {"stamina_per_level", cls.bStaminaPerLevel / 4.0},
        {"life_per_vitality", cls.bLifePerVitality / 4.0},
        {"mana_per_energy", cls.bManaPerMagic / 4.0},
        {"stamina_per_vitality", cls.bStaminaPerVitality / 4.0},
        {"stat_points_per_level", cls.bStatPerLevel},
        {"skill_ids", std::move(ids)}, {"experience_thresholds", std::move(thresholds)},
        {"skills", std::move(skills)}}}};
}

json Inspect(const json& args) {
    if (!args.is_object() || !args.empty()) return Error("get_build_state takes no arguments");
    if (!BindingsReady()) return Error("S13 character bindings changed");
    if (!IsGameReady()) return Error("game is not ready");
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!Valid(player) || player->dwType != UNIT_PLAYER || player->dwTxtFileNo > 6 ||
        !Valid(player->pStats) || !Valid(player->pInfo) || !Valid(player->pPlayerData) ||
        !Valid(player->pPath) || !Valid(player->pPath->pRoom1) ||
        !Valid(player->pPath->pRoom1->pRoom2) || !Valid(player->pPath->pRoom1->pRoom2->pLevel))
        return Error("player stats, skills, or area unavailable");
    sgptDataTable* tables = Tables();
    if (!tables || !tables->dwSkillsRecs || tables->dwSkillsRecs > 4096 ||
        !Readable(tables->pSkillsTxt, tables->dwSkillsRecs * sizeof(SkillsTxt)))
        return Error("native skill table unavailable");
    const DeathMode mode = GetDeathMode(player);
    json hardcore = nullptr;
    if (mode != DeathMode::Unknown) hardcore = mode == DeathMode::Hardcore;
    json skills = json::array();
    Skill* current = player->pInfo->pFirstSkill;
    for (unsigned visited = 0; current; ++visited) {
        if (visited >= 4096 || !Valid(current)) return Error("player skill chain invalid or cyclic");
        SkillsTxt* info = current->pSkillInfo;
        if (!Valid(info) || info->wSkillId >= tables->dwSkillsRecs ||
            &tables->pSkillsTxt[info->wSkillId] != info)
            return Error("player skill record outside native table");
        // Item-granted skills carry an item GUID; they are not hard save points.
        skills.push_back({{"id", info->wSkillId},
                          {"hard_points", current->itemGUID == -1 ? current->skillLevel : 0},
                          {"effective_level", D2COMMON_GetSkillLevel(player, current, TRUE)},
                          {"use_state", D2COMMON_GetSkillUseState(player, current)},
                          {"animation_mode", D2COMMON_GetSkillMode(player, current)},
                          {"item_guid", current->itemGUID}});
        current = current->pNextSkill;
    }
    auto selected = [](Skill* value) -> json {
        return Valid(value) && Valid(value->pSkillInfo) ? json(value->pSkillInfo->wSkillId) : json(nullptr);
    };
    size_t nameLength = 0;
    while (nameLength < 16 && player->pPlayerData->szName[nameLength]) ++nameLength;
    return {{"ok", true}, {"class_id", player->dwTxtFileNo},
            {"name", std::string(player->pPlayerData->szName, nameLength)},
            {"unit_id", player->dwUnitId}, {"mode", player->dwMode},
            {"hardcore", hardcore},
            {"area", player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo},
            {"position", {{"x", player->pPath->xPos}, {"y", player->pPath->yPos}}},
            {"skill_delay_active", D2COMMON_GetUnitState(player, 121) != 0},
            {"selected_skills", {{"left_id", selected(player->pInfo->pLeftSkill)},
                                 {"right_id", selected(player->pInfo->pRightSkill)}}},
            {"stats", Stats(player)}, {"skills", std::move(skills)}};
}
} // namespace CharacterState
