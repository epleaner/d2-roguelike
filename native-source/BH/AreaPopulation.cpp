#define NOMINMAX
#include "AreaPopulation.h"
#include "Constants.h"
#include "D2Ptrs.h"
#include "LabProfile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
using json = nlohmann::json;

// D2GAME.dll S13 SHA-256 24b293b86f95bea0228826c502ea04737ba21b91ca9832e52aac4b11fef14f53.
// 0x1E960 indexes game+0xD8+4*act, allocates 0x18-byte coordinate lists;
// 0x1F6A5 allocates 0x5C-byte monster records, 0x1F9D6 links them at list+0x0C.
#pragma pack(push, 1)
struct InactiveMonster {
    int32_t x, y, classId, unitId;
    uint32_t unitFlags, unitFlagsEx, typeFlags;
    int32_t aiOwner;
    void* minions;
    void* mapAi;
    int32_t aiState, originLevel;
    uint16_t nameSeed;
    uint8_t modifiers[9], reserved;
    uint16_t bossHcIndex, reserved2;
    int32_t experience, maxHp, hp, command1, command2, frame;
    InactiveMonster* next;
};
struct InactiveRoom {
    int32_t x, y;
    void* items;
    InactiveMonster* monsters;
    void* otherUnits;
    InactiveRoom* next;
};
#pragma pack(pop)
static_assert(sizeof(void*) == 4, "S13 inactive records require x86");
static_assert(sizeof(InactiveMonster) == 0x5C && offsetof(InactiveMonster, typeFlags) == 0x18 &&
    offsetof(InactiveMonster, maxHp) == 0x44 && offsetof(InactiveMonster, hp) == 0x48 &&
    offsetof(InactiveMonster, frame) == 0x54 && offsetof(InactiveMonster, next) == 0x58,
    "S13 compressed monster layout");
static_assert(sizeof(InactiveRoom) == 0x18 && offsetof(InactiveRoom, monsters) == 0x0C,
    "S13 inactive room layout");

bool Readable(const void* pointer, size_t bytes) {
    if (!pointer || !bytes) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
    if (bytes > std::numeric_limits<uintptr_t>::max() - begin) return false;
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(pointer, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    return end >= begin && bytes <= end - begin;
}
template <typename T> bool Valid(const T* value) { return Readable(value, sizeof(T)); }

template <size_t N> bool CodeShape(HMODULE module, DWORD rva, const BYTE (&bytes)[N]) {
    const BYTE* address = reinterpret_cast<const BYTE*>(module) + rva;
    MEMORY_BASIC_INFORMATION region = {};
    return Readable(address, N) && VirtualQuery(address, &region, sizeof(region)) &&
        region.AllocationBase == module &&
        (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY)) && !memcmp(address, bytes, N);
}
bool Bind(std::string& error) {
    if (!LabProfile::VerifyServer(error)) return false;
    HMODULE module = GetModuleHandleA("D2GAME.dll");
    if (!module) { error = "pinned S13 server image unavailable"; return false; }
    static const BYTE lists[] = {0x8B,0x4C,0x24,0x08,0x55,0x8B,0x6C,0x24,0x08,0x56,
        0x8B,0xB4,0x8D,0xD8,0,0,0,0x57,0x33,0xC0,0x33,0xFF,0x85,0xF6};
    BYTE allocation[] = {0x8B,0x4B,0x1C,0x6A,0,0x68,0x29,0x05,0,0,
        0x68,0,0,0,0,0xBA,0x5C,0,0,0,0xE8,0x28,0xAC,0xFE,0xFF};
    // PE image base 0x6FC20000: absolute 0x6FD1CEB0 is relocated at load.
    const uint32_t sourcePointer = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(module) + 0xFCEB0);
    memcpy(allocation + 11, &sourcePointer, sizeof(sourcePointer));
    static const BYTE alignment[] = {0x56,0xE8,0xD0,0xAC,0xFE,0xFF,0x83,0xF8,0x02,
        0x75,0x0A,0x8B,0x45,0x18,0x0D,0,0x01,0,0,0xEB,0x1A,0x85,0xC0,
        0x75,0x0A,0x8B,0x45,0x18,0x0D,0,0x02,0,0};
    static const BYTE life[] = {0x8B,0x46,0x10,0x83,0xF8,0x0C,0x74,0x04,0x85,0xC0,
        0x75,0x04,0x83,0x4D,0x18,0x04,0x8B,0x06,0x3B,0xC3};
    static const BYTE hp[] = {0x8B,0x4E,0x0C,0x89,0x4D,0x0C,0x8B,0x96,0xC4,0,0,
        0,0x89,0x55,0x10,0x8B,0x86,0xC8,0,0,0,0x8B,0x74,0x24,0x10};
    static const BYTE link[] = {0x8B,0x54,0x24,0x18,0x53,0x8B,0x5C,0x24,0x18,0x0F,
        0xB6,0xC0,0x52,0x50,0x8B,0x44,0x24,0x44,0x50,0xE8,0x85,0xEF,0xFF,0xFF,
        0x8B,0x48,0x0C,0x89,0x4D,0x58,0x5F,0x89,0x68,0x0C};
    static const BYTE restoreHp[] = {0x8B,0x96,0xA8,0,0,0,0x2B,0x55,0x54,
        0x81,0xFA,0x4C,0x1D,0,0,0x6A,0,0x72,0x14,0x8B,0x45,0x44,
        0x50,0x6A,0x07,0x57};
    if (!CodeShape(module, 0x1E960, lists) || !CodeShape(module, 0x1F6A5, allocation) ||
        !CodeShape(module, 0x1F83E, alignment) || !CodeShape(module, 0x1F923, life) ||
        !CodeShape(module, 0x1F990, hp) || !CodeShape(module, 0x1F9C3, link) ||
        !CodeShape(module, 0x1FEF4, restoreHp)) {
        error = "unexpected S13 inactive monster writer or list layout";
        return false;
    }
    return true;
}

struct RoomOrigin { int32_t x, y; Room2* metadata; };
constexpr size_t kRoomLimit = 256, kListLimit = 2048, kMonsterLimit = 8192, kEnemyLimit = 1024;
template <typename T, size_t N> bool InsertSeen(std::array<const T*, N>& slots, const T* pointer) {
    static_assert((N & (N - 1)) == 0, "visited table must be a power of two");
    size_t index = ((reinterpret_cast<uintptr_t>(pointer) >> 4) * 2654435761u) & (N - 1);
    while (slots[index]) {
        if (slots[index] == pointer) return false;
        index = (index + 1) & (N - 1);
    }
    slots[index] = pointer;
    return true;
}
} // namespace

namespace AreaPopulation {
bool AppendInactive(void* game, UnitAny* player, json& census, std::string& error) {
    census["inactive_enemies"] = json::array();
    census["inactive_hostile_alive_count"] = 0;
    census["population_complete"] = false;
    auto fail = [&](const char* why, bool truncated = false) {
        error = why;
        census["population_error"] = error;
        if (truncated) census["truncated"] = true;
        return false;
    };
    if (!Bind(error)) {
        census["population_error"] = error;
        return false;
    }
    if (!Readable(game, 0xEC) || !Valid(player) || player->dwType != UNIT_PLAYER ||
        !Valid(player->pPath) || !Valid(player->pPath->pRoom1) || !Valid(player->pAct) ||
        player->dwAct >= 5 || player->pAct->dwAct != player->dwAct ||
        !Valid(player->pPath->pRoom1->pRoom2) ||
        !Valid(player->pPath->pRoom1->pRoom2->pLevel))
        return fail("inactive population game/player context unavailable");
    const BYTE* base = reinterpret_cast<const BYTE*>(game);
    const DWORD act = player->dwAct;
    Level* level = player->pPath->pRoom1->pRoom2->pLevel;
    Room1* const initialRoom = player->pPath->pRoom1;
    if (!Valid(level) || !level->dwLevelNo ||
        !census.contains("level_id") || census["level_id"] != level->dwLevelNo)
        return fail("inactive population level identity changed");
    const auto* heads = reinterpret_cast<InactiveRoom* const*>(base + 0xD8);
    const DWORD frame = *reinterpret_cast<const DWORD*>(base + 0xA8);
    std::array<RoomOrigin, kRoomLimit> origins{};
    size_t roomCount = 0;
    for (Room2* room = level->pRoom2First; room;) {
        if (roomCount == kRoomLimit) return fail("inactive population room catalog exceeds bound", true);
        if (!Valid(room) || room->pLevel != level || !room->dwSizeX || !room->dwSizeY ||
            room->dwPosX > INT32_MAX / 5 || room->dwPosY > INT32_MAX / 5 ||
            room->dwSizeX > INT32_MAX / 5 || room->dwSizeY > INT32_MAX / 5)
            return fail("inactive population room metadata invalid");
        const int32_t x = static_cast<int32_t>(room->dwPosX * 5);
        const int32_t y = static_cast<int32_t>(room->dwPosY * 5);
        for (size_t i = 0; i < roomCount; ++i)
            if (origins[i].metadata == room || (origins[i].x == x && origins[i].y == y))
                return fail("inactive population room catalog cyclic or ambiguous");
        origins[roomCount++] = {x, y, room};
        room = room->pRoom2Next;
    }
    if (!roomCount) return fail("inactive population room catalog empty");

    sgptDataTable* const* global = p_D2COMMON_sgptDataTable;
    if (!Valid(global) || !Valid(*global)) return fail("inactive population monster table unavailable");
    const sgptDataTable* table = *global;
    if (!table->dwMonStatsRecs || table->dwMonStatsRecs > 32768 ||
        !Readable(table->pMonStatsTxt, size_t(table->dwMonStatsRecs) * sizeof(MonStatsTxt)))
        return fail("inactive population monster class rows unavailable");

    std::array<const InactiveRoom*, 4096> seenLists{};
    std::array<const InactiveMonster*, 16384> seenMonsters{};
    std::array<bool, kRoomLimit> matchedRooms{};
    size_t listCount = 0, monsterCount = 0;
    const InactiveRoom* initialHead = heads[act];
    for (const InactiveRoom* list = initialHead; list;) {
        if (listCount == kListLimit) return fail("inactive population room lists exceed bound", true);
        if (!Valid(list)) return fail("inactive population room list unreadable");
        if (!InsertSeen(seenLists, list)) return fail("inactive population room lists cyclic");
        ++listCount;
        const Room2* matching = nullptr;
        for (size_t i = 0; i < roomCount; ++i)
            if (list->x == origins[i].x && list->y == origins[i].y) {
                if (matchedRooms[i]) return fail("inactive population duplicate room list");
                matchedRooms[i] = true;
                matching = origins[i].metadata;
                break;
            }
        if (matching) {
            // S13 restores and removes this list when its room becomes active. A
            // simultaneous active room/list pair cannot be certified as disjoint.
            if (matching->pRoom1) return fail("inactive population overlaps active room");
            for (const InactiveMonster* monster = list->monsters; monster;) {
                if (monsterCount == kMonsterLimit)
                    return fail("inactive population monster records exceed bound", true);
                if (!Valid(monster)) return fail("inactive population monster record unreadable");
                if (!InsertSeen(seenMonsters, monster))
                    return fail("inactive population monster records cyclic or shared");
                ++monsterCount;
                const DWORD flags = monster->typeFlags;
                if (monster->classId < 0 || static_cast<DWORD>(monster->classId) >= table->dwMonStatsRecs)
                    return fail("inactive population monster class out of range");
                const MonStatsTxt& klass = table->pMonStatsTxt[monster->classId];
                // The writer stores fixed-point 8-bit life, a death-mode flag,
                // and alignment. Restore treats absent alignment flags as evil.
                const DWORD alignment = flags & 0x380;
                if (alignment && alignment != 0x80 && alignment != 0x100 && alignment != 0x200)
                    return fail("inactive population alignment flags inconsistent");
                if (klass.dwFlags.bKillable && !klass.dwFlags.bNPC && !klass.dwFlags.binTown &&
                    alignment != 0x80 && alignment != 0x100 && !(flags & 0x4)) {
                    if (monster->unitFlagsEx & 0x100)
                        return fail("inactive population has deferred live server unit");
                    if (monster->maxHp <= 0 || monster->hp <= 0 || monster->hp > monster->maxHp)
                        return fail("inactive population live monster life inconsistent");
                    if (census["inactive_enemies"].size() == kEnemyLimit)
                        return fail("inactive population hostile records exceed bound", true);
                    const bool healed = frame - static_cast<DWORD>(monster->frame) >= 7500;
                    const int32_t hp = healed ? monster->maxHp : monster->hp;
                    const char* rank = flags & 0x800 ? "superunique" : flags & 0x2 ? "champion" :
                        flags & 0x1 ? "unique" : flags & 0x20 ? "minion" : "normal";
                    census["inactive_enemies"].push_back({
                        {"source", "server_inactive_room"}, {"unit_id", monster->unitId},
                        {"class_id", monster->classId}, {"origin_level_id", monster->originLevel},
                        {"position", {{"x", monster->x}, {"y", monster->y}}},
                        {"room", {{"x", list->x}, {"y", list->y},
                            {"width", matching->dwSizeX * 5}, {"height", matching->dwSizeY * 5}}},
                        {"hp", hp / 256.0}, {"max_hp", monster->maxHp / 256.0},
                        {"stored_hp", monster->hp / 256.0}, {"healed_on_restore", healed},
                        {"alignment", "evil"}, {"rank", rank},
                        {"name_seed", monster->nameSeed}, {"boss_hc_index", monster->bossHcIndex}});
                    census["inactive_hostile_alive_count"] = census["inactive_enemies"].size();
                }
                monster = monster->next;
            }
        }
        list = list->next;
    }
    if (!Valid(player) || player->dwAct != act || !Valid(player->pPath) ||
        player->pPath->pRoom1 != initialRoom || !Valid(initialRoom) ||
        !Valid(initialRoom->pRoom2) || initialRoom->pRoom2->pLevel != level ||
        *reinterpret_cast<const DWORD*>(base + 0xA8) != frame || heads[act] != initialHead)
        return fail("inactive population context changed during traversal");
    census["population_complete"] = true;
    return true;
}
} // namespace AreaPopulation
