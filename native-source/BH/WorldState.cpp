#define NOMINMAX
#include "WorldState.h"
#include "D2Ptrs.h"
#include "Constants.h"
#include "D2Helpers.h"
#include "InventoryActions.h"
#include "SetItems.h"
#include "ItemCrafting.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace {
using json = nlohmann::json;
static_assert(sizeof(void*) == 4 && sizeof(ItemsTxt) == 0x1A8 &&
    offsetof(ItemsTxt, wreqstr) == 0x10A && offsetof(ItemsTxt, wreqdex) == 0x10C &&
    offsetof(ItemsTxt, nType) == 0x11E && offsetof(ItemsTxt, wtype2) == 0x120 &&
    offsetof(ItemsTxt, blevelreq) == 0x13F, "S13 item requirements ABI");
static_assert(sizeof(D2ItemTypesTxt) == 0xE4 &&
    offsetof(D2ItemTypesTxt, nBody) == 0x09 &&
    offsetof(D2ItemTypesTxt, nBodyLoc1) == 0x0A &&
    offsetof(D2ItemTypesTxt, nBodyLoc2) == 0x0B &&
    offsetof(D2ItemTypesTxt, nClass) == 0x21, "S13 item type equipment ABI");

// A readable region check is necessary but not sufficient: all game structures and
// binding addresses still require validation against the exact deployed binary.
bool Readable(const void* p, size_t bytes) {
    if (!p || !bytes) return false;
    const auto start = reinterpret_cast<uintptr_t>(p);
    if (start > std::numeric_limits<uintptr_t>::max() - bytes) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const auto end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= start && bytes <= end - start;
}
template <typename T> bool Valid(const T* p) { return Readable(p, sizeof(T)); }
json Failure(const char* error) { return {{"ok", false}, {"error", error}}; }
// Bounded, stats-only diagnostics for a mismatched loaded S13 table. Never
// reinterpret unverified rows as catalog metadata.
json StatLayoutFailure(const sgptDataTable* tables) {
    json result = Failure("stat metadata layout incompatible with indexed S13 records");
    json diagnostic = {{"sdk_record_bytes", sizeof(ItemStatCostTxt)},
        {"sdk_stat_offset", offsetof(ItemStatCostTxt, wStat)},
        {"sdk_value_shift_offset", offsetof(ItemStatCostTxt, bValShift)},
        {"sdk_encoding_offset", offsetof(ItemStatCostTxt, bEncode)},
        {"sdk_save_bits_offset", offsetof(ItemStatCostTxt, bSaveBits)},
        {"sdk_save_add_offset", offsetof(ItemStatCostTxt, dwSaveAdd)},
        {"sdk_save_param_bits_offset", offsetof(ItemStatCostTxt, dwSaveParamBits)},
        {"sdk_table_pointer_offset", offsetof(sgptDataTable, pItemStatCostTxt)},
        {"sdk_table_count_offset", offsetof(sgptDataTable, dwItemStatCostRecs)},
        {"loaded_lookup_matches_s13", S13ItemStatCostLookupMatches()},
        {"record_count", tables->dwItemStatCostRecs}};
    const BYTE* mod = reinterpret_cast<const BYTE*>(GetModuleHandleA("ProjectDiablo.dll"));
    if (mod && Readable(mod + 0x2CE6D0, 0x29)) {
        const BYTE* code = mod + 0x2CE6D0;
        constexpr char hex[] = "0123456789abcdef";
        std::string bytes(0x29 * 2, '0');
        for (size_t i = 0; i < 0x29; ++i) {
            bytes[i * 2] = hex[code[i] >> 4];
            bytes[i * 2 + 1] = hex[code[i] & 15];
        }
        diagnostic["loaded_mod_getter_bytes"] = std::move(bytes);
        if (code[0] == 0xA1) {
            DWORD operand = 0;
            std::memcpy(&operand, code + 1, sizeof(operand));
            const uintptr_t base = reinterpret_cast<uintptr_t>(mod);
            if (operand >= base && operand - base < 0x536000)
                diagnostic["mod_getter_global_rva"] = operand - base;
        }
    }
    if (tables->pItemStatCostTxt && tables->dwItemStatCostRecs <= 4096) {
        for (DWORD i = 0; i < tables->dwItemStatCostRecs; ++i) {
            const uintptr_t start = reinterpret_cast<uintptr_t>(tables->pItemStatCostTxt);
            if (start > std::numeric_limits<uintptr_t>::max() -
                    (size_t(i) + 1) * sizeof(ItemStatCostTxt) ||
                !Readable(reinterpret_cast<const void*>(start + size_t(i) * sizeof(ItemStatCostTxt)),
                    sizeof(ItemStatCostTxt))) break;
            if (tables->pItemStatCostTxt[i].wStat != i) {
                diagnostic["first_indexed_id_mismatch"] = i;
                break;
            }
        }
    }
    result["stat_layout_diagnostic"] = std::move(diagnostic);
    return result;
}

bool Limit(const json& args, const char* key, unsigned fallback, unsigned maximum, unsigned& out) {
    if (!args.is_object()) return false;
    out = fallback;
    if (!args.contains(key)) return true;
    const auto& v = args.at(key);
    if (!v.is_number_integer() && !v.is_number_unsigned()) return false;
    if (v.is_number_unsigned()) {
        auto n = v.get<uint64_t>();
        if (!n || n > maximum) return false;
        out = static_cast<unsigned>(n);
    } else {
        auto n = v.get<int64_t>();
        if (n <= 0 || n > maximum) return false;
        out = static_cast<unsigned>(n);
    }
    return true;
}
bool Nonnegative(const json& args, const char* key, uint64_t maximum, unsigned& out) {
    if (!args.is_object() || !args.contains(key) || !args[key].is_number_integer()) return false;
    const auto& value = args[key];
    if (value.is_number_unsigned()) {
        const uint64_t n = value.get<uint64_t>();
        if (n > maximum) return false;
        out = static_cast<unsigned>(n);
    } else {
        const int64_t n = value.get<int64_t>();
        if (n < 0 || static_cast<uint64_t>(n) > maximum) return false;
        out = static_cast<unsigned>(n);
    }
    return true;
}
std::string WideUtf8(const wchar_t* p) {
    if (!Readable(p, sizeof(wchar_t))) return {};
    size_t count = 0;
    while (count < 255 && Readable(p + count, sizeof(wchar_t)) && p[count]) ++count;
    if (count == 255 || !Readable(p + count, sizeof(wchar_t))) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes > 1024) return {};
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, p, static_cast<int>(count), &result[0], bytes, nullptr, nullptr);
    return result;
}
std::string UnitName(UnitAny* unit) {
    if (unit->dwType == UNIT_PLAYER && Valid(unit->pPlayerData))
        return std::string(unit->pPlayerData->szName, strnlen(unit->pPlayerData->szName, sizeof(unit->pPlayerData->szName)));
    if (unit->dwType == UNIT_OBJECT && unit->dwTxtFileNo == 60)
        return WideUtf8(D2CLIENT_GetUnitName(unit));
    if (unit->dwType == UNIT_OBJECT && Valid(unit->pObjectData) &&
        Valid(unit->pObjectData->pTxt)) {
        const ObjectTxt* text = unit->pObjectData->pTxt;
        std::string name = WideUtf8(text->wszName);
        if (!name.empty()) return name;
        return std::string(text->szName, strnlen(text->szName, sizeof(text->szName)));
    }
    return WideUtf8(D2CLIENT_GetUnitName(unit));
}
UnitAny* Player() {
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    return Valid(player) && player->dwType == UNIT_PLAYER ? player : nullptr;
}
json Position(const UnitAny* unit) {
    if (unit->dwType == UNIT_OBJECT && Valid(unit->pObjectPath))
        return {{"x", unit->pObjectPath->dwPosX}, {"y", unit->pObjectPath->dwPosY}};
    if (unit->dwType != UNIT_ITEM && Valid(unit->pPath))
        return {{"x", unit->pPath->xPos}, {"y", unit->pPath->yPos}};
    if (unit->dwType == UNIT_ITEM && Valid(unit->pItemPath))
        return {{"x", unit->pItemPath->dwPosX}, {"y", unit->pItemPath->dwPosY}};
    return {{"x", nullptr}, {"y", nullptr}};
}
json RoomBounds(const Room2* room) {
    return {{"x", room->dwPosX * 5}, {"y", room->dwPosY * 5},
            {"width", room->dwSizeX * 5}, {"height", room->dwSizeY * 5}};
}
}

namespace WorldState {
json DescribeItem(UnitAny* item) {
    if (!Valid(item) || item->dwType != UNIT_ITEM || !Valid(item->pItemData)) return Failure("invalid item unit");
    const ItemData* data = item->pItemData;
    json result = {{"ok", true}, {"id", item->dwUnitId}, {"type", item->dwType},
                   {"class_id", item->dwTxtFileNo}, {"mode", item->dwMode},
                   {"quality", data->dwQuality}, {"file_index", data->dwFileIndex},
                   {"item_level", data->dwItemLevel}, {"flags", data->dwFlags},
                   {"node_page", data->NodePage}, {"item_location", data->ItemLocation},
                   {"body_location", data->BodyLocation},
                   {"rare_prefix", data->wRarePrefix}, {"rare_suffix", data->wRareSuffix},
                   {"auto_prefix", data->wAutoPrefix},
                   {"prefixes", {data->wPrefix[0], data->wPrefix[1], data->wPrefix[2]}},
                   {"suffixes", {data->wSuffix[0], data->wSuffix[1], data->wSuffix[2]}},
                   {"name", UnitName(item)}};
    ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
    if (Valid(text)) {
        result["code"] = GetItemCode(text->szCode);
        result["width"] = text->binvwidth;
        result["height"] = text->binvheight;
    } else {
        result["code"] = nullptr;
        result["width"] = nullptr;
        result["height"] = nullptr;
    }
    if (Valid(item->pItemPath) && item->dwMode != ITEM_MODE_ON_GROUND) {
        result["grid_x"] = item->pItemPath->dwPosX;
        result["grid_y"] = item->pItemPath->dwPosY;
    } else {
        result["grid_x"] = nullptr;
        result["grid_y"] = nullptr;
    }
    if (item->dwMode == ITEM_MODE_ON_GROUND || item->dwMode == ITEM_MODE_BEING_DROPPED)
        result["position"] = Position(item);
    else result["position"] = nullptr;
    if (item->dwMode == ITEM_MODE_ON_CURSOR) result["location"] = "cursor";
    else if (item->dwMode == ITEM_MODE_ON_GROUND) result["location"] = "ground";
    else if (item->dwMode == ITEM_MODE_SOCKETED_IN_ITEM) result["location"] = "socket";
    else if (data->NodePage == NODEPAGE_EQUIP) result["location"] = "equipped";
    else if (data->NodePage == NODEPAGE_BELTSLOTS) result["location"] = "belt";
    else if (data->NodePage == NODEPAGE_STORAGE) {
        switch (data->ItemLocation) {
        case 0: result["location"] = "inventory"; break;
        case 3: result["location"] = "cube"; break;
        case 4: result["location"] = "stash"; break;
        default: result["location"] = "storage"; break;
        }
    } else result["location"] = "other";
    result["owner_id"] = Valid(data->pOwnerInventory) && Valid(data->pOwnerInventory->pOwner)
        ? json(data->pOwnerInventory->pOwner->dwUnitId) : json(nullptr);

    // Raw stat ID/subindex/value, not localized labels or a fabricated affix explanation.
    // The game's computed stat API is also used for common effective properties.
    json properties = json::array();
    if (item->pStats) {
        if (!Valid(item->pStats) || item->pStats->wStatCount1 > 256 ||
            (item->pStats->wStatCount1 &&
             !Readable(item->pStats->pStat, sizeof(Stat) * item->pStats->wStatCount1)))
            return Failure("invalid item stat list");
        for (unsigned i = 0; i < item->pStats->wStatCount1; ++i) {
            const Stat& stat = item->pStats->pStat[i];
            properties.push_back({{"stat_id", stat.wStatIndex}, {"layer", stat.wSubIndex}, {"value", stat.dwStatValue}});
        }
    }
    result["properties"] = std::move(properties);
    json magic = json::array();
    StatList* modifiers = D2COMMON_GetItemPropertyList(item, 0, 0x40);
    if (modifiers) {
        if (!Readable(modifiers, 0x2C) || modifiers->wStatCount1 > 256 ||
            (modifiers->wStatCount1 &&
             !Readable(modifiers->pStat, sizeof(Stat) * modifiers->wStatCount1)))
            return Failure("invalid item modifier list");
        for (unsigned i = 0; i < modifiers->wStatCount1; ++i) {
            const Stat& stat = modifiers->pStat[i];
            json property = {{"stat_id", stat.wStatIndex}, {"layer", stat.wSubIndex},
                             {"value", static_cast<int32_t>(stat.dwStatValue)}};
            magic.push_back(property);
            result["properties"].push_back(std::move(property));
        }
    }
    result["magic_properties"] = std::move(magic);
    result["properties_scope"] = "base_and_magic_property_lists";
    result["effective"] = {{"sockets", D2COMMON_GetUnitStat(item, STAT_SOCKETS, 0)},
                           {"quantity", D2COMMON_GetUnitStat(item, STAT_AMMOQUANTITY, 0)},
                           {"durability", D2COMMON_GetUnitStat(item, STAT_DURABILITY, 0)},
                           {"defense", D2COMMON_GetUnitStat(item, STAT_DEFENSE, 0)}};
    json set = SetItems::Describe(item);
    if (!set.value("ok", true)) {
        if (set.value("error", "") !=
            "set item base code does not match selected piece")
            return set;
        result["set_description_error"] = std::move(set);
    } else {
        result.update(set);
    }
    json sockets = ItemCrafting::DescribeSockets(item);
    if (!sockets.value("ok", true)) return sockets;
    result.update(sockets);
    return result;
}

json Inventory(const json& args) {
    unsigned limit;
    if (!Limit(args, "max_items", 128, 512, limit)) return Failure("invalid max_items");
    UnitAny* player = Player();
    if (!player) return Failure("player unavailable");
    ::Inventory* inv = player->pInventory;
    if (!Valid(inv) || inv->pOwner != player) return Failure("player inventory unavailable");
    json result = {{"ok", true}, {"player_id", player->dwUnitId}, {"items", json::array()},
                   {"cursor_id", nullptr}, {"cursor", nullptr}, {"truncated", false}};
    result["grids"] = json::object();
    for (unsigned storage : {unsigned(STORAGE_INVENTORY), unsigned(STORAGE_CUBE), unsigned(STORAGE_STASH)}) {
        if (storage == STORAGE_STASH && !D2CLIENT_GetUIState(UI_STASH)) continue;
        if (storage == STORAGE_CUBE && !D2CLIENT_GetUIState(UI_CUBE)) continue;
        unsigned width = 0, height = 0;
        if (InventoryActions::GridSize(inv, storage, width, height))
            result["grids"][storage == STORAGE_STASH ? "stash" : storage == STORAGE_CUBE ? "cube" : "inventory"] =
                {{"width", width}, {"height", height}};
    }
    std::unordered_set<UnitAny*> seen;
    UnitAny* current = D2COMMON_GetItemFromInventory(inv);
    for (unsigned i = 0; current; ++i) {
        if (i >= limit) { result["truncated"] = true; break; }
        if (!Valid(current) || !seen.insert(current).second) return Failure("invalid inventory item chain");
        json item = DescribeItem(current);
        if (!item.value("ok", false)) return Failure("invalid inventory item");
        result["items"].push_back(std::move(item));
        current = D2COMMON_GetNextItemFromInventory(current);
    }
    UnitAny* cursor = inv->pCursorItem;
    if (cursor) {
        json item = DescribeItem(cursor);
        if (!item.value("ok", false)) return Failure("invalid cursor item");
        result["cursor_id"] = cursor->dwUnitId;
        result["cursor"] = std::move(item);
    }
    return result;
}

json ItemStats(const json& args) {
    unsigned id;
    if (!Nonnegative(args, "item_id", UINT32_MAX, id) || !args.contains("stats") ||
        !args["stats"].is_array() || args["stats"].empty() || args["stats"].size() > 64)
        return Failure("item_id and 1-64 typed stat requests are required");
    unsigned statIds[64] = {}, layers[64] = {};
    unsigned count = 0;
    for (const auto& entry : args["stats"]) {
        if (!Nonnegative(entry, "stat_id", 511, statIds[count]) ||
            (entry.contains("layer") && !Nonnegative(entry, "layer", 65535, layers[count])))
            return Failure("stat_id must be 0-511 and layer 0-65535");
        ++count;
    }
    UnitAny* player = Player();
    if (!player || !Valid(player->pInventory) || player->pInventory->pOwner != player)
        return Failure("player inventory unavailable");
    UnitAny* selected = nullptr;
    UnitAny* item = D2COMMON_GetItemFromInventory(player->pInventory);
    for (unsigned visited = 0; item; ++visited) {
        if (visited >= 512 || !Valid(item) || item->dwType != UNIT_ITEM ||
            !Valid(item->pItemData) || item->pItemData->pOwnerInventory != player->pInventory)
            return Failure("owned item chain is invalid or incomplete");
        if (item->dwUnitId == id) { selected = item; break; }
        item = D2COMMON_GetNextItemFromInventory(item);
    }
    if (!selected) {
        item = player->pInventory->pCursorItem;
        if (Valid(item) && item->dwType == UNIT_ITEM && item->dwUnitId == id &&
            Valid(item->pItemData) && item->pItemData->pOwnerInventory == player->pInventory)
            selected = item;
    }
    if (!selected) return Failure("item is not owned by the active player");
    json result = {{"ok", true}, {"item_id", id}, {"source", "native_computed"}, {"stats", json::array()}};
    for (unsigned i = 0; i < count; ++i)
        result["stats"].push_back({{"stat_id", statIds[i]}, {"layer", layers[i]},
            {"value", static_cast<int32_t>(D2COMMON_GetUnitStat(selected, statIds[i], layers[i]))}});
    return result;
}

json Units(const json& args) {
    unsigned maxUnits, maxRooms, radius;
    if (!Limit(args, "max_units", 128, 512, maxUnits) ||
        !Limit(args, "max_rooms", 64, 256, maxRooms) ||
        !Limit(args, "radius", 80, 4096, radius)) return Failure("invalid units bounds");
    if (args.contains("objects_only") && !args["objects_only"].is_boolean())
        return Failure("objects_only must be a boolean");
    const bool objectsOnly = args.value("objects_only", false);
    UnitAny* player = Player();
    if (!player || !Valid(player->pPath) || !Valid(player->pAct) ||
        !Valid(player->pPath->pRoom1) || !Valid(player->pPath->pRoom1->pRoom2) ||
        !Valid(player->pPath->pRoom1->pRoom2->pLevel)) return Failure("player path unavailable");
    const int px = player->pPath->xPos, py = player->pPath->yPos;
    json result = {{"ok", true}, {"units", json::array()}, {"truncated", false},
                   {"center", {{"x", px}, {"y", py}}}};
    const DWORD levelId = player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
    std::unordered_set<Room1*> rooms;
    for (Room1* room = player->pAct->pRoom1; room; room = room->pRoomNext) {
        if (rooms.size() >= maxRooms) { result["truncated"] = true; break; }
        if (!Valid(room) || !rooms.insert(room).second) return Failure("invalid loaded room chain");
    }
    // The client hash table includes objects absent from Room1::pUnitFirst (e.g. town stash).
    // D2Ptrs.h:339 and docs/ARCHITECTURE.md:268-274: six types, 128 buckets/type.
    constexpr unsigned kBucketsPerType = 128;
    constexpr unsigned kTypes = 6;
    auto table = reinterpret_cast<UnitAny* const*>(p_D2CLIENT_pUnitTable);
    if (!Readable(table, kBucketsPerType * kTypes * sizeof(UnitAny*)))
        return Failure("client unit table unavailable");
    for (unsigned type : {unsigned(UNIT_OBJECT), unsigned(UNIT_MONSTER),
                          unsigned(UNIT_PLAYER), unsigned(UNIT_ITEM)}) {
        if (objectsOnly && type != UNIT_OBJECT) continue;
        for (unsigned bucket = 0; bucket < kBucketsPerType; ++bucket) {
            std::unordered_set<UnitAny*> chain;
            for (UnitAny* unit = table[type * kBucketsPerType + bucket];
                 unit; unit = unit->pListNext) {
                if (chain.size() >= 2048 || !Valid(unit) || !chain.insert(unit).second ||
                    unit->dwType != type) return Failure("invalid client unit bucket");
                if (unit == player) continue;
                json pos = Position(unit);
                if (!pos["x"].is_number() || !pos["y"].is_number()) continue;
                const int x = pos["x"].get<int>(), y = pos["y"].get<int>();
                if (type == UNIT_ITEM && unit->dwMode != ITEM_MODE_ON_GROUND &&
                    unit->dwMode != ITEM_MODE_BEING_DROPPED) continue;
                Room1* unitRoom = D2COMMON_GetRoomFromUnit(unit);
                if (Valid(unitRoom) && Valid(unitRoom->pRoom2) &&
                    Valid(unitRoom->pRoom2->pLevel) &&
                    unitRoom->pRoom2->pLevel->dwLevelNo != levelId) continue;
                if (!unitRoom && type == UNIT_ITEM) {
                    bool inLoadedRoom = false;
                    for (Room1* loaded : rooms) {
                        if (Valid(loaded->pRoom2) && Valid(loaded->pRoom2->pLevel) &&
                            loaded->pRoom2->pLevel->dwLevelNo == levelId &&
                            x >= int(loaded->dwXStart) && y >= int(loaded->dwYStart) &&
                            uint64_t(x) < uint64_t(loaded->dwXStart) + loaded->dwXSize &&
                            uint64_t(y) < uint64_t(loaded->dwYStart) + loaded->dwYSize) {
                            inLoadedRoom = true;
                            break;
                        }
                    }
                    if (!inLoadedRoom) continue;
                }
                if (!Valid(unitRoom) && type != UNIT_ITEM) continue;
            if (std::max(std::abs(x - px), std::abs(y - py)) > static_cast<int>(radius)) continue;
            if (result["units"].size() >= maxUnits) { result["truncated"] = true; return result; }
            json entry = {{"type", unit->dwType}, {"class_id", unit->dwTxtFileNo},
                          {"id", unit->dwUnitId}, {"mode", unit->dwMode},
                          {"x", x}, {"y", y}, {"name", UnitName(unit)},
                          {"distance_chebyshev", std::max(std::abs(x - px), std::abs(y - py))}};
            if (unit->dwType == UNIT_MONSTER || unit->dwType == UNIT_OBJECT)
                entry["distance_native"] = D2COMMON_GetUnitDistance(player, unit);
            if (unit->dwType == UNIT_MONSTER) {
                entry["owner_type"] = unit->dwOwnerType;
                entry["owner_id"] = unit->dwOwnerId;
                entry["unit_flags"] = unit->dwFlags;
                entry["selectable"] = bool(unit->dwFlags & UNITFLAG_SELECTABLE);
                if (Valid(unit->pMonsterData) && Valid(unit->pMonsterData->pMonStatsTxt)) {
                    const MonStatsTxt* txt = unit->pMonsterData->pMonStatsTxt;
                    entry["npc"] = static_cast<bool>(txt->dwFlags.bNPC);
                    entry["town"] = static_cast<bool>(txt->dwFlags.binTown);
                    entry["killable"] = static_cast<bool>(txt->dwFlags.bKillable);
                    entry["alignment"] = txt->bAlign; // raw MonStatsTxt field; not a hostility inference
                    entry["friendly"] = txt->dwFlags.bNPC && txt->dwFlags.binTown ? json(true) : json(nullptr);
                    entry["boss"] = static_cast<bool>(unit->pMonsterData->fBoss);
                    entry["champion"] = static_cast<bool>(unit->pMonsterData->fChamp);
                    entry["minion"] = static_cast<bool>(unit->pMonsterData->fMinion);
                } else {
                    entry["npc"] = nullptr;
                    entry["town"] = nullptr;
                    entry["killable"] = nullptr;
                    entry["alignment"] = nullptr;
                    entry["friendly"] = nullptr;
                }
                entry["dead"] = unit->dwMode == NPC_MODE_DEATH || unit->dwMode == NPC_MODE_DEAD;
            } else if (unit->dwType == UNIT_PLAYER) {
                entry["dead"] = unit->dwMode == PLAYER_MODE_DEATH || unit->dwMode == PLAYER_MODE_DEAD;
            }
            if (unit->dwType == UNIT_MONSTER || unit->dwType == UNIT_PLAYER) {
                entry["hp_raw"] = D2COMMON_GetUnitStat(unit, STAT_HP, 0);
                entry["max_hp_raw"] = D2COMMON_GetUnitStat(unit, STAT_MAXHP, 0);
            }
            if (unit->dwType == UNIT_ITEM) {
                entry = DescribeItem(unit);
                if (!entry.value("ok", false)) {
                    entry["context"] = "invalid ground item";
                    entry["item_id"] = unit->dwUnitId;
                    entry["position"] = {{"x", x}, {"y", y}};
                    return entry;
                }
                entry["x"] = x;
                entry["y"] = y;
            }
            result["units"].push_back(std::move(entry));
            }
        }
    }
    return result;
}

json Map(const json& args) {
    unsigned maxRooms, maxCells;
    if (!Limit(args, "max_rooms", 64, 256, maxRooms) ||
        !Limit(args, "max_cells", 16384, 262144, maxCells)) return Failure("invalid map bounds");
    if (args.contains("current_level_only") && !args["current_level_only"].is_boolean())
        return Failure("current_level_only must be a boolean");
    const bool currentOnly = args.value("current_level_only", false);
    UnitAny* player = Player();
    if (!player || !Valid(player->pPath) || !Valid(player->pPath->pRoom1) ||
        !Valid(player->pPath->pRoom1->pRoom2) || !Valid(player->pPath->pRoom1->pRoom2->pLevel) ||
        !Valid(player->pAct)) return Failure("player room unavailable");
    Level* level = player->pPath->pRoom1->pRoom2->pLevel;
    json result = {{"ok", true}, {"level_id", level->dwLevelNo},
                   {"player", {{"x", player->pPath->xPos}, {"y", player->pPath->yPos}}},
                   {"level_bounds", {{"x", level->dwPosX * 5}, {"y", level->dwPosY * 5},
                                      {"width", level->dwSizeX * 5}, {"height", level->dwSizeY * 5}}},
                   {"rooms", json::array()}, {"exits", json::array()}, {"truncated", false}};
    // Only loaded Room1 nodes and their explicitly linked Room2 neighbors are known.
    std::unordered_set<Room1*> loaded;
    std::unordered_set<Room2*> included;
    unsigned cells = 0;
    // Spend bounded room/cell capacity on the actual player's collision first.
    // Keep native-chain cycle detection independent of this priority visit.
    Room1* currentRoom = player->pPath->pRoom1;
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (Room1* room = pass == 0 ? currentRoom : player->pAct->pRoom1;
             room; room = pass == 0 ? nullptr : room->pRoomNext) {
            if (loaded.size() >= 4096) return Failure("loaded act room chain exceeds bound");
            if (!Valid(room) || (pass != 0 && !loaded.insert(room).second))
                return Failure("invalid loaded room chain");
            if (pass != 0 && room == currentRoom) continue;
            Room2* room2 = room->pRoom2;
            if (!Valid(room2) || !Valid(room2->pLevel)) return Failure("invalid room metadata");
            if (currentOnly && room2->pLevel != level) continue;
            if (included.size() >= maxRooms) { result["truncated"] = true; break; }
            included.insert(room2);
            json entry = {{"level_id", room2->pLevel->dwLevelNo}, {"bounds", RoomBounds(room2)},
                          {"loaded", true}, {"collision", nullptr},
                          {"adjacent", json::array()}, {"presets", json::array()}};
            if (room->dwRoomsNear && room->dwRoomsNear <= 64 &&
                Readable(room->pRoomsNear, room->dwRoomsNear * sizeof(Room1*))) {
                for (unsigned i = 0; i < room->dwRoomsNear; ++i) {
                    Room1* neighbor = room->pRoomsNear[i];
                    if (Valid(neighbor) && Valid(neighbor->pRoom2) && Valid(neighbor->pRoom2->pLevel))
                        entry["adjacent"].push_back({{"level_id", neighbor->pRoom2->pLevel->dwLevelNo},
                                                     {"bounds", RoomBounds(neighbor->pRoom2)}});
                }
            }
            if (Valid(room->Coll)) {
                CollMap* collision = room->Coll;
                const uint64_t count = uint64_t(collision->dwSizeGameX) * collision->dwSizeGameY;
                if (count > 0 && count <= maxCells - cells && count <= 65536 &&
                    Readable(collision->pMapStart, static_cast<size_t>(count) * sizeof(WORD))) {
                    json values = json::array();
                    for (uint64_t i = 0; i < count; ++i) values.push_back(collision->pMapStart[i]);
                    entry["collision"] = {{"x", collision->dwPosGameX}, {"y", collision->dwPosGameY},
                                          {"width", collision->dwSizeGameX}, {"height", collision->dwSizeGameY},
                                          {"values", std::move(values)}};
                    cells += static_cast<unsigned>(count);
                } else result["truncated"] = true;
            }
            // Preset coordinates are relative to Room2 origin (room units * 5).
            std::unordered_set<PresetUnit*> presets;
            for (PresetUnit* preset = room2->pPreset; preset; preset = preset->pPresetNext) {
                if (presets.size() >= 256) { result["truncated"] = true; break; }
                if (!Valid(preset) || !presets.insert(preset).second) return Failure("invalid preset chain");
                entry["presets"].push_back({{"type", preset->dwType}, {"class_id", preset->dwTxtFileNo},
                                            {"x", room2->dwPosX * 5 + preset->dwPosX},
                                            {"y", room2->dwPosY * 5 + preset->dwPosY}});
            }
            std::unordered_set<RoomTile*> tiles;
            for (RoomTile* tile = room2->pRoomTiles; tile; tile = tile->pNext) {
                if (tiles.size() >= 64) { result["truncated"] = true; break; }
                if (!Valid(tile) || !tiles.insert(tile).second) return Failure("invalid exit tile chain");
                if (!Valid(tile->pRoom2) || !Valid(tile->pRoom2->pLevel)) continue;
                result["exits"].push_back({{"from_bounds", RoomBounds(room2)},
                                           {"to_level_id", tile->pRoom2->pLevel->dwLevelNo},
                                           {"to_bounds", RoomBounds(tile->pRoom2)},
                                           {"raw_tile_number", Readable(tile->nNum, sizeof(DWORD)) ? json(*tile->nNum) : json(nullptr)}});
            }
            result["rooms"].push_back(std::move(entry));
        }
    }
    // Adjacent metadata only; no collision or presets claimed for unloaded rooms.
    json neighbors = json::array();
    std::unordered_set<Room2*> visited;
    for (Room2* room2 : included) {
        if (!room2->dwRoomsNear || room2->dwRoomsNear > 64 ||
            !Readable(room2->pRoom2Near, room2->dwRoomsNear * sizeof(Room2*))) continue;
        for (unsigned i = 0; i < room2->dwRoomsNear; ++i) {
            Room2* neighbor = room2->pRoom2Near[i];
            if (Valid(neighbor) && Valid(neighbor->pLevel) &&
                room2->pLevel->dwLevelNo == level->dwLevelNo &&
                neighbor->pLevel->dwLevelNo != level->dwLevelNo)
                result["exits"].push_back({{"kind", "adjacent_level_boundary"},
                    {"from_bounds", RoomBounds(room2)}, {"to_level_id", neighbor->pLevel->dwLevelNo},
                    {"to_bounds", RoomBounds(neighbor)}, {"loaded", Valid(neighbor->pRoom1)}});
            if (Valid(neighbor) && Valid(neighbor->pLevel) && included.count(neighbor) == 0 &&
                visited.insert(neighbor).second && neighbors.size() < maxRooms)
                neighbors.push_back({{"level_id", neighbor->pLevel->dwLevelNo},
                                     {"bounds", RoomBounds(neighbor)}, {"loaded", Valid(neighbor->pRoom1)}});
        }
    }
    result["adjacent_rooms"] = std::move(neighbors);
    // Generated room metadata is complete independently of the currently loaded
    // collision window. It lets callers account for exploration without force-
    // loading rooms or claiming that unseen monster populations were observed.
    result["level_rooms"] = json::array();
    result["room_catalog_complete"] = true;
    std::unordered_set<Room2*> levelRooms;
    for (Room2* room = level->pRoom2First; room; room = room->pRoom2Next) {
        if (levelRooms.size() >= maxRooms) {
            result["room_catalog_complete"] = false;
            break;
        }
        if (!Valid(room) || room->pLevel != level || !levelRooms.insert(room).second)
            return Failure("invalid generated level room chain");
        json metadata = {{"level_id", level->dwLevelNo}, {"bounds", RoomBounds(room)},
                         {"loaded", Valid(room->pRoom1)}, {"adjacent", json::array()}};
        if (room->dwRoomsNear > 64 ||
            (room->dwRoomsNear && !Readable(room->pRoom2Near, room->dwRoomsNear * sizeof(Room2*))))
            return Failure("invalid generated room adjacency");
        for (unsigned i = 0; i < room->dwRoomsNear; ++i) {
            Room2* neighbor = room->pRoom2Near[i];
            if (!Valid(neighbor) || !Valid(neighbor->pLevel))
                return Failure("invalid generated room neighbor");
            metadata["adjacent"].push_back({{"level_id", neighbor->pLevel->dwLevelNo},
                                              {"bounds", RoomBounds(neighbor)}});
        }
        result["level_rooms"].push_back(std::move(metadata));
    }
    return result;
}

json Catalog(const json& args) {
    const bool filtered = args.is_object() && args.contains("codes");
    unsigned limit;
    if (!Limit(args, "limit", 64, 128, limit) || !args.contains("table") ||
        !args["table"].is_string()) return Failure("invalid catalog arguments");
    std::array<std::string, 8> codes;
    unsigned codeCount = 0;
    if (filtered) {
        if (args["table"] != "bases" || args.contains("offset") || args.contains("limit") ||
            !args["codes"].is_array() || args["codes"].empty() || args["codes"].size() > codes.size())
            return Failure("codes require bases table without offset or limit (1-8 exact codes)");
        for (const auto& value : args["codes"]) {
            if (!value.is_string()) return Failure("invalid base code");
            std::string code = value.get<std::string>();
            if (code.empty() || code.size() > 4 ||
                !std::all_of(code.begin(), code.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; }))
                return Failure("invalid base code");
            for (unsigned n = 0; n < codeCount; ++n)
                if (codes[n] == code) return Failure("duplicate requested base code");
            codes[codeCount++] = std::move(code);
        }
    }
    unsigned offset = 0;
    if (args.contains("offset")) {
        if (!args["offset"].is_number_unsigned() ||
            args["offset"].get<uint64_t>() > 32768) return Failure("invalid catalog offset");
        offset = args["offset"].get<unsigned>();
    }
    if (!Player()) return Failure("player unavailable");
    std::string tableName = args["table"].get<std::string>();
    if (tableName == "sets") return SetItems::Catalog(args);
    json rows = json::array();
    unsigned count = 0;
    if (tableName == "bases") {
        D2ItemDataTbl* tables = D2COMMON_10535_DATATBLS_GetItemDataTables();
        if (!Valid(tables) || tables->nItemsTxtRecordCount <= 0 ||
            tables->nItemsTxtRecordCount > 32768) return Failure("item catalog unavailable");
        count = static_cast<unsigned>(tables->nItemsTxtRecordCount);
        std::array<unsigned, 8> matchIds;
        matchIds.fill(count);
        std::array<unsigned, 8> matchCounts{};
        if (filtered) {
            if (!Readable(tables->pItemsTxt, count * sizeof(ItemsTxt)))
                return Failure("item catalog table unreadable");
            for (unsigned i = 0; i < count; ++i) {
                const char* raw = tables->pItemsTxt[i].szCode;
                unsigned length = 4;
                while (length && (raw[length - 1] == ' ' || raw[length - 1] == '\0')) --length;
                for (unsigned n = 0; n < codeCount; ++n) {
                    if (codes[n].size() == length && !std::memcmp(raw, codes[n].data(), length)) {
                        if (matchCounts[n] == 0) matchIds[n] = i;
                        if (matchCounts[n] < 2) ++matchCounts[n];
                    }
                }
            }
        }
        const unsigned end = std::min(count, offset + limit);
        if (!filtered && offset < end && (!tables->pItemsTxt ||
            !Readable(tables->pItemsTxt + offset, (end - offset) * sizeof(ItemsTxt))))
            return Failure("item catalog page unreadable");
        for (unsigned slot = 0; slot < (filtered ? codeCount : (offset < end ? end - offset : 0)); ++slot) {
            const unsigned i = filtered ? matchIds[slot] : offset + slot;
            if (i == count || (filtered && matchCounts[slot] != 1)) continue;
            const ItemsTxt& record = tables->pItemsTxt[i];
            rows.push_back({{"id", i}, {"code", GetItemCode(record.szCode)},
                            {"name", WideUtf8(D2LANG_GetLocaleText(record.wnamestr))},
                            {"name_string_id", record.wnamestr}, {"item_type", record.nType},
                            {"level", record.blevel}, {"required_level", record.blevelreq},
                            {"width", record.binvwidth}, {"height", record.binvheight},
                            {"is_gem", nullptr}, {"item_type_code", nullptr},
                            {"secondary_item_type", nullptr}, {"secondary_item_type_code", nullptr},
                            {"type_ancestry_scope", nullptr},
                            {"spawnable", record.bspawnable != 0}});
            json equipment = {{"slots", json::array()}, {"required_strength", record.wreqstr},
                {"required_dexterity", record.wreqdex}, {"required_level", record.blevelreq},
                {"class_id", nullptr}, {"coverage", "item_type_unavailable"}};
            rows.back()["equipment"] = std::move(equipment);
            if (Readable(p_D2COMMON_sgptDataTable, sizeof(sgptDataTable*)) &&
                Valid(*p_D2COMMON_sgptDataTable)) {
                sgptDataTable* all = *p_D2COMMON_sgptDataTable;
                if (all->dwItemsTypeRecs > 0 && all->dwItemsTypeRecs <= 32768 &&
                    Readable(all->pItemsTypeTxt, all->dwItemsTypeRecs * sizeof(D2ItemTypesTxt))) {
                    WORD queue[64] = {record.nType};
                    unsigned size = 1;
                    if (record.wtype2 && record.wtype2 != 0xffff) {
                        if (record.wtype2 >= all->dwItemsTypeRecs)
                            return Failure("invalid secondary base item type");
                        rows.back()["secondary_item_type"] = record.wtype2;
                        rows.back()["secondary_item_type_code"] =
                            GetItemCode(all->pItemsTypeTxt[record.wtype2].szCode);
                        if (record.wtype2 != record.nType) queue[size++] = record.wtype2;
                    }
                    bool gem = false;
                    json types = json::array();
                    for (unsigned next = 0; next < size; ++next) {
                        WORD id = queue[next];
                        if (id >= all->dwItemsTypeRecs) return Failure("invalid base item type");
                        const D2ItemTypesTxt& type = all->pItemsTypeTxt[id];
                        types.push_back(id);
                        gem = gem || type.nGem != 0;
                        if (next == 0)
                            rows.back()["item_type_code"] = GetItemCode(type.szCode);
                        if (next == 0) {
                            if (type.nBody) {
                                if (type.nBodyLoc1) rows.back()["equipment"]["slots"].push_back(type.nBodyLoc1);
                                if (type.nBodyLoc2 && type.nBodyLoc2 != type.nBodyLoc1)
                                    rows.back()["equipment"]["slots"].push_back(type.nBodyLoc2);
                            }
                            if (type.nClass <= 6) rows.back()["equipment"]["class_id"] = type.nClass;
                            rows.back()["equipment"]["coverage"] = "native_tables";
                        }
                        for (WORD parent : {type.nEquiv1, type.nEquiv2}) {
                            if (!parent) continue;
                            bool seen = false;
                            for (unsigned n = 0; n < size; ++n) seen = seen || queue[n] == parent;
                            if (!seen) {
                                if (size == 64) return Failure("item type graph exceeds bound");
                                queue[size++] = parent;
                            }
                        }
                    }
                    rows.back()["is_gem"] = gem;
                    rows.back()["type_ancestors"] = std::move(types);
                    rows.back()["type_ancestry_scope"] = "primary_and_secondary_equivalence";
                }
            }
        }
        if (filtered) {
            json missing = json::array(), ambiguous = json::array();
            for (unsigned n = 0; n < codeCount; ++n) {
                if (!matchCounts[n]) missing.push_back(codes[n]);
                else if (matchCounts[n] > 1) ambiguous.push_back(codes[n]);
            }
            return {{"ok", true}, {"table", tableName}, {"total", count},
                {"codes", args["codes"]}, {"records", std::move(rows)},
                {"missing_codes", std::move(missing)}, {"ambiguous_codes", std::move(ambiguous)},
                {"coverage", "complete_loaded_table"}, {"truncated", false}};
        }
    } else if (tableName == "uniques") {
        if (!Readable(p_D2COMMON_sgptDataTable, sizeof(sgptDataTable*)) ||
            !Valid(*p_D2COMMON_sgptDataTable)) return Failure("unique catalog unavailable");
        sgptDataTable* tables = *p_D2COMMON_sgptDataTable;
        if (!tables->dwUniqItemsRecs || tables->dwUniqItemsRecs > 32768)
            return Failure("unique catalog count invalid");
        count = tables->dwUniqItemsRecs;
        const unsigned end = std::min(count, offset + limit);
        if (offset < end && (!tables->pUniqueItemsTxt ||
            !Readable(tables->pUniqueItemsTxt + offset, (end - offset) * sizeof(UniqueItemsTxt))))
            return Failure("unique catalog page unreadable");
        for (unsigned i = offset; i < end; ++i) {
            const UniqueItemsTxt& record = tables->pUniqueItemsTxt[i];
            rows.push_back({{"id", i}, {"table_index", record.wTblIndex},
                            {"name", std::string(record.szName, strnlen(record.szName, sizeof(record.szName)))},
                            {"code", GetItemCode(record.szCode)},
                            {"level", record.wLvl}, {"required_level", record.wLvlReq},
                            {"rarity", record.dwRarity}, {"version", record.dwVersion},
                            {"flags", record.dwUniqueItemFlags}});
        }
    } else if (tableName == "affixes") {
        D2MagicAffixDataTbl* tables = D2COMMON_10492_DATATBLS_GetMagicAffixDataTables();
        if (!Valid(tables) || tables->nMagicAffixTxtRecordCount <= 0 ||
            tables->nMagicAffixTxtRecordCount > 32768) return Failure("affix catalog unavailable");
        count = static_cast<unsigned>(tables->nMagicAffixTxtRecordCount);
        const unsigned end = std::min(count, offset + limit);
        if (offset < end && (!tables->pMagicAffixTxt ||
            !Readable(tables->pMagicAffixTxt + offset, (end - offset) * sizeof(AutoMagicTxt))))
            return Failure("affix catalog page unreadable");
        for (unsigned i = offset; i < end; ++i) {
            const AutoMagicTxt& record = tables->pMagicAffixTxt[i];
            const AutoMagicTxt* pointer = tables->pMagicAffixTxt + i;
            const char* kind = pointer >= tables->pAutoMagic ? "automagic" :
                pointer >= tables->pMagicPrefix ? "prefix" : "suffix";
            rows.push_back({{"id", i}, {"table_index", record.wIndex},
                            {"spawn_id", i + 1}, {"kind", kind}, {"rare", record.nRare != 0},
                            {"name", std::string(record.szName, strnlen(record.szName, sizeof(record.szName)))},
                            {"level", record.dwLevel}, {"max_level", record.dwMaxLevel},
                            {"required_level", record.nLevelReq}, {"group", record.dwGroup},
                            {"spawnable", record.wSpawnable != 0}, {"frequency", record.bFrequency},
                            {"class_specific", record.nClassSpecific != 0xFF}, {"class_id", record.nClassSpecific},
                            {"item_types", record.wIType}, {"excluded_types", record.wEType}});
        }
    } else if (tableName == "stats") {
        if (!Readable(p_D2COMMON_sgptDataTable, sizeof(sgptDataTable*)) ||
            !Valid(*p_D2COMMON_sgptDataTable)) return Failure("stat metadata unavailable");
        const sgptDataTable* tables = *p_D2COMMON_sgptDataTable;
        if (!S13ItemStatCostLayoutValid(tables))
            return StatLayoutFailure(tables);
        count = tables->dwItemStatCostRecs;
        const unsigned end = std::min(count, offset + limit);
        for (unsigned i = offset; i < end; ++i) {
            const ItemStatCostTxt& record = tables->pItemStatCostTxt[i];
            rows.push_back({{"id", i}, {"stat_id", record.wStat}, {"name", nullptr},
                {"units", "native_stat_integer"}, {"value_shift", record.bValShift},
                {"encoding", record.bEncode}, {"save_bits", record.bSaveBits},
                {"save_add", record.dwSaveAdd}, {"save_param_bits", record.dwSaveParamBits}});
        }
    } else return Failure("unknown catalog table");
    return {{"ok", true}, {"table", tableName}, {"offset", offset}, {"total", count},
            {"records", std::move(rows)}, {"truncated", offset + limit < count}};
}

json Ui(const json& args) {
    if (!args.is_object()) return Failure("invalid ui arguments");
    if (!Player()) return Failure("player unavailable");
    return {{"ok", true}, {"inventory", D2CLIENT_GetUIState(UI_INVENTORY) != 0},
            {"inventory_ex", D2CLIENT_GetUIState(UI_INVENTORY_EX) != 0},
            {"stash", D2CLIENT_GetUIState(UI_STASH) != 0},
            {"cube", D2CLIENT_GetUIState(UI_CUBE) != 0},
            {"npc_menu", D2CLIENT_GetUIState(UI_NPCMENU) != 0},
            {"npc_shop", D2CLIENT_GetUIState(UI_NPCSHOP) != 0},
            {"trade", D2CLIENT_GetUIState(UI_TRADE) != 0},
            {"skill_tree", D2CLIENT_GetUIState(UI_SKILLTREE) != 0}};
}
}
