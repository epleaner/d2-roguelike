#define NOMINMAX
#include "ServerMonsters.h"
#include "Constants.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "LabProfile.h"
#include "CharacterState.h"
#include "AreaPopulation.h"
#include "ServerSpawns.h"
#include "RoguelikeMonsterScaling.h"
#include "RoguelikeResistance.h"
#include <wincrypt.h>
#include <detours.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <intrin.h>
#include <mutex>

namespace {
using json = nlohmann::json;
constexpr DWORD kConstructorRva = 0xEEF10, kLookupRva = 0xE03A0;
constexpr DWORD kSetterRva = 0x3A740, kGetterRva = 0x38B70, kBaseGetterRva = 0x38C20;
constexpr const char* kNames[] = {"level", "hp", "attack_rating", "defense", "min_damage", "max_damage",
    "hp_regen", "physical_resistance", "magic_resistance", "fire_resistance", "lightning_resistance",
    "cold_resistance", "poison_resistance"};
constexpr int kStats[] = {12, 6, 19, 31, 21, 22, 74, 36, 37, 39, 41, 43, 45};
constexpr int kMinimum[] = {1, 1, 0, 0, 0, 0, 0, -100, -100, -100, -100, -100, -100};
constexpr int kMaximum[] = {99, 1000000, 1000000, 1000000, 1000000, 1000000, 1000000,
    200, 200, 200, 200, 200, 200};
static_assert(sizeof(kStats) / sizeof(kStats[0]) == ServerMonsters::kAttributeCount, "stat mapping");
static_assert(sizeof(MonStatsTxt) == 0x1A8, "pinned S13 monster table layout");
#pragma pack(push, 1)
struct ConstructorRecord {
    void* game;
    Room1* room;
    void* roomCoord;
    int classId;
    int mode;
    int guid;
    int x;
    int y;
    int extra;
    int16_t flags;
};
#pragma pack(pop)
static_assert(sizeof(ConstructorRecord) == 0x26, "pinned S13 constructor ABI");
// S13 D2Game 0x251F0 reads the 0x34-byte SuperUniques row, calls the
// native spawn at 0x24950, installs the boss index and rolls table modifiers.
#pragma pack(push, 1)
struct SuperUniqueRow {
    WORD id, name;
    int classId, hcIndex, mods[3], sound, minGroup, maxGroup;
    BYTE autoPosition, eclass, stacks, replaceable, transforms[3], padding;
    WORD treasureClasses[3], tail;
};
struct UniqueModifierRow {
    DWORD id;
    WORD version;
    BYTE enabled, transfer, champion, fPick;
    WORD excludes[2], championPick[3], uniquePick[3], padding;
    DWORD constants;
};
#pragma pack(pop)
static_assert(sizeof(SuperUniqueRow) == 0x34, "pinned S13 superunique row");
static_assert(sizeof(UniqueModifierRow) == 0x20, "pinned S13 modifier row");
using SuperSpawn = UnitAny* (__stdcall*)(void*, Room1*, int, int, int);
SuperSpawn gSuperSpawn = nullptr;
void* gRankUnique = nullptr;
void* gRollUnique = nullptr;
void* gRollChampion = nullptr;
void* gApplyModifiers = nullptr;
void* gChampionInit = nullptr;
// S13 0x23260 takes ESI=game, EAX=monster, updates the native region
// unique count and the monster's native unique bit; it is not a flag shortcut.
__declspec(naked) void __stdcall MarkUnique(void*, UnitAny*) {
    __asm {
        push esi
        mov esi, [esp + 8]
        mov eax, [esp + 12]
        call dword ptr [gRankUnique]
        pop esi
        ret 8
    }
}
__declspec(naked) void __stdcall RollUnique(void*, UnitAny*) {
    __asm {
        push 0
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        call dword ptr [gRollUnique]
        ret 8
    }
}
__declspec(naked) int __stdcall RollChampion(void*, UnitAny*) {
    __asm {
        mov eax, [esp + 4]
        push dword ptr [esp + 8]
        call dword ptr [gRollChampion]
        ret 8
    }
}
__declspec(naked) void __stdcall InitializeChampion(void*, UnitAny*, int) {
    __asm {
        mov ecx, [esp + 4]
        mov eax, [esp + 8]
        push dword ptr [esp + 12]
        call dword ptr [gChampionInit]
        ret 12
    }
}
// Native 0x248A0 is the common monster modifier application path; EAX=0
// skips its unrelated forced baseline, while its native four baseline
// effects and all selected unique mods still run on this very unit.
__declspec(naked) void __stdcall ApplyUnique(void*, UnitAny*) {
    __asm {
        push edi
        mov edx, [esp + 8]
        mov edi, [esp + 12]
        xor eax, eax
        push eax
        push eax
        lea eax, [edi + 0x20]
        push eax
        xor eax, eax
        xor ecx, ecx
        call dword ptr [gApplyModifiers]
        pop edi
        ret 8
    }
}
using Constructor = UnitAny* (__fastcall*)(ConstructorRecord*);
using Setter = void (__stdcall*)(UnitAny*, DWORD, int, WORD);
using Getter = DWORD (__stdcall*)(UnitAny*, DWORD, DWORD);
Constructor gConstructor = nullptr;
Constructor gNaturalConstructor = nullptr;
// These pinned S13 room and whole-world initialization paths enclose natural
// population; the constructor hook identifies which area actually populated.
using RoomInitializer = void (__cdecl*)();
RoomInitializer gRoomInitialize = nullptr, gLevelInitialize = nullptr;
thread_local unsigned gPopulationDepth = 0;
thread_local int gPopulationArea = 0;
std::atomic<uint32_t> gPopulationCalls{0};
void __cdecl EnterPopulation() { ++gPopulationDepth; }
void __cdecl ExitPopulation(bool world) {
    const unsigned depth = --gPopulationDepth;
    if (!depth && gPopulationArea) ServerSpawns::OnPopulationCompleted(gPopulationArea);
    if (!depth && world) ServerSpawns::OnWorldPopulationCompleted();
    if (!depth) gPopulationArea = 0;
}
__declspec(naked) void RoomPopulationHook() {
    __asm {
        pushfd
        pushad
        call EnterPopulation
        popad
        popfd
        call dword ptr [gRoomInitialize]
        pushfd
        pushad
        push 0
        call ExitPopulation
        add esp, 4
        popad
        popfd
        ret
    }
}
__declspec(naked) void LevelPopulationHook() {
    __asm {
        pushfd
        pushad
        call EnterPopulation
        popad
        popfd
        call dword ptr [gLevelInitialize]
        pushfd
        pushad
        push 1
        call ExitPopulation
        add esp, 4
        popad
        popfd
        ret
    }
}
std::atomic<void*> gNaturalGame{nullptr};
std::string gNaturalEpoch;
std::array<std::array<std::atomic<uint32_t>, 4>, 255> gNaturalTargets{};
std::array<std::atomic<uint32_t>, 5> gNaturalCounters{};
std::atomic<uint32_t> gRemappedClasses{0};
struct NaturalSample {
    int classId;
    int producedClass;
    DWORD cave;
    DWORD ownerType;
    DWORD ownerId;
    int16_t spawnFlags;
    DWORD callerRva;
    bool gameCaller;
    bool population;
};
std::array<NaturalSample, 64> gNaturalSamples{};
std::mutex gNaturalSamplesMutex;
std::atomic<size_t> gNaturalSampleCount{0};
uintptr_t gNaturalImageBase = 0;
DWORD gNaturalImageSize = 0;
bool gNaturalHookInstalled = false;
UnitAny* __fastcall NaturalHook(ConstructorRecord* record);
Setter gSetter = nullptr;
Getter gGetter = nullptr;
Getter gBaseGetter = nullptr;
void* gLookup = nullptr;
using ListSetter = int (__stdcall*)(StatList*, DWORD, int, WORD);
ListSetter gListSetter = nullptr;
void* gModeStatReturn = nullptr;
std::mutex gManagedMutex;
struct Managed {
    UnitAny* unit = nullptr;
    StatList* stats = nullptr;
    DWORD id = 0;
    std::array<int, 3> targets{};
    unsigned present = 0;
};
std::array<Managed, 1024> gManaged{};
void* gManagedGame = nullptr;
size_t gManagedCount = 0;
int __stdcall ModeListSetter(StatList* list, DWORD stat, int value, WORD layer);

// E03A0 uses ECX=game, EDX=GUID and EAX=unit type. Its body returns in EAX.
__declspec(naked) UnitAny* __stdcall LookupMonster(void*, DWORD) {
    __asm {
        mov ecx, [esp + 4]
        mov edx, [esp + 8]
        mov eax, 1
        call dword ptr [gLookup]
        ret 8
    }
}

bool Readable(const void* pointer, size_t bytes) {
    if (!pointer || !bytes) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (bytes > std::numeric_limits<uintptr_t>::max() - start) return false;
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(pointer, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    return end >= start && bytes <= end - start;
}
template <typename T> bool Valid(const T* value) { return Readable(value, sizeof(T)); }
bool Bound(const BYTE* address, const BYTE* expected, size_t size, HMODULE module) {
    MEMORY_BASIC_INFORMATION region = {};
    if (!Readable(address, size) || !VirtualQuery(address, &region, sizeof(region)) ||
        region.AllocationBase != module ||
        !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    return !memcmp(address, expected, size);
}
bool Bind(std::string& error) {
    if (!LabProfile::VerifyServer(error)) return false;
    HMODULE game = GetModuleHandleA("D2GAME.dll");
    HMODULE common = GetModuleHandleA("D2Common.dll");
    if (!game || !common) { error = "pinned server or common image unavailable"; return false; }
    static const BYTE constructor[] = {0x81,0xEC,0x94,0,0,0,0x53,0x56,0x8B,0xF1,0x8B,0x46,0x0C,0x33,0xDB,0x3B,0xC3,0x57,0x89,0x74,0x24,0x2C,0x7C,0x37};
    static const BYTE lookup[] = {0x85,0xC9,0x75,0x1F,0x68,0xD3,0x02,0,0,0xE8,0x2C,0x9F,0xF2,0xFF};
    static const BYTE setter[] = {0x8B,0x44,0x24,0x0C,0x53,0x8B,0x5C,0x24,0x0C,0x56,0x8B,0x74,0x24,0x0C,0x8B,0xCE,0x90,0x57,0x8B,0x7C,0x24,0x1C,0x57,0x50};
    static const BYTE getter[] = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x75,0x03,0xC2,0x0C,0,0x53,0x8B,0x58,0x5C,0x85,0xDB,0x75,0x06,0x33,0xC0,0x5B,0xC2,0x0C};
    static const BYTE baseGetter[] = {0x8B,0x54,0x24,0x04,0x85,0xD2,0x75,0x05,0x33,0xC0,0xC2,0x0C,0,0x53,0x57,0x8B,0x7C,0x24,0x10,0x85,0xFF,0x7C,0x27,0x8B};
    BYTE* const gameBase = reinterpret_cast<BYTE*>(game);
    BYTE* const commonBase = reinterpret_cast<BYTE*>(common);
    BYTE* const createSite = gameBase + kConstructorRva;
    BYTE* const lookupSite = gameBase + kLookupRva;
    BYTE* const setSite = commonBase + kSetterRva;
    BYTE* const getSite = commonBase + kGetterRva;
    BYTE* const baseSite = commonBase + kBaseGetterRva;
    if (!(Bound(createSite, constructor, sizeof(constructor), game) ||
          (gNaturalHookInstalled && Readable(createSite, 5) && createSite[0] == 0xE9 &&
           createSite + 5 + *reinterpret_cast<const int32_t*>(createSite + 1) ==
               reinterpret_cast<const BYTE*>(&NaturalHook))) ||
        !Bound(lookupSite, lookup, sizeof(lookup), game) ||
        !Bound(setSite, setter, sizeof(setter), common) ||
        !Bound(getSite, getter, sizeof(getter), common) ||
        !Bound(baseSite, baseGetter, sizeof(baseGetter), common) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10887)) != reinterpret_cast<FARPROC>(setSite) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10973)) != reinterpret_cast<FARPROC>(getSite) ||
        GetProcAddress(common, MAKEINTRESOURCEA(10587)) != reinterpret_cast<FARPROC>(baseSite)) {
        error = "unexpected active monster constructor, lookup or unit stat binding";
        return false;
    }
    if (!gSetter) {
        gConstructor = reinterpret_cast<Constructor>(createSite);
        gLookup = lookupSite;
        gSetter = reinterpret_cast<Setter>(setSite);
        gGetter = reinterpret_cast<Getter>(getSite);
        gBaseGetter = reinterpret_cast<Getter>(baseSite);
    }
    return true;
}
bool BindRank(std::string& error) {
    HMODULE module = GetModuleHandleA("D2GAME.dll");
    if (!module) { error = "pinned server image unavailable"; return false; }
    BYTE* base = reinterpret_cast<BYTE*>(module);
    static const BYTE super[] = {0x81,0xEC,0x08,0x01,0,0,0x56,0x8B,0xB4,0x24,0x10,0x01,0,0,0x80,0x7E};
    static const BYTE mark[] = {0x85,0xC0,0x74,0x44,0x8B,0x10,0x83,0xFA,0x01,0x75,0x3D,0x8B,0x48,0x14,0x85,0xC9};
    static const BYTE roll[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF8,0x81,0xEC,0x0C,0x01,0,0,0x53,0x56,0x57,0x8B};
    static const BYTE championRoll[] = {0x81,0xEC,0x0C,0x08,0,0,0x53,0x33,0xDB,0x3B,0xC3,0x0F,0x84,0xEC,0,0};
    static const BYTE apply[] = {0x85,0xC0,0x53,0x55,0x56,0x74,0x12,0x8B,0x44,0x24,0x18,0x50,0x8B,0x44,0x24,0x18};
    static const BYTE championInit[] = {0x56,0x57,0x8B,0xF8,0x85,0xFF,0x8B,0xF1,0x74,0x68,0x8B,0x0F,0x83,0xF9,0x01,0x75};
    if (!Bound(base + 0x251F0, super, sizeof(super), module) ||
        !Bound(base + 0x23260, mark, sizeof(mark), module) ||
        !Bound(base + 0x23CA0, roll, sizeof(roll), module) ||
        !Bound(base + 0x23AE0, championRoll, sizeof(championRoll), module) ||
        !Bound(base + 0x248A0, apply, sizeof(apply), module) ||
        !Bound(base + 0x24BF0, championInit, sizeof(championInit), module)) {
        error = "unexpected S13 native superunique or modifier initialization"; return false;
    }
    gSuperSpawn = reinterpret_cast<SuperSpawn>(base + 0x251F0);
    gRankUnique = base + 0x23260;
    gRollUnique = base + 0x23CA0;
    gRollChampion = base + 0x23AE0;
    gApplyModifiers = base + 0x248A0;
    gChampionInit = base + 0x24BF0;
    return true;
}
const SuperUniqueRow* SuperRows(const sgptDataTable* table) {
    if (!table || !table->dwSuperUniquesRecs || table->dwSuperUniquesRecs > 1024 ||
        !Readable(table->pSuperUniquesTxt, size_t(table->dwSuperUniquesRecs) * sizeof(SuperUniqueRow)))
        return nullptr;
    return reinterpret_cast<const SuperUniqueRow*>(table->pSuperUniquesTxt);
}
const UniqueModifierRow* ModifierRows(const sgptDataTable* table) {
    if (!table || !table->dwMonUModRecs || table->dwMonUModRecs > 255 ||
        !Readable(table->pMonUModTxt, size_t(table->dwMonUModRecs) * sizeof(UniqueModifierRow)))
        return nullptr;
    return reinterpret_cast<const UniqueModifierRow*>(table->pMonUModTxt);
}
bool Integer(const json& value, int64_t minimum, int64_t maximum, int64_t& out) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) {
        uint64_t n = value.get<uint64_t>();
        if (n > static_cast<uint64_t>(maximum)) return false;
        out = static_cast<int64_t>(n);
    } else out = value.get<int64_t>();
    return out >= minimum && out <= maximum;
}
sgptDataTable* Tables() {
    if (!Valid(p_D2COMMON_sgptDataTable)) return nullptr;
    sgptDataTable* table = *p_D2COMMON_sgptDataTable;
    if (!Valid(table) || !table->dwMonStatsRecs || table->dwMonStatsRecs > 32768 ||
        !Readable(table->pMonStatsTxt, size_t(table->dwMonStatsRecs) * sizeof(MonStatsTxt))) return nullptr;
    return table;
}
bool Spawnable(const MonStatsTxt& row) {
    return row.dwFlags.bEnabled && row.dwFlags.bKillable && !row.dwFlags.bNPC && !row.dwFlags.binTown;
}
json LiveTableDigest(const void* rows, DWORD count, size_t rowSize) {
    json identity = {{"sha256", nullptr}, {"rows", nullptr}, {"covered", false}};
    if (!count || count > 32768 || rowSize > 65536 ||
        !Readable(rows, size_t(count) * rowSize)) return identity;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[32] = {};
    DWORD length = sizeof(digest);
    const bool ok = CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(rows),
            static_cast<DWORD>(size_t(count) * rowSize), 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    if (!ok) return identity;
    constexpr char hex[] = "0123456789abcdef";
    std::string text(64, '0');
    for (size_t i = 0; i < sizeof(digest); ++i) {
        text[2 * i] = hex[digest[i] >> 4];
        text[2 * i + 1] = hex[digest[i] & 15];
    }
    identity["sha256"] = std::move(text);
    identity["rows"] = count;
    identity["covered"] = true;
    return identity;
}
json Failure(const std::string& reason) { return {{"ok", false}, {"error", reason}}; }
std::string WideUtf8(const wchar_t* text) {
    if (!text) return {};
    size_t count = 0;
    for (; count < 255; ++count) {
        if (!Readable(text + count, sizeof(wchar_t))) return {};
        if (!text[count]) break;
    }
    if (count == 255) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes > 1024) return {};
    std::string result(bytes, '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), &result[0], bytes, nullptr, nullptr) != bytes) return {};
    return result;
}
bool Context(UnitAny* player, Room1*& room, DWORD& level) {
    if (!Valid(player) || player->dwType != UNIT_PLAYER || !Valid(player->pPath) ||
        !Valid(player->pPath->pRoom1)) return false;
    room = player->pPath->pRoom1;
    if (!Valid(room->pRoom2) || !Valid(room->pRoom2->pLevel)) return false;
    level = room->pRoom2->pLevel->dwLevelNo;
    return level != 0;
}
bool ObservedRoom(UnitAny* player, Room1* room, DWORD level) {
    if (!Valid(player->pAct) || !Valid(room) || !Valid(room->pRoom2) ||
        !Valid(room->pRoom2->pLevel) || room->pRoom2->pLevel->dwLevelNo != level) return false;
    Room1* cursor = player->pAct->pRoom1;
    for (unsigned i = 0; i < 1024 && cursor; ++i) {
        if (!Valid(cursor)) return false;
        if (cursor == room) return true;
        cursor = cursor->pRoomNext;
    }
    return false;
}
json Identity(UnitAny* unit) {
    sgptDataTable* table = Tables();
    const MonsterData* data = unit->pMonsterData;
    if (!table || unit->dwTxtFileNo >= table->dwMonStatsRecs || !Valid(data))
        return json::object();
    const BYTE flags = *(reinterpret_cast<const BYTE*>(data) + 0x16);
    int superId = -1;
    const SuperUniqueRow* supers = SuperRows(table);
    if ((flags & 2) && supers && data->wUniqueNo < table->dwSuperUniquesRecs &&
        supers[data->wUniqueNo].classId == static_cast<int>(unit->dwTxtFileNo))
        superId = data->wUniqueNo;
    const char* rank = (flags & 2) ? "superunique" :
        (flags & 4) ? "champion" : (flags & 8) ? "unique" : "normal";
    const MonStatsTxt& monster = table->pMonStatsTxt[unit->dwTxtFileNo];
    std::string className = WideUtf8(D2LANG_GetLocaleText(monster.wNameStr));
    std::string name = className;
    if (superId >= 0)
        name = WideUtf8(D2LANG_GetLocaleText(supers[superId].name));
    json modifiers = json::array();
    const UniqueModifierRow* rows = ModifierRows(table);
    for (BYTE mod : data->anEnchants) {
        if (!mod) break;
        modifiers.push_back({{"id", mod},
            {"catalog_id", rows && mod < table->dwMonUModRecs ? rows[mod].id : 0}});
    }
    return {{"name", name}, {"class_name", className}, {"rank", rank},
        {"superunique_id", superId >= 0 ? json(superId) : json(nullptr)},
        {"name_seed", data->NameSeed}, {"modifiers", std::move(modifiers)},
        {"minion", bool(flags & 0x10)}, {"set_boss", bool(monster.dwFlags.bSetBoss)},
        {"prime_evil", bool(monster.dwFlags.bPrimeEvil)}};
}

bool CollectAreaMonsters(Level* area, std::vector<UnitAny*>& units, std::string& error) {
    if (!Valid(area)) { error = "spawn census level unavailable"; return false; }
    unsigned rooms = 0, visited = 0;
    for (Room2* metadata = area->pRoom2First; metadata; metadata = metadata->pRoom2Next) {
        if (++rooms > 256 || !Valid(metadata) || metadata->pLevel != area) {
            error = "spawn census room chain invalid or exceeds bound"; return false;
        }
        Room1* active = metadata->pRoom1;
        if (!active) continue;
        if (!Valid(active) || active->pRoom2 != metadata) {
            error = "spawn census active room identity invalid"; return false;
        }
        unsigned chain = 0;
        for (UnitAny* unit = active->pUnitFirst; unit; unit = unit->pRoomNext) {
            if (++chain > 4096 || ++visited > 8192 || !Valid(unit)) {
                error = "spawn census unit chain invalid or exceeds bound"; return false;
            }
            if (unit->dwType != UNIT_MONSTER) continue;
            if (!Valid(unit->pPath) || unit->pPath->pRoom1 != active ||
                !Valid(unit->pStats) || !Valid(unit->pMonsterData) ||
                !Valid(unit->pMonsterData->pMonStatsTxt) || units.size() >= 1024) {
                error = "spawn census monster record invalid or exceeds bound"; return false;
            }
            units.push_back(unit);
        }
    }
    std::sort(units.begin(), units.end(),
        [](UnitAny* left, UnitAny* right) { return left->dwUnitId < right->dwUnitId; });
    if (std::adjacent_find(units.begin(), units.end(),
            [](UnitAny* left, UnitAny* right) { return left->dwUnitId == right->dwUnitId; }) != units.end()) {
        error = "spawn census duplicate monster identity"; return false;
    }
    return true;
}
json State(UnitAny* unit) {
    const int hp = static_cast<int>(gGetter(unit, 6, 0));
    const int maxHp = static_cast<int>(gGetter(unit, 7, 0));
    json attributes = json::object();
    for (size_t i = 0; i < ServerMonsters::kAttributeCount; ++i) {
        if (i == 1) attributes[kNames[i]] = double(hp) / 256.0;
        else attributes[kNames[i]] = static_cast<int>(gGetter(unit, kStats[i], 0));
    }
    attributes["max_hp"] = double(maxHp) / 256.0;
    json result = {{"id", unit->dwUnitId}, {"type", UNIT_MONSTER}, {"class_id", unit->dwTxtFileNo},
        {"present", true}, {"mode", unit->dwMode}, {"x", unit->pPath->xPos}, {"y", unit->pPath->yPos},
        {"dead", unit->dwMode == NPC_MODE_DEATH || unit->dwMode == NPC_MODE_DEAD || hp <= 0},
        {"hp_raw", hp}, {"max_hp_raw", maxHp}, {"attributes", std::move(attributes)}};
    result.update(Identity(unit));
    return result;
}
bool SetEffective(UnitAny* unit, int stat, int target) {
    // The unit setter changes the base list; native equipment/modifier lists
    // remain attached. Compensate their existing contribution, not the target.
    const int64_t base = static_cast<int>(gBaseGetter(unit, stat, 0));
    const int64_t total = static_cast<int>(gGetter(unit, stat, 0));
    const int64_t replacement = base + int64_t(target) - total;
    if (replacement < INT_MIN || replacement > INT_MAX) return false;
    gSetter(unit, stat, static_cast<int>(replacement), 0);
    return true;
}
bool ScaleMonsterLevel(UnitAny* unit, int level) {
    const int nativeLevel = static_cast<int>(gGetter(unit, 12, 0));
    const int nativeMax = static_cast<int>(gGetter(unit, 7, 0));
    const int nativeHp = static_cast<int>(gGetter(unit, 6, 0));
    RoguelikeMonsterScaling::HitPoints scaled{};
    if (!RoguelikeMonsterScaling::ScaleHitPoints(
            nativeLevel, level, nativeHp, nativeMax, scaled)) return false;
    if (!SetEffective(unit, 12, level) ||
        !SetEffective(unit, 7, scaled.maximumRaw) ||
        !SetEffective(unit, 6, scaled.currentRaw)) return false;
    return static_cast<int>(gGetter(unit, 12, 0)) == level &&
        static_cast<int>(gGetter(unit, 7, 0)) == scaled.maximumRaw &&
        static_cast<int>(gGetter(unit, 6, 0)) == scaled.currentRaw;
}
constexpr size_t kFirstResistance = 7;
constexpr size_t kResistanceCount = 6;
bool ResistanceRow(UnitAny* unit, int depth, const MonStatsTxt*& row) {
    if (depth < 1 || depth > 99 || !unit || !Valid(unit) ||
        unit->dwType != UNIT_MONSTER || !Valid(unit->pStats) ||
        !Valid(unit->pMonsterData) ||
        !Valid(unit->pMonsterData->pMonStatsTxt)) return false;
    row = unit->pMonsterData->pMonStatsTxt;
    return row->dwFlags.bKillable && !row->dwFlags.bNPC &&
        !row->dwFlags.binTown && gGetter(unit, STAT_ALIGNMENT, 0) == 0;
}
bool BuildResistanceTargets(UnitAny* unit, const MonStatsTxt& row,
    int depth, int* targets) {
    const WORD normalValues[] = {row.wResDmg, row.wResMagic, row.wResFire,
        row.wResLight, row.wResCold, row.wResPoison};
    const WORD nightmareValues[] = {row.wResDmg_N, row.wResMagic_N,
        row.wResFire_N, row.wResLight_N, row.wResCold_N, row.wResPoison_N};
    const WORD hellValues[] = {row.wResDmg_H, row.wResMagic_H, row.wResFire_H,
        row.wResLight_H, row.wResCold_H, row.wResPoiosn_H};
    for (size_t i = 0; i < kResistanceCount; ++i) {
        const int normal = RoguelikeResistance::DecodeMonsterResistanceWord(normalValues[i]);
        const int nightmare = RoguelikeResistance::DecodeMonsterResistanceWord(nightmareValues[i]);
        const int hell = RoguelikeResistance::DecodeMonsterResistanceWord(hellValues[i]);
        int interpolated = 0;
        if (!RoguelikeResistance::InterpolateMonsterResistance(
                normal, nightmare, hell, depth, interpolated)) return false;
        const int stat = kStats[kFirstResistance + i];
        const int64_t delta = int64_t(interpolated) - normal;
        const int64_t target = int64_t(gGetter(unit, stat, 0)) + delta;
        const int64_t replacement = int64_t(gBaseGetter(unit, stat, 0)) + delta;
        if (target < INT_MIN || target > INT_MAX ||
            replacement < INT_MIN || replacement > INT_MAX) return false;
        targets[i] = static_cast<int>(target);
    }
    return true;
}
bool ApplyResistanceTargets(UnitAny* unit, const int* targets) {
    for (size_t i = 0; i < kResistanceCount; ++i) {
        const int stat = kStats[kFirstResistance + i];
        if (static_cast<int>(gGetter(unit, stat, 0)) != targets[i] &&
            !SetEffective(unit, stat, targets[i])) return false;
    }
    for (size_t i = 0; i < kResistanceCount; ++i)
        if (static_cast<int>(gGetter(unit, kStats[kFirstResistance + i], 0)) !=
            targets[i]) return false;
    return true;
}
bool ScaleMonsterResistances(UnitAny* unit, int depth) {
    if (depth == 0) return true;
    const MonStatsTxt* row = nullptr;
    int targets[kResistanceCount]{};
    return ResistanceRow(unit, depth, row) &&
        BuildResistanceTargets(unit, *row, depth, targets) &&
        ApplyResistanceTargets(unit, targets);
}
bool ScaleMonsterForZone(UnitAny* unit, int level, int runDepth) {
    return ScaleMonsterLevel(unit, level) &&
        ScaleMonsterResistances(unit, runDepth);
}
bool ScaleEncounterStat(UnitAny* unit, int stat, int level, int nativeLevel) {
    const int original = static_cast<int>(gGetter(unit, stat, 0));
    if (original < 0) return false;
    const int64_t scaled = (int64_t(original) * level + nativeLevel / 2) /
        nativeLevel;
    return scaled <= INT_MAX &&
        SetEffective(unit, stat, static_cast<int>(scaled)) &&
        static_cast<int>(gGetter(unit, stat, 0)) == scaled;
}
bool ScaleEncounterMonster(UnitAny* unit, int level, int runDepth,
    int& nativeLevel) {
    if (!unit || !Valid(unit) || !Valid(unit->pStats)) return false;
    nativeLevel = static_cast<int>(gGetter(unit, 12, 0));
    if (nativeLevel < 1 || nativeLevel > 65535 ||
        !ScaleMonsterForZone(unit, level, runDepth))
        return false;
    return ScaleEncounterStat(unit, STAT_ATTACKRATING, level, nativeLevel) &&
        ScaleEncounterStat(unit, STAT_DEFENSE, level, nativeLevel);
}
// EEF10's E7B0 stat initialization completed when the trampoline returns.
UnitAny* __fastcall NaturalHook(ConstructorRecord* record) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    // The input row is read only after checking its complete ABI-sized extent.
    const bool readableRecord = Valid(record);
    const int originalClass = readableRecord ? record->classId : -1;
    // Constructor input is stack-owned by its caller. Only inside a verified
    // room population frame and our configured cave may we replace its class;
    // restore it immediately so upstream population logic sees its own input.
    std::array<uint32_t, 4> configuredRoster{};
    int configuredArea = 0;
    if (gPopulationDepth && readableRecord && originalClass >= 0 &&
        record->game == gNaturalGame.load(std::memory_order_acquire) &&
        Valid(record->room) && Valid(record->room->pRoom2) &&
        Valid(record->room->pRoom2->pLevel)) {
        const DWORD area = record->room->pRoom2->pLevel->dwLevelNo;
        if ((area >= 13 && area <= 16) || (area >= 138 && area <= 254)) {
            configuredArea = area;
            for (unsigned index = 0; index < configuredRoster.size(); ++index)
                configuredRoster[index] =
                    gNaturalTargets[area][index].load(std::memory_order_acquire);
        }
    }
    const uint32_t configured = configuredRoster[0];
    uint32_t selected = configured;
    bool selectedClassMatched = false;
    for (const uint32_t target : configuredRoster) {
        if (!target) break;
        if (originalClass == int(target >> 16)) {
            selected = target;
            selectedClassMatched = true;
            break;
        }
    }
    const bool nativeExtra = configuredArea &&
        ServerSpawns::PreserveNativeMonsterClass(record->game, configuredArea, originalClass);
    const bool remap = configured && !nativeExtra && !selectedClassMatched;
    if (remap) record->classId = int(configured >> 16);
    UnitAny* unit = gNaturalConstructor(record);
    if (remap) {
        record->classId = originalClass;
        gRemappedClasses.fetch_add(1, std::memory_order_relaxed);
    }
    gNaturalCounters[0].fetch_add(1, std::memory_order_relaxed);
    if (!unit || !readableRecord ||
        record->game != gNaturalGame.load(std::memory_order_acquire) ||
        !Valid(unit) || unit->dwType != UNIT_MONSTER || !Valid(record->room) ||
        !Valid(record->room->pRoom2) || !Valid(record->room->pRoom2->pLevel) ||
        !Valid(unit->pStats)) return unit;
    gNaturalCounters[1].fetch_add(1, std::memory_order_relaxed);
    const DWORD cave = record->room->pRoom2->pLevel->dwLevelNo;
    if (!((cave >= 13 && cave <= 16) || (cave >= 138 && cave <= 254))) return unit;
    if (gPopulationDepth && configured && !gPopulationArea) gPopulationArea = cave;
    gNaturalCounters[2].fetch_add(1, std::memory_order_relaxed);
    if (originalClass >= 0 && originalClass <= 32767 &&
        gNaturalSampleCount.load(std::memory_order_relaxed) < gNaturalSamples.size()) {
        std::lock_guard<std::mutex> lock(gNaturalSamplesMutex);
        const size_t count = gNaturalSampleCount.load(std::memory_order_relaxed);
        if (count < gNaturalSamples.size() &&
            record->game == gNaturalGame.load(std::memory_order_acquire)) {
            const uintptr_t offset = caller - gNaturalImageBase;
            const bool gameCaller = offset < gNaturalImageSize;
            gNaturalSamples[count] = {originalClass, int(unit->dwTxtFileNo), cave,
                unit->dwOwnerType, unit->dwOwnerId, record->flags,
                gameCaller ? static_cast<DWORD>(offset) : 0, gameCaller,
                gPopulationDepth != 0};
            gNaturalSampleCount.store(count + 1, std::memory_order_release);
        }
    }
    if (!gPopulationDepth) return unit;
    gPopulationCalls.fetch_add(1, std::memory_order_relaxed);
    if (!selected || (unit->dwTxtFileNo != (selected >> 16) && !nativeExtra))
        return unit;
    gNaturalCounters[3].fetch_add(1, std::memory_order_relaxed);
    const int level = selected & 0xffff;
    if (!ScaleMonsterForZone(unit, level,
            ServerSpawns::CombatRunDepth(record->game, cave))) return unit;
    ServerSpawns::ApplyStockMapMonster(record->game, unit, cave);
    gNaturalCounters[4].fetch_add(1, std::memory_order_relaxed);
    return unit;
}
bool InstallNaturalHook(std::string& error) {
    if (gNaturalHookInstalled) return true;
    if (!Bind(error)) return false; // exact entry and stat ordinals
    const auto* image = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2GAME.dll"));
    if (!Readable(image, sizeof(IMAGE_DOS_HEADER))) {
        error = "natural constructor image header unavailable"; return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(*dos) ||
        dos->e_lfanew > 4096 ||
        !Readable(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) {
        error = "natural constructor image header invalid"; return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.SizeOfImage <= kConstructorRva) {
        error = "natural constructor image dimensions invalid"; return false;
    }
    gNaturalImageBase = reinterpret_cast<uintptr_t>(image);
    static const BYTE roomEntry[] = {0x56, 0x8B, 0xF0, 0x57, 0xE8, 0xC7, 0xAD, 0x01, 0x00};
    static const BYTE levelEntry[] = {0x83, 0xEC, 0x08, 0x57, 0x8B, 0xF8, 0x85, 0xFF, 0x75, 0x1F};
    const auto* roomSite = image + 0x2CB80;
    const auto* levelSite = image + 0x2CBE0;
    if (!Readable(roomSite, sizeof(roomEntry)) ||
        !Readable(levelSite, sizeof(levelEntry)) ||
        memcmp(roomSite, roomEntry, sizeof(roomEntry)) ||
        memcmp(levelSite, levelEntry, sizeof(levelEntry))) {
        error = "unexpected pinned S13 room population entry"; return false;
    }
    gRoomInitialize = reinterpret_cast<RoomInitializer>(const_cast<BYTE*>(roomSite));
    gLevelInitialize = reinterpret_cast<RoomInitializer>(const_cast<BYTE*>(levelSite));
    gNaturalImageSize = nt->OptionalHeader.SizeOfImage;
    gNaturalConstructor = gConstructor;
    if (DetourTransactionBegin() != NO_ERROR) {
        error = "cannot begin natural constructor detour"; return false;
    }
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        DetourTransactionAbort(); error = "cannot update server thread for constructor detour"; return false;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        DetourTransactionAbort(); error = "cannot enumerate threads for constructor detour"; return false;
    }
    std::array<HANDLE, 1024> threads{};
    size_t used = 0;
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    bool ok = Thread32First(snapshot, &entry) != 0;
    const DWORD process = GetCurrentProcessId(), current = GetCurrentThreadId();
    do {
        if (!ok) break;
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == current) continue;
        if (used == threads.size()) { ok = false; break; }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
            THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) { ok = false; break; }
        if (DetourUpdateThread(thread) != NO_ERROR) { CloseHandle(thread); ok = false; break; }
        threads[used++] = thread;
    } while (Thread32Next(snapshot, &entry));
    const DWORD endError = GetLastError();
    CloseHandle(snapshot);
    if (!ok || endError != ERROR_NO_MORE_FILES ||
        DetourAttach(reinterpret_cast<void**>(&gNaturalConstructor),
            reinterpret_cast<void*>(NaturalHook)) != NO_ERROR ||
        DetourAttach(reinterpret_cast<void**>(&gRoomInitialize),
            reinterpret_cast<void*>(RoomPopulationHook)) != NO_ERROR ||
        DetourAttach(reinterpret_cast<void**>(&gLevelInitialize),
            reinterpret_cast<void*>(LevelPopulationHook)) != NO_ERROR) {
        DetourTransactionAbort();
        for (size_t i = 0; i < used; ++i) CloseHandle(threads[i]);
        error = "cannot attach validated natural constructor detour"; return false;
    }
    const LONG committed = DetourTransactionCommit();
    for (size_t i = 0; i < used; ++i) CloseHandle(threads[i]);
    if (committed != NO_ERROR) {
        error = "cannot commit natural constructor detour"; return false;
    }
    gNaturalHookInstalled = true;
    return true;
}

int __stdcall ModeListSetter(StatList* list, DWORD stat, int value, WORD layer) {
    void* caller = _ReturnAddress();
    const int result = gListSetter(list, stat, value, layer);
    // Only the measured last write of the native monster-mode offense refresh.
    // Other lists, stats, units and call sites retain their ordinary behavior.
    if (caller != gModeStatReturn || stat != 19 || layer || !Valid(list) ||
        list->dwUnitType != UNIT_MONSTER) return result;
    std::lock_guard<std::mutex> lock(gManagedMutex);
    for (size_t index = 0; index < gManagedCount; ++index) {
        const Managed& managed = gManaged[index];
        if (managed.id != list->dwUnitId || managed.unit != list->pUnit) continue;
        UnitAny* unit = managed.unit;
        if (!Valid(unit) || unit->dwUnitId != managed.id || unit->dwType != UNIT_MONSTER ||
            unit->pStats != managed.stats) return result;
        constexpr int stats[] = {19, 21, 22};
        for (unsigned i = 0; i < 3; ++i)
            if (managed.present & (1u << i)) SetEffective(unit, stats[i], managed.targets[i]);
        return result;
    }
    return result;
}

bool InstallModeHook(std::string& error) {
    HMODULE game = GetModuleHandleA("D2GAME.dll");
    HMODULE common = GetModuleHandleA("D2Common.dll");
    BYTE* base = reinterpret_cast<BYTE*>(game);
    static const BYTE modeTail[] = {0x6A,0,0x51,0x6A,0x15,0x56,0xE8,0x21,0x32,0xF9,0xFF,
        0x6A,0,0x57,0x6A,0x16,0x56,0xE8,0x16,0x32,0xF9,0xFF,
        0x6A,0,0x55,0x6A,0x13,0x56,0xE8,0x0B,0x32,0xF9,0xFF};
    static const BYTE setter[] = {0x8B,0x44,0x24,0x04,0x85,0xC0,0x74,0x09,0x89,0x44,0x24,0x04,
        0xE9,0xBF,0xFA,0xFF,0xFF,0xC2,0x10,0,0xCC,0xCC,0xCC,0xCC};
    auto original = reinterpret_cast<ListSetter>(GetProcAddress(common, MAKEINTRESOURCEA(10188)));
    auto slot = reinterpret_cast<void**>(base + 0xF8480);
    if (!game || !common || !Bound(base + 0x774B6, modeTail, sizeof(modeTail), game) ||
        reinterpret_cast<BYTE*>(original) != reinterpret_cast<BYTE*>(common) + 0x3A7B0 ||
        !Bound(reinterpret_cast<BYTE*>(original), setter, sizeof(setter), common) ||
        !Readable(slot, sizeof(*slot)) || reinterpret_cast<uintptr_t>(slot) % sizeof(void*) ||
        (*slot != reinterpret_cast<void*>(original) && *slot != reinterpret_cast<void*>(&ModeListSetter))) {
        error = "unexpected native monster-mode refresh or stat-list import"; return false;
    }
    if (*slot == reinterpret_cast<void*>(&ModeListSetter)) return true;
    DWORD protect = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protect)) {
        error = "cannot protect owned mode-refresh import"; return false;
    }
    gListSetter = original;
    gModeStatReturn = base + 0x774D7;
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&ModeListSetter));
    DWORD ignored;
    if (!VirtualProtect(slot, sizeof(*slot), protect, &ignored)) {
        error = "cannot restore owned mode-refresh import protection"; return false;
    }
    return true;
}

bool PrepareManaged(void* game, size_t count) {
    std::lock_guard<std::mutex> lock(gManagedMutex);
    if (gManagedGame != game) { gManagedCount = 0; gManagedGame = game; }
    for (size_t i = 0; i < gManagedCount;) {
        if (LookupMonster(game, gManaged[i].id) != gManaged[i].unit) {
            gManaged[i] = gManaged[--gManagedCount];
        } else ++i;
    }
    return count <= gManaged.size() - gManagedCount;
}

void Manage(UnitAny* unit, const ServerMonsters::Enemy& enemy) {
    std::lock_guard<std::mutex> lock(gManagedMutex);
    Managed& entry = gManaged[gManagedCount++];
    entry = {};
    entry.unit = unit; entry.stats = unit->pStats; entry.id = unit->dwUnitId;
    constexpr unsigned indexes[] = {2, 4, 5};
    for (unsigned i = 0; i < 3; ++i) if (enemy.present & (1u << indexes[i])) {
        entry.present |= 1u << i; entry.targets[i] = enemy.values[indexes[i]];
    }
}
struct Tile { int x, y; };
bool Reserve(Room1* room, UnitAny* player, size_t count, std::vector<Tile>& tiles) {
    if (!Valid(room->Coll)) return false;
    const CollMap* map = room->Coll;
    const uint64_t cells = uint64_t(map->dwSizeGameX) * map->dwSizeGameY;
    if (!cells || cells > 65536 || !Readable(map->pMapStart, static_cast<size_t>(cells) * sizeof(WORD))) return false;
    const int px = player->pPath->xPos, py = player->pPath->yPos;
    for (int radius = 3; radius <= 16 && tiles.size() < count; ++radius) {
        for (int dy = -radius; dy <= radius && tiles.size() < count; ++dy) {
            for (int dx = -radius; dx <= radius && tiles.size() < count; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                const int x = px + dx, y = py + dy;
                if (x < 1 || y < 1 || x > UINT16_MAX - 1 || y > UINT16_MAX - 1) continue;
                bool free = true;
                // Conservative 3x3 footprint and two-tile unit separation.
                for (int oy = -1; oy <= 1 && free; ++oy) for (int ox = -1; ox <= 1; ++ox) {
                    int64_t mx = int64_t(x) + ox - map->dwPosGameX;
                    int64_t my = int64_t(y) + oy - map->dwPosGameY;
                    if (mx < 0 || my < 0 || mx >= map->dwSizeGameX || my >= map->dwSizeGameY ||
                        (map->pMapStart[size_t(my) * map->dwSizeGameX + size_t(mx)] & 0x3E01)) { free = false; break; }
                }
                if (!free) continue;
                for (const Tile& reserved : tiles)
                    if (std::abs(x - reserved.x) <= 2 && std::abs(y - reserved.y) <= 2) { free = false; break; }
                if (!free) continue;
                UnitAny* unit = room->pUnitFirst;
                unsigned visited = 0;
                for (; unit && visited < 4096; unit = unit->pRoomNext, ++visited) {
                    if (!Valid(unit)) return false;
                    if (unit->dwType == UNIT_ITEM) {
                        if (!Valid(unit->pItemPath)) return false;
                        if (std::abs(x - int(unit->pItemPath->dwPosX)) <= 2 &&
                            std::abs(y - int(unit->pItemPath->dwPosY)) <= 2) { free = false; break; }
                    } else if (unit->dwType == UNIT_OBJECT) {
                        if (!Valid(unit->pObjectPath)) return false;
                        if (std::abs(x - int(unit->pObjectPath->dwPosX)) <= 2 &&
                            std::abs(y - int(unit->pObjectPath->dwPosY)) <= 2) { free = false; break; }
                    } else {
                        if (!Valid(unit->pPath)) return false;
                        if (std::abs(x - int(unit->pPath->xPos)) <= 2 &&
                            std::abs(y - int(unit->pPath->yPos)) <= 2) { free = false; break; }
                    }
                }
                if (visited == 4096) return false;
                if (free) tiles.push_back({x, y});
            }
        }
    }
    return tiles.size() == count;
}
}

namespace ServerMonsters {
json NaturalCounters() {
    json samples = json::array();
    {
        std::lock_guard<std::mutex> lock(gNaturalSamplesMutex);
        if (gNaturalGame.load(std::memory_order_acquire)) {
            const size_t count = gNaturalSampleCount.load(std::memory_order_acquire);
            for (size_t i = 0; i < count; ++i) {
                const auto& sample = gNaturalSamples[i];
                samples.push_back({{"class_id", sample.classId},
                    {"produced_class_id", sample.producedClass}, {"cave_id", sample.cave},
                    {"caller_module", sample.gameCaller ? "D2Game.dll" : "foreign"},
                    {"caller_rva", sample.gameCaller ? json(sample.callerRva) : json(nullptr)},
                    {"owner_type", sample.ownerType}, {"owner_id", sample.ownerId},
                    {"population", sample.population},
                    {"spawn_flags", sample.spawnFlags}});
            }
        }
    }
    return {{"population_calls", gPopulationCalls.load(std::memory_order_relaxed)},
        {"class_remapped", gRemappedClasses.load(std::memory_order_relaxed)},
        {"calls", gNaturalCounters[0].load(std::memory_order_relaxed)},
        {"game_matched", gNaturalCounters[1].load(std::memory_order_relaxed)},
        {"cave_matched", gNaturalCounters[2].load(std::memory_order_relaxed)},
        {"class_matched", gNaturalCounters[3].load(std::memory_order_relaxed)},
        {"applied", gNaturalCounters[4].load(std::memory_order_relaxed)},
        {"sample_capacity", gNaturalSamples.size()}, {"sample_count", samples.size()},
        {"samples", std::move(samples)}};
}
bool EnableNaturalZone(void* game, const std::string& epoch, int level,
    const std::array<int, 4>& monsterClasses, unsigned monsterClassCount,
    int monsterLevel, std::string& error) {
    if (!game || !((level >= 13 && level <= 16) || (level >= 138 && level <= 254)) ||
        !monsterClassCount || monsterClassCount > monsterClasses.size() ||
        monsterLevel < 1 || monsterLevel > 99 || !InstallNaturalHook(error))
        return false;
    for (unsigned index = 0; index < monsterClassCount; ++index) {
        if (monsterClasses[index] < 0 || monsterClasses[index] > 32767) {
            error = "natural cave roster contains an invalid monster class";
            return false;
        }
        for (unsigned previous = 0; previous < index; ++previous)
            if (monsterClasses[previous] == monsterClasses[index]) {
                error = "natural cave roster contains duplicate classes";
                return false;
            }
    }
    if (gNaturalGame.load(std::memory_order_acquire) != game ||
        gNaturalEpoch != epoch) {
        gNaturalGame.store(nullptr, std::memory_order_release);
        for (auto& areaTargets : gNaturalTargets)
            for (auto& selected : areaTargets)
                selected.store(0, std::memory_order_release);
        for (auto& counter : gNaturalCounters) counter.store(0, std::memory_order_relaxed);
        gPopulationCalls.store(0, std::memory_order_relaxed);
        gRemappedClasses.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(gNaturalSamplesMutex);
            gNaturalSampleCount.store(0, std::memory_order_release);
        }
        gNaturalEpoch = epoch;
        gNaturalGame.store(game, std::memory_order_release);
    }
    if (gNaturalTargets[level][0].load(std::memory_order_acquire)) {
        error = "natural cave level already configured"; return false;
    }
    for (unsigned index = 0; index < monsterClassCount; ++index)
        gNaturalTargets[level][index].store(
            (uint32_t(monsterClasses[index]) << 16) |
                uint32_t(monsterLevel), std::memory_order_release);
    return true;
}
json AttributeSchema() {
    json properties = json::object();
    for (size_t i = 0; i < kAttributeCount; ++i)
        properties[kNames[i]] = {{"type", "integer"}, {"minimum", kMinimum[i]}, {"maximum", kMaximum[i]}};
    return {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)},
        {"dependencies", {{"min_damage", json::array({"max_damage"})}, {"max_damage", json::array({"min_damage"})}}}};
}

bool Parse(const json& args, Operation operation, Request& output, std::string& error) {
    Request candidate;
    candidate.operation = operation;
    if (!args.is_object() || !args.contains("request_id") || !args.at("request_id").is_string()) {
        error = "request_id is required"; return false;
    }
    const std::string id = args.at("request_id").get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) {
        error = "request_id must be 32 lowercase hex characters"; return false;
    }
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "request_id" &&
            !(operation == Operation::Observe && it.key() == "expected_epoch") &&
            !(operation == Operation::Observe && it.key() == "unit_ids") &&
            !(operation == Operation::Inspect && it.key() == "unit_ids") &&
            !(operation == Operation::Spawn && it.key() == "enemies")) {
            error = "unknown enemy request field"; return false;
        }
    if (operation == Operation::Observe && args.contains("expected_epoch")) {
        const json& expected = args.at("expected_epoch");
        if (!expected.is_string()) { error = "expected_epoch must be an epoch string"; return false; }
        candidate.expectedEpoch = expected.get<std::string>();
        if (candidate.expectedEpoch.empty() || candidate.expectedEpoch.size() > 128 ||
            candidate.expectedEpoch.find_first_not_of("0123456789abcdef-") != std::string::npos) {
            error = "expected_epoch must be a valid epoch string"; return false;
        }
    }
    Room1* room = nullptr;
    DWORD level = 0;
    if (!Context(D2CLIENT_GetPlayerUnit(), room, level)) { error = "client player room unavailable"; return false; }
    if (operation == Operation::Spawn && IsTown(level)) { error = "enemy creation is forbidden in town"; return false; }
    if (!Bind(error)) return false;
    if (operation == Operation::Area) {
        output = std::move(candidate);
        return true;
    }
    if (operation == Operation::Inspect || operation == Operation::Observe) {
        if ((operation == Operation::Inspect && !args.contains("unit_ids")) ||
            (args.contains("unit_ids") && (!args.at("unit_ids").is_array() ||
                args.at("unit_ids").empty() || args.at("unit_ids").size() > 16))) {
            error = "unit_ids must contain 1..16 IDs"; return false;
        }
        if (args.contains("unit_ids")) for (const json& value : args.at("unit_ids")) {
            int64_t idValue;
            if (!Integer(value, 0, UINT32_MAX, idValue) ||
                std::find(candidate.unitIds.begin(), candidate.unitIds.end(), static_cast<DWORD>(idValue)) != candidate.unitIds.end()) {
                error = "unit_ids must be distinct uint32 values"; return false;
            }
            candidate.unitIds.push_back(static_cast<DWORD>(idValue));
        }
    } else {
        sgptDataTable* table = Tables();
        if (!table) { error = "live monster table unavailable"; return false; }
        if (!args.contains("enemies") || !args.at("enemies").is_array() ||
            args.at("enemies").empty() || args.at("enemies").size() > 16) {
            error = "enemies must contain 1..16 entries"; return false;
        }
        unsigned total = 0;
        for (const json& spec : args.at("enemies")) {
            if (!spec.is_object() ||
                spec.contains("class_id") == spec.contains("superunique_id")) {
                error = "provide exactly one of class_id or superunique_id"; return false;
            }
            for (auto it = spec.begin(); it != spec.end(); ++it)
                if (it.key() != "class_id" && it.key() != "superunique_id" &&
                    it.key() != "rank" && it.key() != "count" && it.key() != "attributes") {
                    error = "unknown enemy entry field"; return false;
                }
            int64_t number;
            Enemy enemy = {};
            enemy.count = 1;
            if (spec.contains("superunique_id")) {
                const SuperUniqueRow* rows = SuperRows(table);
                if (!rows || !Integer(spec.at("superunique_id"), 0, table->dwSuperUniquesRecs - 1, number) ||
                    rows[number].classId < 0 ||
                    static_cast<DWORD>(rows[number].classId) >= table->dwMonStatsRecs ||
                    !Spawnable(table->pMonStatsTxt[rows[number].classId])) {
                    error = "superunique_id has no spawnable native definition"; return false;
                }
                enemy.superuniqueId = static_cast<int>(number);
                enemy.classId = rows[number].classId;
            } else {
                if (!Integer(spec.at("class_id"), 0, table->dwMonStatsRecs - 1, number) ||
                    !Spawnable(table->pMonStatsTxt[number])) {
                    error = "class_id is not a spawnable native monster"; return false;
                }
                enemy.classId = static_cast<int>(number);
            }
            if (spec.contains("rank")) {
                if (!spec.at("rank").is_string() || enemy.superuniqueId >= 0) {
                    error = "rank requires a class_id and normal/champion/unique"; return false;
                }
                const std::string rank = spec.at("rank").get<std::string>();
                if (rank == "champion") enemy.rank = Rank::Champion;
                else if (rank == "unique") enemy.rank = Rank::Unique;
                else if (rank != "normal") {
                    error = "rank must be normal, champion or unique"; return false;
                }
            }
            if ((enemy.superuniqueId >= 0 || enemy.rank != Rank::Normal) &&
                (!BindRank(error) || !ModifierRows(table))) {
                if (error.empty()) error = "native unique modifier table unavailable";
                return false;
            }
            if (spec.contains("count")) {
                if (!Integer(spec.at("count"), 1, 16, number) ||
                    (enemy.superuniqueId >= 0 && number != 1)) {
                    error = "count must be 1..16 (one for a native superunique)"; return false;
                }
                enemy.count = static_cast<int>(number);
            }
            total += enemy.count;
            if (enemy.superuniqueId >= 0) {
                const SuperUniqueRow& row = SuperRows(table)[enemy.superuniqueId];
                if (!row.stacks && std::any_of(candidate.enemies.begin(), candidate.enemies.end(),
                    [&](const Enemy& previous) { return previous.superuniqueId == enemy.superuniqueId; })) {
                    error = "non-stacking native superunique requested twice"; return false;
                }
                if (row.minGroup < 0 || row.maxGroup < row.minGroup ||
                    row.maxGroup > 256) {
                    error = "superunique native table group exceeds supported bound"; return false;
                }
            }
            if (total > 16) { error = "requested enemy leaders must not exceed 16"; return false; }
            if (spec.contains("attributes")) {
                const json& attributes = spec.at("attributes");
                if (!attributes.is_object()) { error = "attributes must be an object"; return false; }
                for (auto it = attributes.begin(); it != attributes.end(); ++it) {
                    size_t index = 0;
                    while (index < kAttributeCount && it.key() != kNames[index]) ++index;
                    if (index == kAttributeCount || !Integer(it.value(), kMinimum[index], kMaximum[index], number)) {
                        error = "unknown or out-of-range enemy attribute: " + it.key(); return false;
                    }
                    enemy.values[index] = static_cast<int>(number);
                    enemy.present |= static_cast<uint16_t>(1u << index);
                }
            }
            if (bool(enemy.present & (1u << 4)) != bool(enemy.present & (1u << 5)) ||
                ((enemy.present & (1u << 4)) && enemy.values[4] > enemy.values[5])) {
                error = "min_damage and max_damage must be provided together and ordered"; return false;
            }
            candidate.enemies.push_back(enemy);
        }
    }
    output = std::move(candidate);
    return true;
}

json Catalog(const json& args) {
    if (!args.is_object()) return Failure("catalog arguments must be an object");
    for (auto it = args.begin(); it != args.end(); ++it)
        if (it.key() != "offset" && it.key() != "limit" && it.key() != "table")
            return Failure("unknown catalog field");
    if (args.contains("table") && !args.at("table").is_string())
        return Failure("table must be a string");
    const std::string selector = args.value("table", std::string("monsters"));
    if (selector != "monsters" && selector != "superuniques" && selector != "modifiers")
        return Failure("table must be monsters, superuniques or modifiers");
    std::string error;
    if (!Bind(error)) return Failure(error);
    sgptDataTable* table = Tables();
    if (!table) return Failure("live monster table unavailable");
    const SuperUniqueRow* supers = selector == "superuniques" ? SuperRows(table) : nullptr;
    const UniqueModifierRow* mods = selector == "modifiers" ? ModifierRows(table) : nullptr;
    if ((selector == "superuniques" && !supers) || (selector == "modifiers" && !mods))
        return Failure("selected native table unavailable");
    const DWORD count = selector == "superuniques" ? table->dwSuperUniquesRecs :
        selector == "modifiers" ? table->dwMonUModRecs : table->dwMonStatsRecs;
    int64_t offset = 0, limit = 64;
    if ((args.contains("offset") && !Integer(args.at("offset"), 0, count, offset)) ||
        (args.contains("limit") && !Integer(args.at("limit"), 1, 64, limit)))
        return Failure("catalog offset or limit out of range");
    json result = {{"ok", true}, {"source", "native_toolkit"}, {"table", selector},
        {"offset", offset}, {"limit", limit}, {"total", count}, {"records", json::array()}};
    for (DWORD id = static_cast<DWORD>(offset); id < count && id < offset + limit; ++id) {
        if (selector == "superuniques") {
            const SuperUniqueRow& row = supers[id];
            const bool eligible = row.classId >= 0 && static_cast<DWORD>(row.classId) < table->dwMonStatsRecs &&
                Spawnable(table->pMonStatsTxt[row.classId]) &&
                row.minGroup >= 0 && row.maxGroup >= row.minGroup && row.maxGroup <= 256;
            result["records"].push_back({{"superunique_id", id}, {"class_id", row.classId},
                {"name", WideUtf8(D2LANG_GetLocaleText(row.name))}, {"spawnable", eligible},
                {"modifier_ids", {row.mods[0], row.mods[1], row.mods[2]}},
                {"min_group", row.minGroup}, {"max_group", row.maxGroup},
                {"auto_position", bool(row.autoPosition)}, {"stacks", bool(row.stacks)}});
        } else if (selector == "modifiers") {
            const UniqueModifierRow& row = mods[id];
            result["records"].push_back({{"modifier_id", id}, {"catalog_id", row.id},
                {"enabled", bool(row.enabled)}, {"champion", bool(row.champion)},
                {"champion_pick", {row.championPick[0], row.championPick[1], row.championPick[2]}},
                {"unique_pick", {row.uniquePick[0], row.uniquePick[1], row.uniquePick[2]}},
                {"constants", row.constants}});
        } else {
            const MonStatsTxt& row = table->pMonStatsTxt[id];
            result["records"].push_back({{"class_id", id},
                {"name", WideUtf8(D2LANG_GetLocaleText(row.wNameStr))},
                {"spawnable", Spawnable(row)}, {"enabled", bool(row.dwFlags.bEnabled)},
                {"killable", bool(row.dwFlags.bKillable)}, {"npc", bool(row.dwFlags.bNPC)},
                {"town", bool(row.dwFlags.binTown)},
                {"set_boss", bool(row.dwFlags.bSetBoss)},
                {"prime_evil", bool(row.dwFlags.bPrimeEvil)}});
        }
    }
    return result;
}

json ObserveSnapshot(void* game, UnitAny* serverPlayer, const Request& request,
    bool includeRuntimeIdentity) {
    Room1* room = nullptr;
    DWORD level = 0;
    if (!game || !Context(serverPlayer, room, level)) return Failure("server player context unavailable");
    std::string error;
    if (!Bind(error)) return Failure(error);
    json result = {{"ok", true}, {"status", "observed"}, {"source", "offline_server"},
        {"player", json::object()}, {"enemies", json::array()},
        {"coverage", {{"player", true}, {"player_vitals", false}, {"selected_skills", false},
            {"selected_enemies", "requested_ids_only"}, {"requested_enemy_count", request.unitIds.size()},
            {"area_census", false}, {"runtime_data", false}, {"partial", false}}}};
    json& player = result["player"];
    player = {{"unit_id", serverPlayer->dwUnitId}, {"class_id", serverPlayer->dwTxtFileNo},
        {"mode", serverPlayer->dwMode}, {"area_id", level},
        {"position", {{"x", serverPlayer->pPath->xPos}, {"y", serverPlayer->pPath->yPos}}},
        {"life_raw", nullptr}, {"max_life_raw", nullptr}, {"mana_raw", nullptr},
        {"max_mana_raw", nullptr}, {"skill_delay_active", nullptr},
        {"left_skill", nullptr}, {"right_skill", nullptr}};
    player["name"] = nullptr;
    if (Valid(serverPlayer->pPlayerData)) {
        size_t length = strnlen_s(serverPlayer->pPlayerData->szName, 16);
        if (length && length < 16)
            player["name"] = std::string(serverPlayer->pPlayerData->szName, length);
        else result["coverage"]["partial"] = true;
    } else result["coverage"]["partial"] = true;
    if (Valid(serverPlayer->pStats)) {
        player["life_raw"] = gGetter(serverPlayer, 6, 0);
        player["max_life_raw"] = gGetter(serverPlayer, 7, 0);
        player["mana_raw"] = gGetter(serverPlayer, 8, 0);
        player["max_mana_raw"] = gGetter(serverPlayer, 9, 0);
        player["skill_delay_active"] = D2COMMON_GetUnitState(serverPlayer, 121) != 0;
        result["coverage"]["player_vitals"] = true;
    } else result["coverage"]["partial"] = true;
    if (Valid(serverPlayer->pInfo)) {
        bool skillsReadable = true;
        auto selected = [&](Skill* skill) -> json {
            if (!skill) return nullptr;
            if (!Valid(skill) || !Valid(skill->pSkillInfo)) {
                skillsReadable = false;
                return nullptr;
            }
            return {{"id", skill->pSkillInfo->wSkillId}, {"hard_points", skill->skillLevel},
                {"effective_level", D2COMMON_GetSkillLevel(serverPlayer, skill, TRUE)},
                {"use_state", D2COMMON_GetSkillUseState(serverPlayer, skill)},
                {"animation_mode", D2COMMON_GetSkillMode(serverPlayer, skill)}};
        };
        player["left_skill"] = selected(serverPlayer->pInfo->pLeftSkill);
        player["right_skill"] = selected(serverPlayer->pInfo->pRightSkill);
        result["coverage"]["selected_skills"] = skillsReadable;
        if (!skillsReadable) result["coverage"]["partial"] = true;
    } else result["coverage"]["partial"] = true;
    for (DWORD id : request.unitIds) {
        UnitAny* unit = LookupMonster(game, id);
        if (!unit) {
            result["enemies"].push_back({{"id", id}, {"present", false}, {"dead", nullptr},
                {"coverage", "missing"}});
            continue;
        }
        if (!Valid(unit) || unit->dwType != UNIT_MONSTER || unit->dwUnitId != id ||
            !Valid(unit->pPath) || !ObservedRoom(serverPlayer, unit->pPath->pRoom1, level) ||
            !Valid(unit->pStats) || !Valid(unit->pMonsterData) ||
            !Valid(unit->pMonsterData->pMonStatsTxt) ||
            unit->pMonsterData->pMonStatsTxt->dwFlags.bNPC ||
            unit->pMonsterData->pMonStatsTxt->dwFlags.binTown) {
            result["enemies"].push_back({{"id", id}, {"present", nullptr}, {"dead", nullptr},
                {"coverage", "outside_selected_server_scope_or_unreadable"}});
            result["coverage"]["partial"] = true;
        } else {
            json enemy = State(unit);
            enemy["coverage"] = "selected_server_state";
            result["enemies"].push_back(std::move(enemy));
        }
    }
    // Bind verifies the pinned server file and callable code bytes. Loaded table
    // hashes are captured once per epoch by the collector, not on every sample.
    if (includeRuntimeIdentity) {
        result["runtime_identity"] = {{"verified_server_file_sha256",
            "24b293b86f95bea0228826c502ea04737ba21b91ca9832e52aac4b11fef14f53"},
            {"tables", json::object()}};
        json& identities = result["runtime_identity"]["tables"];
        sgptDataTable* table = Tables();
        if (table) {
            identities["monsters"] = LiveTableDigest(table->pMonStatsTxt, table->dwMonStatsRecs, sizeof(MonStatsTxt));
            identities["missiles"] = LiveTableDigest(table->pMissilesTxt, table->dwMissilesRecs, sizeof(MissilesTxt));
            identities["skills"] = LiveTableDigest(table->pSkillsTxt, table->dwSkillsRecs, sizeof(SkillsTxt));
            identities["item_stats"] = LiveTableDigest(table->pItemStatCostTxt,
                table->dwItemStatCostRecs, sizeof(ItemStatCostTxt));
            identities["item_types"] = LiveTableDigest(table->pItemsTypeTxt,
                table->dwItemsTypeRecs, sizeof(D2ItemTypesTxt));
        }
        D2ItemDataTbl* items = D2COMMON_10535_DATATBLS_GetItemDataTables();
        identities["items"] = Valid(items) && items->nItemsTxtRecordCount > 0 ?
            LiveTableDigest(items->pItemsTxt, static_cast<DWORD>(items->nItemsTxtRecordCount),
                sizeof(ItemsTxt)) : json{{"sha256",nullptr},{"rows",nullptr},{"covered",false}};
        bool complete = identities.size() == 6;
        for (auto it = identities.begin(); it != identities.end(); ++it)
            complete = complete && it.value().value("covered", false);
        result["coverage"]["runtime_data"] = complete;
        if (!complete) result["coverage"]["partial"] = true;
    }
    return result;
}

json Execute(void* game, UnitAny* serverPlayer, const Request& request) {
    Room1* room = nullptr;
    DWORD level = 0;
    if (!game || !Context(serverPlayer, room, level)) return Failure("server player context unavailable");
    std::string error;
    if (!Bind(error)) return Failure(error);
    if (request.operation == Operation::Area) {
        if (!CharacterState::BindingsReady() || !Readable(game, 0xAC))
            return Failure("native server player readiness bindings unavailable");
        Level* area = room->pRoom2->pLevel;
        auto bounds = [](Room2* metadata) {
            return json{{"x", metadata->dwPosX * 5}, {"y", metadata->dwPosY * 5},
                        {"width", metadata->dwSizeX * 5}, {"height", metadata->dwSizeY * 5}};
        };
        json result = {{"ok", true}, {"status", "observed"}, {"source", "offline_server"},
                       {"level_id", level}, {"player_room", bounds(room->pRoom2)},
                       {"rooms", json::array()}, {"enemies", json::array()},
                       {"room_catalog_complete", true}, {"truncated", false}};
        result["player"] = {{"unit_id", serverPlayer->dwUnitId}, {"class_id", serverPlayer->dwTxtFileNo},
            {"mode", serverPlayer->dwMode},
            {"level", gGetter(serverPlayer, 12, 0)},
            {"experience", static_cast<DWORD>(gGetter(serverPlayer, 13, 0))},
            {"skill_delay_active", D2COMMON_GetUnitState(serverPlayer, 121) != 0},
            {"position", {{"x", serverPlayer->pPath->xPos}, {"y", serverPlayer->pPath->yPos}}},
            {"life", gGetter(serverPlayer, 6, 0) / 256.0},
            {"mana", gGetter(serverPlayer, 8, 0) / 256.0},
            {"max_life", gGetter(serverPlayer, 7, 0) / 256.0},
            {"max_mana", gGetter(serverPlayer, 9, 0) / 256.0}};
        // S13 D2Common ordinal 10334 writes the computed path count at Path+0x28.
        const Path* path = serverPlayer->pPath;
        result["player"]["path_result"] = {
            {"target", {{"x", path->xTarget}, {"y", path->yTarget}}},
            {"point_count", path->_3[1]}, {"flags", path->dwFlags},
            {"path_type", path->dwPathType}};
        result["game_frame"] = *reinterpret_cast<const DWORD*>(reinterpret_cast<const BYTE*>(game) + 0xA8);
        auto selectedSkill = [&](Skill* selected) -> json {
            if (!Valid(selected) || !Valid(selected->pSkillInfo)) return nullptr;
            return {{"id", selected->pSkillInfo->wSkillId},
                {"hard_points", selected->skillLevel},
                {"effective_level", D2COMMON_GetSkillLevel(serverPlayer, selected, TRUE)},
                {"use_state", D2COMMON_GetSkillUseState(serverPlayer, selected)},
                {"animation_mode", D2COMMON_GetSkillMode(serverPlayer, selected)}};
        };
        result["player"]["right_skill"] = Valid(serverPlayer->pInfo) ?
            selectedSkill(serverPlayer->pInfo->pRightSkill) : json(nullptr);
        result["player"]["left_skill"] = Valid(serverPlayer->pInfo) ?
            selectedSkill(serverPlayer->pInfo->pLeftSkill) : json(nullptr);
        unsigned roomCount = 0, unitCount = 0;
        for (Room2* metadata = area->pRoom2First; metadata; metadata = metadata->pRoom2Next) {
            if (++roomCount > 256 || !Valid(metadata) || metadata->pLevel != area)
                return Failure("server level room metadata invalid or exceeds census bound");
            Room1* active = metadata->pRoom1;
            const bool loaded = active != nullptr;
            if (loaded && (!Valid(active) || active->pRoom2 != metadata))
                return Failure("server active room identity invalid or unreadable");
            result["rooms"].push_back({{"bounds", bounds(metadata)}, {"loaded", loaded}});
            if (!loaded) continue;
            unsigned chainLength = 0;
            for (UnitAny* unit = active->pUnitFirst; unit; unit = unit->pRoomNext) {
                if (++chainLength > 4096 || ++unitCount > 8192 || !Valid(unit))
                    return Failure("server room unit chain invalid or exceeds census bound");
                if (unit->dwType != UNIT_MONSTER) continue;
                if (!Valid(unit->pPath) || unit->pPath->pRoom1 != active ||
                    !Valid(unit->pStats) || !Valid(unit->pMonsterData) ||
                    !Valid(unit->pMonsterData->pMonStatsTxt))
                    return Failure("server monster census record invalid");
                const MonStatsTxt& txt = *unit->pMonsterData->pMonStatsTxt;
                if (!txt.dwFlags.bKillable || txt.dwFlags.bNPC || txt.dwFlags.binTown ||
                    gGetter(unit, STAT_ALIGNMENT, 0) != 0) continue;
                if (result["enemies"].size() >= 1024)
                    return Failure("server enemy census exceeds bound");
                json enemy = State(unit);
                enemy["room"] = bounds(metadata);
                result["enemies"].push_back(std::move(enemy));
            }
        }
        AreaPopulation::AppendInactive(game, serverPlayer, result, error);
        return result;
    }
    if (request.operation == Operation::Inspect) {
        json result = {{"ok", true}, {"status", "observed"}, {"source", "offline_server"}, {"enemies", json::array()}};
        for (DWORD id : request.unitIds) {
            UnitAny* unit = LookupMonster(game, id);
            if (!Valid(unit) || unit->dwType != UNIT_MONSTER || unit->dwUnitId != id ||
                !Valid(unit->pPath) || !ObservedRoom(serverPlayer, unit->pPath->pRoom1, level) ||
                !Valid(unit->pStats) || !Valid(unit->pMonsterData) ||
                !Valid(unit->pMonsterData->pMonStatsTxt) ||
                unit->pMonsterData->pMonStatsTxt->dwFlags.bNPC ||
                unit->pMonsterData->pMonStatsTxt->dwFlags.binTown) {
                result["enemies"].push_back({{"id", id}, {"present", false}});
            } else result["enemies"].push_back(State(unit));
        }
        return result;
    }
    if (IsTown(level)) return Failure("enemy creation is forbidden in town");
    if (request.encounterLevel &&
        (request.encounterLevel < 1 || request.encounterLevel > 99 ||
         request.enemies.size() != 1 ||
         std::any_of(request.enemies.begin(), request.enemies.end(),
             [](const Enemy& enemy) {
                 return enemy.count != 1 ||
                     (enemy.rank != Rank::Normal && enemy.rank != Rank::Unique);
             })))
        return Failure("zone encounter must use one native normal/unique/superunique group");
    const int combatRunDepth = request.encounterLevel ?
        ServerSpawns::CombatRunDepth(game, static_cast<int>(level)) : 0;
    sgptDataTable* table = Tables();
    if (!table) return Failure("live monster table unavailable");
    size_t total = 0;
    for (const Enemy& enemy : request.enemies) {
        if (enemy.classId < 0 || static_cast<DWORD>(enemy.classId) >= table->dwMonStatsRecs ||
            !Spawnable(table->pMonStatsTxt[enemy.classId]) || enemy.count < 1 || enemy.count > 16 ||
            (total += enemy.count) > 16) return Failure("monster specification changed before server execution");
        if (enemy.superuniqueId >= 0) {
            const SuperUniqueRow* rows = SuperRows(table);
            if (!rows || static_cast<DWORD>(enemy.superuniqueId) >= table->dwSuperUniquesRecs ||
                rows[enemy.superuniqueId].classId != enemy.classId ||
                rows[enemy.superuniqueId].maxGroup < 0 ||
                rows[enemy.superuniqueId].maxGroup < rows[enemy.superuniqueId].minGroup ||
                rows[enemy.superuniqueId].maxGroup > 256 || enemy.count != 1)
                return Failure("native superunique definition changed");
            const size_t bossByte = 0x1D30 + static_cast<unsigned>(enemy.superuniqueId) / 8;
            if (!Readable(game, bossByte + 1))
                return Failure("native superunique encounter bits unavailable");
            if (!rows[enemy.superuniqueId].stacks &&
                (*(reinterpret_cast<const BYTE*>(game) + bossByte) &
                 (1u << (static_cast<unsigned>(enemy.superuniqueId) & 7))))
                return Failure("non-stacking superunique already spawned in this game; save/exit and load to start a fresh encounter");
        }
        if ((enemy.superuniqueId >= 0 || enemy.rank != Rank::Normal) &&
            (!BindRank(error) || !ModifierRows(table)))
            return Failure(error.empty() ? "native unique modifier table changed" : error);
    }
    if (!total || !Valid(room->Coll)) return Failure("server collision room unavailable");
    std::vector<Tile> tiles;
    tiles.reserve(total);
    if (!Reserve(room, serverPlayer, total, tiles)) return Failure("insufficient nonoverlapping safe tiles in loaded server room");
    if (!PrepareManaged(game, total)) return Failure("controlled enemy registry capacity exhausted");
    if (!InstallModeHook(error)) return Failure(error);
    Room1* const reservedRoom = room;
    CollMap* const reservedMap = room->Coll;
    const DWORD reservedLevel = level;
    std::vector<UnitAny*> beforeUnits;
    if (!CollectAreaMonsters(reservedRoom->pRoom2->pLevel, beforeUnits, error))
        return Failure(error);
    std::vector<DWORD> beforeIds;
    beforeIds.reserve(beforeUnits.size());
    for (UnitAny* unit : beforeUnits) beforeIds.push_back(unit->dwUnitId);
    json result = {{"ok", true}, {"status", "spawned"}, {"source", "offline_server"},
        {"requested_count", total}, {"spawned_count", 0}, {"enemies", json::array()},
        {"additional_enemies", json::array()}, {"created_unit_count", 0},
        {"group_observation_complete", false}};
    size_t index = 0;
    auto finishGroup = [&]() {
        result["additional_enemies"] = json::array();
        beforeUnits.clear();
        std::string censusError;
        if (!CollectAreaMonsters(reservedRoom->pRoom2->pLevel, beforeUnits, censusError)) {
            result["group_observation_error"] = censusError;
            return false;
        }
        for (UnitAny* unit : beforeUnits) {
            if (std::binary_search(beforeIds.begin(), beforeIds.end(), unit->dwUnitId))
                continue;
            bool primary = false;
            for (const json& row : result["enemies"])
                if (row["id"] == unit->dwUnitId) { primary = true; break; }
            if (primary) continue;
            int nativeLevel = 0;
            const bool nativeSuperunique = request.encounterLevel &&
                request.enemies[0].superuniqueId >= 0;
            if (request.encounterLevel &&
                (unit->dwType != UNIT_MONSTER ||
                 (!nativeSuperunique &&
                  std::none_of(request.enemies.begin(), request.enemies.end(),
                      [unit](const Enemy& enemy) {
                          return unit->dwTxtFileNo == static_cast<DWORD>(enemy.classId);
                      })) ||
                 result["additional_enemies"].size() >= 16 ||
                 !Valid(unit->pPath) || !Valid(unit->pStats) ||
                 !ObservedRoom(serverPlayer, unit->pPath->pRoom1, level) ||
                 !ScaleEncounterMonster(unit, request.encounterLevel, combatRunDepth,
                     nativeLevel))) {
                result["group_observation_error"] = "additional monster combat stat scaling could not be certified";
                return false;
            }
            if (request.encounterLevel)
                ServerSpawns::ApplyStockMapMonster(game, unit, level);
            json row = State(unit);
            if (request.encounterLevel) row["native_level"] = nativeLevel;
            row["spawn_role"] = "native_additional";
            result["additional_enemies"].push_back(std::move(row));
        }
        result["created_unit_count"] = result["enemies"].size() + result["additional_enemies"].size();
        result["group_observation_complete"] = true;
        return true;
    };
    auto partial = [&](const std::string& reason) {
        result["ok"] = false;
        result["status"] = "partial";
        result["error"] = reason + "; inspect returned IDs, do not retry blindly";
        finishGroup();
        return result;
    };
    for (const Enemy& enemy : request.enemies) for (int n = 0; n < enemy.count; ++n, ++index) {
        if (!Context(serverPlayer, room, level) || room != reservedRoom || level != reservedLevel ||
            IsTown(level) || !Valid(room->Coll) || room->Coll != reservedMap ||
            uint64_t(reservedMap->dwSizeGameX) * reservedMap->dwSizeGameY == 0 ||
            uint64_t(reservedMap->dwSizeGameX) * reservedMap->dwSizeGameY > 65536 ||
            !Readable(reservedMap->pMapStart,
                size_t(reservedMap->dwSizeGameX) * reservedMap->dwSizeGameY * sizeof(WORD)))
            return partial("server room changed during construction");
        UnitAny* unit = nullptr;
        if (enemy.superuniqueId >= 0) {
            if (!Readable(game, 0x1D31) ||
                *(reinterpret_cast<const BYTE*>(game) + 0x6D) >= 3)
                return partial("native game difficulty unavailable");
            unit = gSuperSpawn(game, room, tiles[index].x, tiles[index].y, enemy.superuniqueId);
        } else {
            ConstructorRecord record = {game, room, nullptr, enemy.classId, NPC_MODE_STAND, 0,
                tiles[index].x, tiles[index].y, -1, 0x42};
            unit = gConstructor(&record);
        }
        if (!Valid(unit)) return partial("native monster spawn did not return a unit");
        // Record the ID before any later validation or stat operation: the unit may already exist.
        result["enemies"].push_back({{"id", unit->dwUnitId}, {"present", true}});
        result["spawned_count"] = result["enemies"].size();
        if (unit->dwType != UNIT_MONSTER || unit->dwTxtFileNo != static_cast<DWORD>(enemy.classId) ||
            !Valid(unit->pPath) || !Valid(unit->pStats) || !Valid(unit->pMonsterData) ||
            !ObservedRoom(serverPlayer, unit->pPath->pRoom1, level) ||
            (enemy.superuniqueId < 0 &&
                (unit->pPath->xPos != tiles[index].x || unit->pPath->yPos != tiles[index].y)) ||
            unit->dwMode == NPC_MODE_DEATH || unit->dwMode == NPC_MODE_DEAD)
            return partial("native monster identity, location or initialization mismatched");
        int nativeLevel = 0;
        if (request.encounterLevel &&
            !ScaleEncounterMonster(unit, request.encounterLevel, combatRunDepth,
                nativeLevel))
            return partial("empty-room encounter combat stats could not be scaled");
        if (request.encounterLevel)
            ServerSpawns::ApplyStockMapMonster(game, unit, level);
        if (enemy.rank == Rank::Unique) {
            MarkUnique(game, unit);
            RollUnique(game, unit);
            ApplyUnique(game, unit);
        } else if (enemy.rank == Rank::Champion) {
            const int selected = RollChampion(game, unit);
            if (selected <= 0 || selected > 255 ||
                !ModifierRows(table) || static_cast<DWORD>(selected) >= table->dwMonUModRecs)
                return partial("native champion modifier selection failed");
            InitializeChampion(game, unit, selected);
        }
        const json identity = Identity(unit);
        if (enemy.superuniqueId >= 0 &&
            (identity.value("rank", std::string()) != "superunique" ||
             identity.value("superunique_id", json(nullptr)) != enemy.superuniqueId))
            return partial("native superunique identity mismatched");
        if (enemy.rank == Rank::Unique && identity.value("rank", std::string()) != "unique")
            return partial("native unique rank mismatched");
        if (enemy.rank == Rank::Champion && identity.value("rank", std::string()) != "champion")
            return partial("native champion rank mismatched");
        for (size_t i = 0; i < kAttributeCount; ++i) if (enemy.present & (1u << i)) {
            if (i == 1) {
                const int scaled = enemy.values[i] << 8;
                if (!SetEffective(unit, 7, scaled) || !SetEffective(unit, 6, scaled))
                    return partial("native life compensation overflowed");
            } else if (!SetEffective(unit, kStats[i], enemy.values[i]))
                return partial("native stat compensation overflowed");
        }
        result["enemies"].back() = State(unit);
        if (request.encounterLevel)
            result["enemies"].back()["native_level"] = nativeLevel;
        for (size_t i = 0; i < kAttributeCount; ++i) if (enemy.present & (1u << i)) {
            const int expected = i == 1 ? enemy.values[i] << 8 : enemy.values[i];
            const int observed = static_cast<int>(gGetter(unit, kStats[i], 0));
            if (observed != expected || (i == 1 && static_cast<int>(gGetter(unit, 7, 0)) != expected)) {
                result["mismatch"] = {{"attribute", kNames[i]}, {"expected", expected},
                    {"observed", observed}, {"base", static_cast<int>(gBaseGetter(unit, kStats[i], 0))}};
                return partial("native enemy stat readback mismatched");
            }
        }
        Manage(unit, enemy);
    }
    if (!finishGroup()) {
        result["ok"] = false;
        result["status"] = "partial";
        result["error"] = "native group observation or level scaling incomplete; inspect IDs, do not retry";
    }
    return result;
}
json SpawnEventNpc(void* game, UnitAny* player, int classId, int x, int y) {
    Room1* room = nullptr;
    DWORD level = 0;
    std::string error;
    if (!game || !Context(player, room, level) || IsTown(level) || !Bind(error))
        return Failure("field NPC constructor context unavailable: " + error);
    sgptDataTable* table = Tables();
    if (!table || classId < 0 || static_cast<DWORD>(classId) >= table->dwMonStatsRecs ||
        !table->pMonStatsTxt[classId].dwFlags.bNPC ||
        !table->pMonStatsTxt[classId].dwFlags.bInteract)
        return Failure("selected class is not an interactable stock NPC");
    // The run controller reserves a reachable, mutually spaced tile for
    // the whole event; the stock constructor still owns NPC identity/menus.
    ConstructorRecord record = {game, room, nullptr, classId, NPC_MODE_STAND, 0,
        x, y, -1, 0x42};
    UnitAny* npc = gConstructor(&record);
    if (!Valid(npc) || npc->dwType != UNIT_MONSTER ||
        npc->dwTxtFileNo != static_cast<DWORD>(classId) ||
        !Valid(npc->pPath) || npc->pPath->pRoom1 != room ||
        npc->pPath->xPos != x || npc->pPath->yPos != y ||
        !Valid(npc->pMonsterData) || !Valid(npc->pMonsterData->pMonStatsTxt) ||
        !npc->pMonsterData->pMonStatsTxt->dwFlags.bNPC ||
        !npc->pMonsterData->pMonStatsTxt->dwFlags.bInteract)
        return Failure("field merchant creation uncertain; inspect room before retry");
    return {{"ok", true}, {"status", "merchant_spawned"}, {"unit_id", npc->dwUnitId},
        {"class_id", classId}, {"level_id", level}, {"x", x}, {"y", y},
        {"unit_flags", npc->dwFlags},
        {"selectable", bool(npc->dwFlags & UNITFLAG_SELECTABLE)}};
}
void Clear() {
    std::lock_guard<std::mutex> lock(gManagedMutex);
    gManagedCount = 0;
    gManagedGame = nullptr;
    gNaturalGame.store(nullptr, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(gNaturalSamplesMutex);
        gNaturalSampleCount.store(0, std::memory_order_release);
    }
    for (auto& areaTargets : gNaturalTargets)
        for (auto& selected : areaTargets)
            selected.store(0, std::memory_order_release);
}
}
