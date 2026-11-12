#define NOMINMAX
#include "GameplayActions.h"

#include "Constants.h"
#include "D2Helpers.h"
#include "D2Ptrs.h"
#include "CharacterState.h"

#include <cstdint>
#include <limits>
#include <algorithm>
#include <cstddef>

namespace {
using json = nlohmann::json;

struct PendingNpc {
    UnitAny* player = nullptr;
    DWORD id = 0;
    DWORD classId = 0;
    ULONGLONG deadline = 0;
};
PendingNpc pendingNpc;
const char* npcError = nullptr;

bool Readable(const void* pointer, size_t size) {
    if (!pointer || !size) return false;
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(pointer, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
        return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    return start <= end && size <= end - start;
}

json Error(const char* error) { return {{"ok", false}, {"error", error}}; }
json Requested() { return {{"ok", true}, {"status", "requested"}}; }

bool EmptyObject(const json& args) { return args.is_object() && args.empty(); }

bool UnsignedField(const json& args, const char* key, uint64_t min, uint64_t max, DWORD& result) {
    if (!args.is_object() || !args.contains(key)) return false;
    const json& value = args.at(key);
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) {
        const uint64_t n = value.get<uint64_t>();
        if (n < min || n > max) return false;
        result = static_cast<DWORD>(n);
        return true;
    }
    const int64_t n = value.get<int64_t>();
    if (n < 0 || static_cast<uint64_t>(n) < min || static_cast<uint64_t>(n) > max) return false;
    result = static_cast<DWORD>(n);
    return true;
}

UnitAny* Player() {
    if (!IsGameReady()) return nullptr;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    return player && player->dwType == UNIT_PLAYER ? player : nullptr;
}

UnitAny* FindMonster(DWORD id, UnitAny* player) {
    UnitAny* unit = D2CLIENT_FindServerSideUnit(id, UNIT_MONSTER);
    if (!unit || unit->dwType != UNIT_MONSTER || unit->dwUnitId != id ||
        !unit->pPath || !unit->pPath->pRoom1 || !unit->pPath->pRoom1->pRoom2 ||
        !unit->pPath->pRoom1->pRoom2->pLevel ||
        unit->pPath->pRoom1->pRoom2->pLevel->dwLevelNo !=
            player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo ||
        !unit->pMonsterData || !unit->pMonsterData->pMonStatsTxt) return nullptr;
    return unit;
}

void UnitPacket(BYTE opcode, DWORD id, DWORD type = UNIT_MONSTER, DWORD flag = 0) {
    BYTE packet[9] = {opcode};
    packet[1] = static_cast<BYTE>(type);
    packet[2] = static_cast<BYTE>(type >> 8);
    packet[3] = static_cast<BYTE>(type >> 16);
    packet[4] = static_cast<BYTE>(type >> 24);
    packet[5] = static_cast<BYTE>(id);
    packet[6] = static_cast<BYTE>(id >> 8);
    packet[7] = static_cast<BYTE>(id >> 16);
    packet[8] = static_cast<BYTE>(id >> 24);
    D2NET_SendPacket(sizeof(packet), flag, packet);
}

bool SelectSkill(WORD skill, bool left) {
    // The same 0x3C packet and hand bit used by D2Helpers::SetSkill.
    BYTE packet[9] = {0x3C, static_cast<BYTE>(skill), static_cast<BYTE>(skill >> 8),
                      0, static_cast<BYTE>(left ? 0x80 : 0), 0xFF, 0xFF, 0xFF, 0xFF};
    return D2NET_SendPacket(sizeof(packet), 0, packet) != 0;
}

json NpcStateOnThread(UnitAny* player) {
    UnitAny* npc = D2CLIENT_GetCurrentInteractingNPC();
    const bool menuOpen = D2CLIENT_GetUIState(UI_NPCMENU) != 0;
    const bool shopOpen = D2CLIENT_GetUIState(UI_NPCSHOP) != 0;
    json result = {{"ok", true}, {"menu_open", menuOpen},
                   {"shop_open", shopOpen}, {"current_npc", nullptr}};
    result["pending_interaction"] = pendingNpc.player ?
        json{{"unit_id", pendingNpc.id}, {"stage", "approaching"}} : json(nullptr);
    result["interaction_error"] = npcError ? json(npcError) : json(nullptr);
    if (npc && npc->dwType == UNIT_MONSTER && npc->pPath && npc->pMonsterData &&
        npc->pMonsterData->pMonStatsTxt && npc->pMonsterData->pMonStatsTxt->dwFlags.bNPC &&
        npc->pPath->pRoom1 && npc->pPath->pRoom1->pRoom2 &&
        npc->pPath->pRoom1->pRoom2->pLevel &&
        npc->pPath->pRoom1->pRoom2->pLevel->dwLevelNo ==
            player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo) {
        result["current_npc"] = {{"unit_id", npc->dwUnitId}, {"class_id", npc->dwTxtFileNo},
                                 {"x", D2CLIENT_GetUnitX(npc)}, {"y", D2CLIENT_GetUnitY(npc)}};
    }
    return result;
}
} // namespace

namespace GameplayActions {
json Move(const json& args) {
    DWORD x, y;
    if (!UnsignedField(args, "x", 1, 65535, x) || !UnsignedField(args, "y", 1, 65535, y))
        return Error("x and y must be positive 16-bit world tile coordinates");
    bool run = true;
    if (args.contains("mode")) {
        if (!args["mode"].is_string()) return Error("mode must be run or walk");
        if (args["mode"] != "run" && args["mode"] != "walk")
            return Error("mode must be run or walk");
        run = args["mode"] == "run";
    }
    if (!Player()) return Error("game is not ready");
    // Server-authoritative world movement avoids client click prediction and
    // hover/viewport state. The offline server owns movement and collision.
    BYTE packet[5] = {static_cast<BYTE>(run ? 0x03 : 0x01),
        static_cast<BYTE>(x), static_cast<BYTE>(x >> 8),
        static_cast<BYTE>(y), static_cast<BYTE>(y >> 8)};
    if (!D2NET_SendPacket(sizeof(packet), 0, packet))
        return Error("native movement packet was not accepted");
    json result = Requested();
    result["x"] = x;
    result["y"] = y;
    result["mode"] = run ? "run" : "walk";
    return result;
}

json Approach(const json& args) {
    DWORD id, type;
    if (!UnsignedField(args, "unit_id", 0, std::numeric_limits<DWORD>::max(), id) ||
        !UnsignedField(args, "unit_type", UNIT_MONSTER, UNIT_OBJECT, type))
        return Error("unit_id and unit_type (monster/NPC=1, object=2) are required");
    bool run = true;
    if (args.contains("mode")) {
        if (!args["mode"].is_string() || (args["mode"] != "run" && args["mode"] != "walk"))
            return Error("mode must be run or walk");
        run = args["mode"] == "run";
    }
    UnitAny* player = Player();
    if (!player) return Error("game is not ready");
    UnitAny* target = D2CLIENT_FindServerSideUnit(id, type);
    if (!target || target->dwType != type || target->dwUnitId != id)
        return Error("target unit not found");
    Room1* room = D2COMMON_GetRoomFromUnit(target);
    if (!room || !room->pRoom2 || !room->pRoom2->pLevel ||
        room->pRoom2->pLevel->dwLevelNo != player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo ||
        (type == UNIT_MONSTER && (target->dwMode == NPC_MODE_DEATH || target->dwMode == NPC_MODE_DEAD)))
        return Error("target must be alive and in the current area");
    const int dx = D2CLIENT_GetUnitX(target) - player->pPath->xPos;
    const int dy = D2CLIENT_GetUnitY(target) - player->pPath->yPos;
    if (dx < -30 || dx > 30 || dy < -30 || dy > 30)
        return Error("target is more than thirty tiles away; navigate nearer first");
    // Native 0x02/0x04 follows the unit, rather than a stale host-side waypoint.
    UnitPacket(run ? 0x04 : 0x02, id, type);
    json result = Requested();
    result["unit_id"] = id;
    result["unit_type"] = type;
    result["mode"] = run ? "run" : "walk";
    return result;
}

json Attack(const json& args) {
    DWORD id;
    if (!UnsignedField(args, "unit_id", 0, std::numeric_limits<DWORD>::max(), id))
        return Error("unit_id must be a uint32 monster unit id");
    DWORD skill = 0;
    const bool explicitSkill = args.contains("skill_id");
    if (explicitSkill && !UnsignedField(args, "skill_id", 0, 65535, skill))
        return Error("skill_id must be a uint16 skill id");
    bool left = true;
    if (args.contains("hand")) {
        if (!args["hand"].is_string()) return Error("hand must be left or right");
        if (args["hand"] != "left" && args["hand"] != "right")
            return Error("hand must be left or right");
        left = args["hand"] == "left";
    }
    if (!left && (!explicitSkill || !skill))
        return Error("right-hand attacks require an owned, nonzero skill_id");
    UnitAny* player = Player();
    if (!player) return Error("game is not ready");
    UnitAny* target = FindMonster(id, player);
    if (!target) return Error("monster unit not found in the current area");
    const MonStatsTxt* txt = target->pMonsterData->pMonStatsTxt;
    if (IsTown(player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo) ||
        target->dwMode == NPC_MODE_DEATH || target->dwMode == NPC_MODE_DEAD ||
        !txt->dwFlags.bKillable || txt->dwFlags.bNPC || txt->dwFlags.binTown ||
        D2COMMON_GetUnitStat(target, STAT_ALIGNMENT, 0) != 0 ||
        (target->dwOwnerId != 0 && target->dwOwnerId != 0xFFFFFFFF &&
         (target->dwOwnerType == UNIT_PLAYER || target->dwOwnerType == UNIT_MONSTER)))
        return Error("target is dead, friendly, neutral, or not a killable enemy");
    // Exact S13 packet parser CD630 -> CCFE0 -> CB690 rejects targets
    // beyond 50 world tiles on either axis before dispatching an attack.
    const int dx = int(player->pPath->xPos) - int(target->pPath->xPos);
    const int dy = int(player->pPath->yPos) - int(target->pPath->yPos);
    if (dx < -50 || dx > 50 || dy < -50 || dy > 50)
        return Error("target is outside the native unit-packet range; approach first");
    if (!player->pInfo) return Error("player skills are unavailable");
    if (skill) {
        bool owned = false;
        for (Skill* known = player->pInfo->pFirstSkill; known; known = known->pNextSkill) {
            if (known->pSkillInfo && known->pSkillInfo->wSkillId == skill) {
                owned = true;
                break;
            }
        }
        if (!owned) return Error("skill_id is not available to the player");
    }
    // One skill selection and one targeted unit action; no sleeps, retries, or cursor state.
    // 0x06 and 0x0D act on the unit ID rather than its current (possibly moving) tile.
    SelectSkill(static_cast<WORD>(skill), left);
    UnitPacket(left ? 0x06 : 0x0D, id);
    json result = Requested();
    result["unit_id"] = id;
    result["skill_id"] = skill;
    result["hand"] = left ? "left" : "right";
    return result;
}

json Cast(const json& args) {
    DWORD skillId;
    if (!UnsignedField(args, "skill_id", 1, 65535, skillId))
        return Error("skill_id must be a nonzero uint16");
    if (!args.contains("hand") || !args["hand"].is_string() ||
        (args["hand"] != "left" && args["hand"] != "right"))
        return Error("hand must be left or right");
    const bool left = args["hand"] == "left";
    const bool unitCast = args.contains("unit_id");
    const bool coordinateCast = args.contains("x") || args.contains("y");
    if (unitCast == coordinateCast || args.size() != (unitCast ? 3 : 4))
        return Error("provide exactly unit_id or both x and y");
    DWORD x = 0, y = 0, id = 0;
    if (unitCast) {
        if (!UnsignedField(args, "unit_id", 0, std::numeric_limits<DWORD>::max(), id))
            return Error("unit_id must be a uint32 monster unit id");
    } else if (!UnsignedField(args, "x", 1, 65535, x) ||
               !UnsignedField(args, "y", 1, 65535, y))
        return Error("x and y must be uint16 world coordinates");

    UnitAny* player = Player();
    if (!Readable(player, sizeof(UnitAny)) || !Readable(player->pPath, sizeof(Path)) ||
        !Readable(player->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(player->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(player->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
        !Readable(player->pInfo, sizeof(Info))) return Error("player area or skills unavailable");
    if (!CharacterState::BindingsReady()) return Error("S13 casting bindings changed");
    const DWORD area = player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
    if (player->dwMode == PLAYER_MODE_DEATH || player->dwMode == PLAYER_MODE_DEAD)
        return Error("player cannot cast while dead");
    UnitAny* target = nullptr;
    if (unitCast) {
        target = D2CLIENT_FindServerSideUnit(id, UNIT_MONSTER);
        if (!Readable(target, sizeof(UnitAny)) || target->dwType != UNIT_MONSTER ||
            target->dwUnitId != id || !Readable(target->pPath, sizeof(Path)) ||
            !Readable(target->pPath->pRoom1, sizeof(Room1)) ||
            !Readable(target->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
            !Readable(target->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
            target->pPath->pRoom1->pRoom2->pLevel->dwLevelNo != area ||
            !Readable(target->pMonsterData, sizeof(MonsterData)) ||
            !Readable(target->pMonsterData->pMonStatsTxt, sizeof(MonStatsTxt)))
            return Error("monster unit not found in the current area");
        if (target->dwMode == NPC_MODE_DEATH || target->dwMode == NPC_MODE_DEAD)
            return Error("living monster unit not found in the current area");
        const MonStatsTxt* txt = target->pMonsterData->pMonStatsTxt;
        if (!txt->dwFlags.bKillable || txt->dwFlags.bNPC || txt->dwFlags.binTown ||
            D2COMMON_GetUnitStat(target, STAT_ALIGNMENT, 0) != 0)
            return Error("target is friendly, neutral, or not a killable enemy");
        x = target->pPath->xPos;
        y = target->pPath->yPos;
    }
    const int dx = int(player->pPath->xPos) - int(x);
    const int dy = int(player->pPath->yPos) - int(y);
    // D2Game S13 CBF50/CD630 -> CB690 reject either axis beyond 50 tiles.
    if (dx < -50 || dx > 50 || dy < -50 || dy > 50)
        return Error("cast target exceeds the native 50-tile packet range");
    if (!unitCast) {
        // Restrict remote coordinates to loaded rooms belonging to this area.
        Room1* room = player->pPath->pRoom1;
        bool found = false;
        const DWORD neighbors = room->dwRoomsNear;
        if (neighbors > 64 || (neighbors &&
            !Readable(room->pRoomsNear, neighbors * sizeof(Room1*))))
            return Error("nearby room list is invalid");
        for (DWORD index = 0; index <= neighbors; ++index) {
            Room1* candidate = index ? room->pRoomsNear[index - 1] : room;
            if (!Readable(candidate, sizeof(Room1)) ||
                !Readable(candidate->pRoom2, sizeof(Room2)) ||
                !Readable(candidate->pRoom2->pLevel, sizeof(Level)) ||
                candidate->pRoom2->pLevel->dwLevelNo != area) continue;
            if (x >= candidate->dwXStart && y >= candidate->dwYStart &&
                x - candidate->dwXStart < candidate->dwXSize &&
                y - candidate->dwYStart < candidate->dwYSize) { found = true; break; }
        }
        if (!found) return Error("coordinate is not in a loaded room of the current area");
    }
    Skill* known = player->pInfo->pFirstSkill;
    for (unsigned visited = 0; known && visited < 4096; ++visited) {
        if (!Readable(known, sizeof(Skill)) || !Readable(known->pSkillInfo, sizeof(SkillsTxt)))
            return Error("player skill chain is invalid");
        if (known->pSkillInfo->wSkillId == skillId) break;
        known = known->pNextSkill;
    }
    if (!known || !known->pSkillInfo || known->pSkillInfo->wSkillId != skillId ||
        known->itemGUID != -1)
        return Error("skill is not an owned player skill (item-granted casts are unsupported)");
    const SkillsTxt* skill = known->pSkillInfo;
    if (skill->dwFlags.bPassive || skill->dwFlags.bAura)
        return Error("passive and aura skills cannot be coordinate-cast");
    if (IsTown(area) && !skill->dwFlags.bInTown)
        return Error("skill cannot be used in town");
    const int level = D2COMMON_GetSkillLevel(player, known, TRUE);
    if (level <= 0 || level > 255) return Error("skill is not ready at an effective level");
    if (D2COMMON_GetUnitState(player, 121)) // S13 state SKILLDELAY.
        return Error("player is in a skill cooldown state");
    const unsigned shift = skill->wManaShift & 0xFF;
    if (shift > 16) return Error("skill mana shift outside native range");
    const int64_t scaled = (int64_t(static_cast<int16_t>(skill->wMana)) +
                            int64_t(level - 1) * static_cast<int16_t>(skill->wLevelMana)) *
                           (int64_t(1) << shift);
    const int64_t cost = std::max<int64_t>(int64_t(skill->wMinMana) << 8, scaled);
    if (cost < 0 || cost > std::numeric_limits<int>::max())
        return Error("skill mana cost is outside native range");
    const int mana = static_cast<int>(D2COMMON_GetUnitStat(player, STAT_MANA, 0));
    if (mana < cost) return Error("insufficient mana for the requested skill");

    // S13 D2Game dispatcher CC56B indexes opcode * 8 at FA078:
    // 05 -> CE5A0 and 0C -> CE520: one-shot 5-byte x/y casts via CBF50.
    // 06 and 0D -> CD630 (9-byte type/id). No approach or retry.
    if (!SelectSkill(static_cast<WORD>(skillId), left))
        return Error("native skill selection packet was not accepted");
    bool sent = false;
    if (unitCast) {
        BYTE packet[9] = {static_cast<BYTE>(left ? 0x06 : 0x0D), UNIT_MONSTER, 0, 0, 0,
                          static_cast<BYTE>(id), static_cast<BYTE>(id >> 8),
                          static_cast<BYTE>(id >> 16), static_cast<BYTE>(id >> 24)};
        sent = D2NET_SendPacket(sizeof(packet), 0, packet) != 0;
    } else {
        BYTE packet[5] = {static_cast<BYTE>(left ? 0x05 : 0x0C),
                          static_cast<BYTE>(x), static_cast<BYTE>(x >> 8),
                          static_cast<BYTE>(y), static_cast<BYTE>(y >> 8)};
        sent = D2NET_SendPacket(sizeof(packet), 0, packet) != 0;
    }
    if (!sent) return Error("native cast packet was not accepted");
    json result = Requested();
    result["skill_id"] = skillId;
    result["hand"] = left ? "left" : "right";
    result["target"] = unitCast ? json{{"unit_id", id}} : json{{"x", x}, {"y", y}};
    result["precast"] = {{"mana", mana / 256.0}, {"mana_cost", cost / 256.0},
                         {"effective_skill_level", level}, {"skill_delay_active", false},
                         {"player_mode", player->dwMode}, {"area_id", area}};
    return result;
}

void ResetNpc() {
    pendingNpc = {};
    npcError = nullptr;
}

void TickNpc() {
    if (!pendingNpc.player) return;
    if (GetTickCount64() >= pendingNpc.deadline) {
        pendingNpc = {};
        npcError = "Native NPC approach timed out";
        return;
    }
    UnitAny* player = Player();
    if (!player || player != pendingNpc.player ||
        player->dwMode == PLAYER_MODE_DEATH || player->dwMode == PLAYER_MODE_DEAD) {
        pendingNpc = {};
        npcError = "NPC approach cancelled because the player changed or died";
        return;
    }
    UnitAny* npc = FindMonster(pendingNpc.id, player);
    if (!npc || npc->dwTxtFileNo != pendingNpc.classId ||
        !npc->pMonsterData->pMonStatsTxt->dwFlags.bNPC ||
        !npc->pMonsterData->pMonStatsTxt->dwFlags.bInteract) {
        pendingNpc = {};
        npcError = "NPC target is no longer available";
        return;
    }
    if (D2CLIENT_GetUIState(UI_NPCMENU) || D2CLIENT_GetUIState(UI_NPCSHOP)) {
        pendingNpc = {};
        return;
    }
    if (D2COMMON_GetUnitDistance(player, npc) < 9) {
        // Observe range and emit interaction in the SAME engine frame.
        UnitPacket(0x13, pendingNpc.id, UNIT_MONSTER, 1);
        pendingNpc = {};
    }
}

json InteractNpc(const json& args) {
    DWORD id;
    if (!UnsignedField(args, "unit_id", 0, std::numeric_limits<DWORD>::max(), id))
        return Error("unit_id must be a uint32 NPC unit id");
    UnitAny* player = Player();
    if (!player) return Error("game is not ready");
    UnitAny* npc = FindMonster(id, player);
    if (!npc) return Error("NPC unit not found in the current area");
    const MonStatsTxt* txt = npc->pMonsterData->pMonStatsTxt;
    if (!txt->dwFlags.bNPC || !txt->dwFlags.bInteract ||
        npc->dwMode == NPC_MODE_DEATH || npc->dwMode == NPC_MODE_DEAD)
        return Error("target is not a live, interactable NPC");
    if (pendingNpc.player || D2CLIENT_GetCurrentInteractingNPC() ||
        D2CLIENT_GetUIState(UI_NPCMENU) || D2CLIENT_GetUIState(UI_NPCSHOP))
        return Error("an NPC interaction is already open or pending");
    const int dx = D2CLIENT_GetUnitX(npc) - player->pPath->xPos;
    const int dy = D2CLIENT_GetUnitY(npc) - player->pPath->yPos;
    if (dx < -30 || dx > 30 || dy < -30 || dy > 30)
        return Error("NPC is more than thirty tiles away; navigate nearer first");
    npcError = nullptr;
    pendingNpc = {player, id, npc->dwTxtFileNo, GetTickCount64() + 15000};
    if (D2COMMON_GetUnitDistance(player, npc) >= 9)
        UnitPacket(0x04, id, UNIT_MONSTER);
    TickNpc();
    json result = Requested();
    result["unit_id"] = id;
    return result;
}

json CloseInteraction(const json& args) {
    if (!EmptyObject(args)) return Error("close_interaction takes an empty object");
    UnitAny* player = Player();
    if (!player) return Error("game is not ready");
    if (pendingNpc.player) {
        ResetNpc();
        return Requested();
    }
    const json state = NpcStateOnThread(player);
    if (state["current_npc"].is_null() && !state["menu_open"].get<bool>() &&
        !state["shop_open"].get<bool>())
        return Error("no NPC interaction is open");
    // Closing a menu/speech alone does not close an opened shop. The native
    // shop-specific panel callback performs interaction and inventory cleanup.
    if (D2CLIENT_GetUIState(UI_NPCSHOP))
        D2CLIENT_SetUIVar(UI_NPCSHOP, 1, 0);
    else
        D2CLIENT_CloseNPCInteract();
    return Requested();
}

json NpcState(const json& args) {
    if (!EmptyObject(args)) return Error("npc_state takes an empty object");
    UnitAny* player = Player();
    if (!player) return Error("game is not ready");
    return NpcStateOnThread(player);
}
} // namespace GameplayActions
