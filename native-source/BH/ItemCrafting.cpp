#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ItemCrafting.h"

#include "Constants.h"
#include "D2Helpers.h"
#include "D2Ptrs.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace {
using json = nlohmann::json;
using CubeCount = int (__stdcall*)();
using CubeRecord = CubeMainTxt* (__stdcall*)(int);
using ItemHasType = int (__stdcall*)(UnitAny*, int);
constexpr unsigned kMaxRecords = 32768;
constexpr unsigned kMaxSockets = 6;
static_assert(sizeof(RunesTxt) == 0x120, "S13 runeword record stride");
static_assert(sizeof(CubeMainTxt) == 0x148, "S13 cube recipe record stride");

json Failure(const char* error) { return {{"ok", false}, {"error", error}}; }

bool Readable(const void* p, size_t bytes) {
    if (!p || !bytes) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    if (address > std::numeric_limits<uintptr_t>::max() - bytes) return false;
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(p, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(region.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    return end >= address && bytes <= end - address;
}
template <typename T> bool Valid(const T* p) { return Readable(p, sizeof(T)); }

bool U32(const json& args, const char* name, DWORD& value) {
    if (!args.is_object() || !args.contains(name) || !args.at(name).is_number_integer()) return false;
    if (args.at(name).is_number_unsigned()) {
        const auto n = args.at(name).get<uint64_t>();
        if (n > UINT32_MAX) return false;
        value = static_cast<DWORD>(n);
    } else {
        const auto n = args.at(name).get<int64_t>();
        if (n < 0 || n > UINT32_MAX) return false;
        value = static_cast<DWORD>(n);
    }
    return true;
}
void Put32(BYTE* p, DWORD n) {
    p[0] = static_cast<BYTE>(n);
    p[1] = static_cast<BYTE>(n >> 8);
    p[2] = static_cast<BYTE>(n >> 16);
    p[3] = static_cast<BYTE>(n >> 24);
}

// Both exports are audited against the S13 Common PE; older SDK cube ordinals
// point outside that image's export range. Check the machine-code entrypoints
// before calling either, rather than trusting a matched ordinal alone.
bool CubeExports(CubeCount& count, CubeRecord& record) {
    HMODULE module = GetModuleHandleA("D2Common.dll");
    if (!module) return false;
    FARPROC c = GetProcAddress(module, MAKEINTRESOURCEA(10675));
    FARPROC r = GetProcAddress(module, MAKEINTRESOURCEA(10783));
    // ASLR relocates embedded absolute operands; inspect invariant opcodes only.
    if (!Readable(c, 6) || !Readable(r, 2) ||
        reinterpret_cast<const BYTE*>(c)[0] != 0xA1 ||
        reinterpret_cast<const BYTE*>(c)[5] != 0xC3 ||
        reinterpret_cast<const BYTE*>(r)[0] != 0x8B ||
        reinterpret_cast<const BYTE*>(r)[1] != 0x0D) return false;
    count = reinterpret_cast<CubeCount>(c);
    record = reinterpret_cast<CubeRecord>(r);
    return true;
}

bool RuneTable(int& count, RunesTxt*& first) {
    int* countPointer = D2COMMON_GetRunesTxtRecords(); // S13 -11032 returns pointer, not value.
    if (!Valid(countPointer) || *countPointer <= 0 || *countPointer > static_cast<int>(kMaxRecords)) return false;
    count = *countPointer;
    // S13 -10006 deliberately asserts on index zero; use the loaded global
    // table pointer from the audited getter's machine code instead.
    HMODULE module = GetModuleHandleA("D2Common.dll");
    if (!module) return false;
    auto* fn = GetProcAddress(module, MAKEINTRESOURCEA(10006));
    if (!Readable(fn, 0x1e) || reinterpret_cast<const BYTE*>(fn)[0] != 0x8B ||
        reinterpret_cast<const BYTE*>(fn)[1] != 0x44 ||
        reinterpret_cast<const BYTE*>(fn)[0x10] != 0x8B ||
        reinterpret_cast<const BYTE*>(fn)[0x11] != 0x0D) return false;
    RunesTxt** global = nullptr;
    std::memcpy(&global, reinterpret_cast<const BYTE*>(fn) + 0x12, sizeof(global));
    if (!Valid(global) || !Readable(*global, static_cast<size_t>(count) * sizeof(RunesTxt))) return false;
    first = *global;
    return true;
}

std::string Fixed(const char* chars, size_t length) {
    size_t end = 0;
    while (end < length && chars[end]) ++end;
    return std::string(chars, end);
}
json Stats(const ItemsTxtStat* stats, unsigned count) {
    json result = json::array();
    for (unsigned n = 0; n < count; ++n)
        result.push_back({{"property", stats[n].dwProp}, {"parameter", stats[n].dwPar},
                          {"min", stats[n].dwMin}, {"max", stats[n].dwMax}});
    return result;
}
ItemHasType NativeItemHasType() {
    HMODULE module = GetModuleHandleA("D2Common.dll");
    if (!module) return nullptr;
    FARPROC fn = GetProcAddress(module, MAKEINTRESOURCEA(10744));
    const BYTE signature[] = {0x53, 0x8B, 0x5C, 0x24, 0x08};
    return Readable(fn, sizeof(signature)) && !std::memcmp(fn, signature, sizeof(signature))
        ? reinterpret_cast<ItemHasType>(fn) : nullptr;
}
bool BaseCompatible(const RunesTxt& row, UnitAny* base, DWORD sockets, unsigned used,
                    ItemHasType itemHasType) {
    if (!base || !itemHasType || !(base->pItemData->dwFlags & ITEM_HASSOCKETS) ||
        base->pItemData->dwQuality < ITEM_QUALITY_INFERIOR ||
        base->pItemData->dwQuality > ITEM_QUALITY_SUPERIOR || used ||
        sockets == 0 || sockets > kMaxSockets) return false;
    unsigned runes = 0;
    // Native matcher 10918 stops at the first non-positive signed slot;
    // S13 pads unused rune slots with UINT32_MAX, not just zero.
    for (DWORD rune : row.dwRune) {
        if (static_cast<int32_t>(rune) <= 0) break;
        ++runes;
    }
    if (runes != sockets) return false;
    for (WORD type : row.dwEType) {
        if (!type) break;
        if (itemHasType(base, static_cast<int16_t>(type))) return false;
    }
    for (WORD type : row.dwIType) {
        if (!type) break;
        if (itemHasType(base, static_cast<int16_t>(type))) return true;
    }
    return false;
}
json Runeword(const RunesTxt& row, int id, UnitAny* base, DWORD sockets, unsigned used,
              ItemHasType itemHasType) {
    json allowed = json::array(), excluded = json::array(), runeIds = json::array(), codes = json::array();
    D2ItemDataTbl* items = D2COMMON_10535_DATATBLS_GetItemDataTables();
    const bool validItems = Valid(items) && items->nItemsTxtRecordCount > 0 &&
        items->nItemsTxtRecordCount <= static_cast<int>(kMaxRecords) &&
        Readable(items->pItemsTxt, items->nItemsTxtRecordCount * sizeof(ItemsTxt));
    for (WORD type : row.dwIType) allowed.push_back(type);
    for (WORD type : row.dwEType) excluded.push_back(type);
    for (DWORD rune : row.dwRune) {
        runeIds.push_back(rune);
        if (rune && validItems && rune < static_cast<DWORD>(items->nItemsTxtRecordCount))
            codes.push_back(GetItemCode(items->pItemsTxt[rune].szCode));
        else codes.push_back(nullptr);
    }
    json result = {{"id", id}, {"name", Fixed(row.szName, sizeof(row.szName))},
                   {"rune_name", Fixed(row.szRuneName, sizeof(row.szRuneName))},
                   {"complete", row.bComplete != 0}, {"enabled", row.bComplete != 0},
                   {"server_only", row.bServer != 0},
                   {"native_record_id", row.dwRwId}, {"allowed_item_types", std::move(allowed)},
                   {"excluded_item_types", std::move(excluded)}, {"rune_item_ids", std::move(runeIds)},
                   {"rune_codes", std::move(codes)}, {"modifiers", Stats(row.hStats, 7)}};
    if (base) result["base_compatible"] = row.bComplete &&
        BaseCompatible(row, base, sockets, used, itemHasType);
    return result;
}
json CubeCode(WORD itemClass) {
    D2ItemDataTbl* items = D2COMMON_10535_DATATBLS_GetItemDataTables();
    if (!Valid(items) || items->nItemsTxtRecordCount <= 0 ||
        items->nItemsTxtRecordCount > static_cast<int>(kMaxRecords) ||
        itemClass >= items->nItemsTxtRecordCount || !items->pItemsTxt ||
        !Readable(items->pItemsTxt + itemClass, sizeof(ItemsTxt))) return nullptr;
    return GetItemCode(items->pItemsTxt[itemClass].szCode);
}
json CubeType(WORD typeId) {
    if (!Readable(p_D2COMMON_sgptDataTable, sizeof(sgptDataTable*))) return nullptr;
    sgptDataTable* tables = *p_D2COMMON_sgptDataTable;
    if (!Valid(tables) || !tables->dwItemsTypeRecs || tables->dwItemsTypeRecs > kMaxRecords ||
        typeId >= tables->dwItemsTypeRecs || !tables->pItemsTypeTxt ||
        !Readable(tables->pItemsTypeTxt + typeId, sizeof(D2ItemTypesTxt))) return nullptr;
    return GetItemCode(tables->pItemsTypeTxt[typeId].szCode);
}
json CubeItem(const CubeInputItem& input) {
    WORD flags = 0;
    std::memcpy(&flags, &input, sizeof(flags));
    const bool isType = (flags & 2) != 0;
    return {{"flags", flags}, {"item", input.Item},
            {"item_type", isType ? json(input.Item) : json(nullptr)},
            {"item_type_code", isType ? CubeType(input.Item) : json(nullptr)},
            {"item_code", !isType && (flags & 1) ? CubeCode(input.Item) : json(nullptr)},
            {"item_id", input.ItemID}, {"quality", input.Quality}, {"quantity", input.Quantity}};
}
json CubeItem(const CubeOutputItem& output) {
    json prefixes = json::array(), suffixes = json::array(), mods = json::array();
    WORD flags = 0;
    std::memcpy(&flags, &output, sizeof(flags));
    for (WORD n : output.PrefixId) prefixes.push_back(n);
    for (WORD n : output.SuffixId) suffixes.push_back(n);
    for (const auto& mod : output.sMods)
        mods.push_back({{"property", mod.dwMod}, {"parameter", mod.wModParam},
                        {"min", mod.wModMin}, {"max", mod.wModMax}, {"chance", mod.bModChance}});
    return {{"flags", flags}, {"item", output.Item},
            {"item_type", output.Type == 253 ? json(output.Item) : json(nullptr)},
            {"item_type_code", output.Type == 253 ? CubeType(output.Item) : json(nullptr)},
            {"item_code", output.Type == 252 ? CubeCode(output.Item) : json(nullptr)},
            {"item_id", output.ItemID}, {"parameter", output.nParam},
            {"quality", output.nQuality}, {"quantity", output.nQuantity},
            {"type", output.Type}, {"level", output.Lvl}, {"player_level", output.PLvl},
            {"item_level", output.ILvl}, {"prefix_ids", std::move(prefixes)},
            {"suffix_ids", std::move(suffixes)}, {"modifiers", std::move(mods)}};
}

struct Context { UnitAny* player; Inventory* inventory; };
Context Current() {
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    return {player, player && player->dwType == UNIT_PLAYER && Valid(player->pInventory) &&
            player->pInventory->pOwner == player ? player->pInventory : nullptr};
}
UnitAny* Owned(Context ctx, DWORD id) {
    if (!ctx.inventory) return nullptr;
    unsigned seen = 0;
    for (UnitAny* item = D2COMMON_GetItemFromInventory(ctx.inventory);
         item && seen++ < 8192; item = D2COMMON_GetNextItemFromInventory(item)) {
        if (!Valid(item) || !Valid(item->pItemData)) return nullptr;
        if (item->dwType == UNIT_ITEM && item->dwUnitId == id &&
            item->pItemData->pOwnerInventory == ctx.inventory) return item;
    }
    return nullptr;
}
UnitAny* Cursor(Context ctx) {
    if (!ctx.inventory || !Valid(ctx.inventory->pCursorItem)) return nullptr;
    UnitAny* item = D2CLIENT_GetCursorItem();
    return item && item == ctx.inventory->pCursorItem && Valid(item->pItemData) &&
        item->dwType == UNIT_ITEM && item->dwMode == ITEM_MODE_ON_CURSOR &&
        item->pItemData->pOwnerInventory == ctx.inventory ? item : nullptr;
}

// Walk the same first/next pointers as S13 D2Common -10918. That engine
// accessor evaluates children in precisely this order when matching runes.
bool Children(UnitAny* item, json& rows, unsigned& count) {
    rows = json::array();
    count = 0;
    if (!item->pInventory) return true;
    if (!Valid(item->pInventory) || item->pInventory->dwSignature != 0x01020304 ||
        item->pInventory->pOwner != item) return false;
    for (UnitAny* child = item->pInventory->pFirstItem; child;) {
        if (count == kMaxSockets || !Valid(child) || child->dwType != UNIT_ITEM ||
            child->dwMode != ITEM_MODE_SOCKETED_IN_ITEM || !Valid(child->pItemData) ||
            child->pItemData->pOwnerInventory != item->pInventory) return false;
        // Socket ingredients cannot themselves own nested item inventories.
        // Do not recurse through WorldState::DescribeItem while it includes
        // this readback for all items.
        if (child->pInventory) return false;
        ItemsTxt* text = D2COMMON_GetItemText(child->dwTxtFileNo);
        if (!Valid(text)) return false;
        rows.push_back({{"ok", true}, {"id", child->dwUnitId},
                        {"class_id", child->dwTxtFileNo}, {"code", GetItemCode(text->szCode)},
                        {"quality", child->pItemData->dwQuality},
                        {"file_index", child->pItemData->dwFileIndex},
                        {"flags", child->pItemData->dwFlags},
                        {"mode", child->dwMode}, {"location", "socket"},
                        {"owner_item_id", item->dwUnitId}, {"insertion_index", count}});
        ++count;
        child = child->pItemData->pNextInvItem;
    }
    return true;
}
bool CubeBox(UnitAny* item) {
    if (!Valid(item) || !Valid(item->pItemData)) return false;
    ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
    // The cube uses a page of the player's inventory, not a socket inventory.
    return Valid(text) && std::memcmp(text->szCode, "box", 3) == 0 &&
        item->dwMode == ITEM_MODE_INV_STASH_CUBE_STORE &&
        item->pItemData->NodePage == NODEPAGE_STORAGE &&
        item->pItemData->ItemLocation == STORAGE_INVENTORY;
}
UnitAny* FindCube(Context ctx) {
    if (!ctx.inventory) return nullptr;
    UnitAny* box = nullptr;
    unsigned seen = 0;
    for (UnitAny* item = D2COMMON_GetItemFromInventory(ctx.inventory);
         item && seen++ < 8192; item = D2COMMON_GetNextItemFromInventory(item)) {
        if (!Valid(item) || !Valid(item->pItemData)) return nullptr;
        if (item->pItemData->pOwnerInventory != ctx.inventory || !CubeBox(item)) continue;
        if (box) return nullptr; // ambiguous cube identity
        box = item;
    }
    return box;
}
} // namespace

namespace ItemCrafting {
json Catalog(const json& args) {
    if (!args.is_object() || !args.contains("table") || !args.at("table").is_string())
        return Failure("table must be runewords or cube_recipes");
    const std::string table = args.at("table").get<std::string>();
    if (table != "runewords" && table != "cube_recipes") return Failure("unknown crafting table");
    DWORD offset = 0, limit = 50;
    if ((args.contains("offset") && !U32(args, "offset", offset)) ||
        (args.contains("limit") && !U32(args, "limit", limit)) || !limit || limit > 100)
        return Failure("offset must be unsigned and limit must be 1..100");
    if (table != "runewords" && args.contains("base_item_id"))
        return Failure("base_item_id is supported only for runewords");
    json entries = json::array();
    int total = 0;
    if (table == "runewords") {
        RunesTxt* first = nullptr;
        if (!RuneTable(total, first)) return Failure("S13 loaded runeword table unavailable");
        UnitAny* base = nullptr;
        DWORD sockets = 0;
        unsigned used = 0;
        ItemHasType itemHasType = nullptr;
        if (args.contains("base_item_id")) {
            DWORD id;
            if (!U32(args, "base_item_id", id))
                return Failure("base_item_id must be an unsigned 32-bit integer");
            Context ctx = Current();
            if (!IsGameReady() || !ctx.inventory) return Failure("player inventory unavailable");
            base = Owned(ctx, id);
            if (!base || !Valid(base->pItemData) ||
                !((base->dwMode == ITEM_MODE_INV_STASH_CUBE_STORE &&
                   base->pItemData->NodePage == NODEPAGE_STORAGE &&
                   (base->pItemData->ItemLocation == STORAGE_INVENTORY ||
                    (base->pItemData->ItemLocation == STORAGE_STASH && D2CLIENT_GetUIState(UI_STASH)) ||
                    (base->pItemData->ItemLocation == STORAGE_CUBE && D2CLIENT_GetUIState(UI_CUBE)))) ||
                  (base->dwMode == ITEM_MODE_EQUIPPED && base->pItemData->NodePage == NODEPAGE_EQUIP)))
                return Failure("base item must be owned in visible storage or equipment");
            sockets = D2COMMON_GetUnitStat(base, STAT_SOCKETS, 0);
            json children;
            if (sockets > kMaxSockets || !Children(base, children, used) || used > sockets)
                return Failure("base socket state is unreadable");
            itemHasType = NativeItemHasType();
            if (!itemHasType) return Failure("native S13 item-type checker unavailable");
        }
        for (unsigned i = offset; i < static_cast<unsigned>(total) && i - offset < limit; ++i)
            entries.push_back(Runeword(first[i], i, base, sockets, used, itemHasType));
    } else {
        CubeCount count = nullptr;
        CubeRecord record = nullptr;
        if (!CubeExports(count, record) || (total = count()) <= 0 || total > static_cast<int>(kMaxRecords))
            return Failure("S13 loaded cube recipe table unavailable");
        for (unsigned i = offset; i < static_cast<unsigned>(total) && i - offset < limit; ++i) {
            CubeMainTxt* row = record(i);
            if (!Valid(row)) return Failure("cube recipe page is unreadable");
            json inputs = json::array(), outputs = json::array();
            for (const auto& input : row->InputItem) inputs.push_back(CubeItem(input));
            for (const auto& output : row->OutputItem) outputs.push_back(CubeItem(output));
            entries.push_back({{"id", i}, {"enabled", row->bEnabled != 0},
                {"ladder", row->bLadder != 0}, {"minimum_difficulty", row->bMindiff},
                {"class", row->bClass}, {"operation", row->bOp},
                {"operation_extra", {row->_1[0], row->_1[1], row->_1[2]}},
                {"parameter", row->dwParam}, {"value", row->dwValue},
                {"number_inputs", row->bNuminputs}, {"version", row->wVersion},
                {"inputs", std::move(inputs)}, {"outputs", std::move(outputs)}});
        }
    }
    return {{"ok", true}, {"table", table}, {"offset", offset},
            {"total", total}, {"entries", std::move(entries)}};
}

json DescribeSockets(UnitAny* item) {
    json result = {{"socket_contents_complete", false}, {"socket_children", json::array()},
                   {"runeword", {{"active", false}, {"id", nullptr}, {"name", nullptr}}}};
    if (!Valid(item) || item->dwType != UNIT_ITEM || !Valid(item->pItemData)) return result;
    const bool active = (item->pItemData->dwFlags & ITEM_RUNEWORD) != 0;
    result["runeword"]["active"] = active;
    const int sockets = static_cast<int>(D2COMMON_GetUnitStat(item, STAT_SOCKETS, 0));
    if (sockets < 0 || sockets > static_cast<int>(kMaxSockets)) return result;
    unsigned childrenCount = 0;
    json children;
    if (!Children(item, children, childrenCount) || childrenCount > static_cast<unsigned>(sockets)) return result;
    result["socket_children"] = std::move(children);
    result["socket_contents_complete"] = true;
    if (active && childrenCount > 0 && childrenCount == static_cast<unsigned>(sockets)) {
        RunesTxt* first = nullptr;
        int total = 0;
        if (RuneTable(total, first)) {
            RunesTxt* record = D2COMMON_ITEMS_GetRunesTxtRecordFromItem(item);
            const uintptr_t address = reinterpret_cast<uintptr_t>(record);
            const uintptr_t start = reinterpret_cast<uintptr_t>(first);
            if (address >= start && address - start < static_cast<size_t>(total) * sizeof(RunesTxt) &&
                (address - start) % sizeof(RunesTxt) == 0) {
                const int index = static_cast<int>((address - start) / sizeof(RunesTxt));
                result["runeword"]["id"] = index;
                result["runeword"]["name"] = Fixed(first[index].szName, sizeof(first[index].szName));
            }
        }
    }
    return result;
}

json Socket(const json& args) {
    DWORD socketableId, targetId;
    if (!U32(args, "socketable_id", socketableId) || !U32(args, "target_id", targetId) ||
        socketableId == targetId) return Failure("distinct unsigned socketable_id and target_id are required");
    Context ctx = Current();
    if (!IsGameReady() || !ctx.inventory) return Failure("player inventory unavailable");
    UnitAny* ingredient = Cursor(ctx);
    if (!ingredient || ingredient->dwUnitId != socketableId)
        return Failure("gem or rune is not on the active player's cursor");
    UnitAny* target = Owned(ctx, targetId);
    if (!target || !Valid(target->pItemData) ||
        !((target->dwMode == ITEM_MODE_INV_STASH_CUBE_STORE &&
           target->pItemData->NodePage == NODEPAGE_STORAGE &&
           (target->pItemData->ItemLocation == STORAGE_INVENTORY ||
            (target->pItemData->ItemLocation == STORAGE_CUBE && D2CLIENT_GetUIState(UI_CUBE)) ||
            (target->pItemData->ItemLocation == STORAGE_STASH && D2CLIENT_GetUIState(UI_STASH)))) ||
          (target->dwMode == ITEM_MODE_EQUIPPED && target->pItemData->NodePage == NODEPAGE_EQUIP)))
        return Failure("target must be owned equipment or visible player storage");
    ItemsTxt* sourceText = D2COMMON_GetItemText(ingredient->dwTxtFileNo);
    if (!Valid(sourceText)) return Failure("ingredient item record unavailable");
    // ItemTypes.txt is the current-season loaded classification; nGem also
    // covers mod-added gems without treating stackable rune types as sockets.
    sgptDataTable* all = Valid(p_D2COMMON_sgptDataTable) ? *p_D2COMMON_sgptDataTable : nullptr;
    if (!Valid(all) || !all->dwItemsTypeRecs || all->dwItemsTypeRecs > kMaxRecords ||
        !Readable(all->pItemsTypeTxt, all->dwItemsTypeRecs * sizeof(D2ItemTypesTxt)))
        return Failure("loaded item-type table unavailable");
    WORD queue[64] = {sourceText->nType};
    unsigned count = 1;
    bool gem = false, rune = false;
    for (unsigned at = 0; at < count; ++at) {
        const WORD type = queue[at];
        if (type >= all->dwItemsTypeRecs) return Failure("ingredient item type is invalid");
        const D2ItemTypesTxt& row = all->pItemsTypeTxt[type];
        gem = gem || row.nGem != 0;
        rune = rune || type == ITEM_TYPE_RUNE;
        for (WORD parent : {row.nEquiv1, row.nEquiv2}) {
            if (!parent) continue;
            bool found = false;
            for (unsigned k = 0; k < count; ++k) found = found || queue[k] == parent;
            if (!found) {
                if (count == 64) return Failure("ingredient type ancestry exceeds bound");
                queue[count++] = parent;
            }
        }
    }
    if (!gem && !rune) return Failure("cursor item is not a socketable gem or rune");
    if (sourceText->bstackable || sourceText->bhasinv || ingredient->pInventory)
        return Failure("stacked or container items cannot be inserted directly");
    const DWORD sockets = D2COMMON_GetUnitStat(target, STAT_SOCKETS, 0);
    if (!sockets || sockets > kMaxSockets || !(target->pItemData->dwFlags & ITEM_HASSOCKETS))
        return Failure("target has no supported sockets");
    json children;
    unsigned used = 0;
    if (!Children(target, children, used) || used >= sockets)
        return Failure("target sockets are full or their contents are unreadable");
    BYTE packet[9] = {0x28};
    Put32(packet + 1, socketableId);
    Put32(packet + 5, targetId);
    D2NET_SendPacket(sizeof(packet), 0, packet);
    return {{"ok", true}, {"status", "requested"}, {"socketable_id", socketableId},
            {"target_id", targetId}, {"prior_socket_count", used}};
}

json Cube(const json& args) {
    if (!args.is_object() || args.size() != 1 || !args.contains("action") ||
        !args.at("action").is_string()) return Failure("action must be open, close, or transmute");
    const std::string action = args.at("action").get<std::string>();
    if (action != "open" && action != "close" && action != "transmute")
        return Failure("action must be open, close, or transmute");
    Context ctx = Current();
    if (!IsGameReady() || !ctx.inventory) return Failure("player inventory unavailable");
    UnitAny* box = FindCube(ctx);
    if (!box) return Failure("a unique owned Horadric Cube in inventory is required");
    if (ctx.inventory->pCursorItem || D2CLIENT_GetCursorItem()) return Failure("cursor must be empty");
    const bool open = D2CLIENT_GetUIState(UI_CUBE) != 0;
    if (action == "open") {
        if (open) return Failure("cube is already open");
        if (!ctx.player->pPath) return Failure("player position unavailable");
        // 0x20 / GUID / player X / player Y: the server invokes the native
        // item's pSpell and responds with the cube-open UI packet.
        BYTE packet[13] = {0x20};
        Put32(packet + 1, box->dwUnitId);
        Put32(packet + 5, static_cast<DWORD>(ctx.player->pPath->xPos));
        Put32(packet + 9, static_cast<DWORD>(ctx.player->pPath->yPos));
        D2NET_SendPacket(sizeof(packet), 0, packet);
    } else if (action == "close") {
        if (!open) return Failure("cube is not open");
        // SetUIVar closes via the native panel-specific cleanup callback.
        D2CLIENT_SetUIVar(UI_CUBE, 1, 0);
    } else {
        if (!open) return Failure("cube must be open before transmutation");
        // Exact S13 native callback sends 4F/button24/param0. The inherited
        // 595C0 binding instead sends trade green-check (button7).
        const BYTE* client = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2Client.dll"));
        constexpr BYTE signature[] = {
            0x83, 0xEC, 0x08, 0x33, 0xC0, 0x53, 0x66, 0x89, 0x44, 0x24, 0x07,
            0x66, 0x89, 0x44, 0x24, 0x09, 0x8D, 0x44, 0x24, 0x04, 0x50, 0xBB,
            0x07, 0x00, 0x00, 0x00, 0xC6, 0x44, 0x24, 0x08, 0x4F, 0x66, 0xC7,
            0x44, 0x24, 0x09, 0x18, 0x00, 0xE8, 0x25, 0x78, 0xF8, 0xFF};
        if (!client || !Readable(client + 0x8CB90, sizeof(signature)) ||
            std::memcmp(client + 0x8CB90, signature, sizeof(signature)))
            return Failure("S13 native cube transmute callback unavailable");
        // Recipe matching, consumption and output creation remain native.
        D2CLIENT_Transmute();
    }
    return {{"ok", true}, {"status", "requested"}, {"action", action}, {"cube_id", box->dwUnitId}};
}
} // namespace ItemCrafting
