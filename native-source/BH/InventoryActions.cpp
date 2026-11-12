#include "InventoryActions.h"

#include "Constants.h"
#include "D2Helpers.h"
#include "D2Ptrs.h"

#include <cstdint>
#include <string>

namespace {
using json = nlohmann::json;

json Failure(const char* reason) { return {{"ok", false}, {"error", reason}}; }

bool Unsigned(const json& args, const char* name, DWORD& value) {
    if (!args.is_object() || !args.contains(name) || !args[name].is_number_integer()) return false;
    if (args[name].is_number_unsigned()) {
        const auto n = args[name].get<uint64_t>();
        if (n > UINT32_MAX) return false;
        value = static_cast<DWORD>(n);
        return true;
    }
    const auto n = args[name].get<int64_t>();
    if (n < 0 || n > UINT32_MAX) return false;
    value = static_cast<DWORD>(n);
    return true;
}

void Put32(BYTE* packet, size_t offset, DWORD value) {
    packet[offset] = static_cast<BYTE>(value);
    packet[offset + 1] = static_cast<BYTE>(value >> 8);
    packet[offset + 2] = static_cast<BYTE>(value >> 16);
    packet[offset + 3] = static_cast<BYTE>(value >> 24);
}

void Send5(BYTE opcode, DWORD id) {
    BYTE packet[5] = {opcode};
    Put32(packet, 1, id);
    D2NET_SendPacket(sizeof(packet), 0, packet);
}

// Exact classic C2S item opcodes and wire widths: Diablo2PacketsData,
// src/data/1.14d/client2gs.json; Kolbot Packet.js / Storage.js corroborate
// 0x16, 0x17, 0x18, 0x19 and buffer 0/4. Native S13 cursor, equipment,
// inventory and visible-stash transitions plus save/reload are verified.
void ToBuffer(DWORD id, DWORD x, DWORD y, DWORD buffer) {
    BYTE packet[17] = {0x18};
    Put32(packet, 1, id);
    Put32(packet, 5, x);
    Put32(packet, 9, y);
    Put32(packet, 13, buffer);
    D2NET_SendPacket(sizeof(packet), 0, packet);
}

void ToBody(DWORD id, DWORD slot) {
    BYTE packet[9] = {0x1A};
    Put32(packet, 1, id);
    Put32(packet, 5, slot);
    D2NET_SendPacket(sizeof(packet), 0, packet);
}

struct Context {
    UnitAny* player;
    Inventory* inventory;
};

Context Current() {
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    return {player, player && player->dwType == UNIT_PLAYER ? player->pInventory : nullptr};
}

// Never trust a client-side GUID lookup alone: prove membership in *our*
// inventory, then check item mode and its owner-inventory backpointer.
UnitAny* Owned(Context ctx, DWORD id) {
    if (!ctx.inventory) return nullptr;
    for (UnitAny* item = D2COMMON_GetItemFromInventory(ctx.inventory); item;
         item = D2COMMON_GetNextItemFromInventory(item)) {
        if (item->dwType == UNIT_ITEM && item->dwUnitId == id && item->pItemData &&
            item->pItemData->pOwnerInventory == ctx.inventory) return item;
    }
    return nullptr;
}

UnitAny* Cursor(Context ctx) {
    if (!ctx.inventory) return nullptr;
    UnitAny* item = D2CLIENT_GetCursorItem();
    if (!item || item->dwType != UNIT_ITEM || item->dwMode != ITEM_MODE_ON_CURSOR ||
        item->pItemData == nullptr || item->pItemData->pOwnerInventory != ctx.inventory ||
        item->dwUnitId != (ctx.inventory->pCursorItem ? ctx.inventory->pCursorItem->dwUnitId : 0)) return nullptr;
    return item;
}

// This is deliberately one action per request. A cursor transition must be
// observed by the caller before placement; never submit dependent packets
// speculatively or claim server acceptance from transport acceptance.
json PickupOwned(Context ctx, DWORD id, unsigned allowedStorage) {
    UnitAny* item = Owned(ctx, id);
    if (!item) return Failure("item is not owned by the active player");
    if (Cursor(ctx) || ctx.inventory->pCursorItem) return Failure("cursor is occupied");
    if (item->dwMode == ITEM_MODE_INV_STASH_CUBE_STORE && item->pItemData->NodePage == NODEPAGE_STORAGE) {
        const unsigned storage = item->pItemData->ItemLocation;
        if (storage == STORAGE_STASH &&
            (allowedStorage != STORAGE_STASH || !D2CLIENT_GetUIState(UI_STASH)))
            return Failure("stash retrieval requires the visible stash page");
        if (storage == STORAGE_CUBE &&
            (allowedStorage != STORAGE_CUBE || !D2CLIENT_GetUIState(UI_CUBE)))
            return Failure("cube retrieval requires the owned cube open");
        if (storage != STORAGE_INVENTORY && storage != allowedStorage)
            return Failure("item is not in the requested owned storage");
        Send5(0x19, id);
    } else if (item->dwMode == ITEM_MODE_EQUIPPED && item->pItemData->NodePage == NODEPAGE_EQUIP) {
        const DWORD slot = item->pItemData->BodyLocation;
        if (slot < EQUIP_HEAD || slot > EQUIP_LEFT_SECONDARY) return Failure("invalid equipped body slot");
        BYTE packet[3] = {0x1C, static_cast<BYTE>(slot), 0};
        D2NET_SendPacket(sizeof(packet), 0, packet);
    } else {
        return Failure("item is not in supported inventory, visible stash, or equipment location");
    }
    return {{"ok", true}, {"status", "requested"}, {"item_id", id}, {"destination", "cursor"}};
}

// Use allocated native inventory stores, not dormant drawing-layout globals.
bool Fits(Context ctx, UnitAny* item, DWORD x, DWORD y, unsigned storage, const char*& error) {
    unsigned width = 0, height = 0;
    if (!InventoryActions::GridSize(ctx.inventory, storage, width, height)) {
        error = "inventory grid dimensions unavailable or invalid";
        return false;
    }
    ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
    if (!text || !text->binvwidth || !text->binvheight ||
        x > width || y > height ||
        text->binvwidth > width - x || text->binvheight > height - y) {
        error = "item does not fit inside target grid";
        return false;
    }
    for (UnitAny* other = D2COMMON_GetItemFromInventory(ctx.inventory); other;
         other = D2COMMON_GetNextItemFromInventory(other)) {
        if (!other->pItemData || other->dwType != UNIT_ITEM ||
            other->dwMode != ITEM_MODE_INV_STASH_CUBE_STORE ||
            other->pItemData->NodePage != NODEPAGE_STORAGE ||
            other->pItemData->ItemLocation != storage) continue;
        if (other->dwUnitId == item->dwUnitId) continue;
        if (!other->pItemPath) { error = "target grid has an item with unknown position"; return false; }
        ItemsTxt* occupied = D2COMMON_GetItemText(other->dwTxtFileNo);
        if (!occupied || !occupied->binvwidth || !occupied->binvheight) {
            error = "target grid has an item with unknown dimensions";
            return false;
        }
        const DWORD ox = other->pItemPath->dwPosX, oy = other->pItemPath->dwPosY;
        if (ox < x + text->binvwidth && x < ox + occupied->binvwidth &&
            oy < y + text->binvheight && y < oy + occupied->binvheight) {
            error = "target cells are occupied";
            return false;
        }
    }
    return true;
}

json Place(Context ctx, const json& args, unsigned storage) {
    DWORD id, x, y;
    if (!Unsigned(args, "item_id", id) || !Unsigned(args, "x", x) || !Unsigned(args, "y", y))
        return Failure("item_id, x and y must be unsigned 32-bit integers");
    UnitAny* item = Cursor(ctx);
    if (!item || item->dwUnitId != id) return Failure("specified item is not on the active player's cursor");
    if (storage == STORAGE_STASH && !D2CLIENT_GetUIState(UI_STASH)) return Failure("stash must be open");
    if (storage == STORAGE_CUBE && !D2CLIENT_GetUIState(UI_CUBE)) return Failure("cube must be open");
    const char* error = nullptr;
    if (!Fits(ctx, item, x, y, storage, error)) return Failure(error);
    ToBuffer(id, x, y, storage);
    return {{"ok", true}, {"status", "requested"}, {"item_id", id},
            {"destination", storage == STORAGE_STASH ? "current_stash_page" :
                storage == STORAGE_CUBE ? "cube" : "inventory"}, {"x", x}, {"y", y}};
}
// Historical S12 class IDs are not S13 identity evidence: check object text.
bool StashName(const char* text, size_t length) {
    if (!text) return false;
    for (size_t i = 0; i + 5 <= length && text[i]; ++i) {
        size_t n = 0;
        for (; n < 5; ++n) {
            char c = text[i + n];
            if (!c || (c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c) != "stash"[n]) break;
        }
        if (n == 5) return true;
    }
    return false;
}

bool StashObject(UnitAny* object, UnitAny* player) {
    if (!object || object->dwType != UNIT_OBJECT || !object->pObjectPath ||
        !object->pObjectPath->pRoom1 || !object->pObjectPath->pRoom1->pRoom2 ||
        !object->pObjectPath->pRoom1->pRoom2->pLevel) return false;
    if (object->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo !=
        player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo) return false;
    ObjectTxt* text = D2COMMON_GetObjectText(object->dwTxtFileNo);
    if (!text) return false;
    if (StashName(text->szName, sizeof(text->szName))) return true;
    const wchar_t* name = text->wszName;
    for (size_t i = 0; i + 5 < sizeof(text->wszName) / sizeof(wchar_t) && name[i]; ++i) {
        size_t n = 0;
        for (; n < 5; ++n) {
            wchar_t c = name[i + n];
            if (!c || (c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c) != L"stash"[n]) break;
        }
        if (n == 5) return true;
    }
    return false;
}

// Historical bf812ac found stash objects absent from room unit lists.
UnitAny* FindStash(UnitAny* player, DWORD requestedId, bool explicitId, bool& ambiguous) {
    if (explicitId) {
        UnitAny* object = D2CLIENT_FindServerSideUnit(requestedId, UNIT_OBJECT);
        return object && object->dwUnitId == requestedId && StashObject(object, player) ? object : nullptr;
    }
    UnitAny** table = reinterpret_cast<UnitAny**>(*Var_D2CLIENT_pUnitTable());
    if (!table) return nullptr;
    UnitAny* closest = nullptr;
    int closestDistance = 7;
    for (int bucket = 0; bucket < 128; ++bucket) {
        int count = 0;
        for (UnitAny* object = table[UNIT_OBJECT * 128 + bucket]; object && count++ < 2048;
             object = object->pListNext) {
            if (!StashObject(object, player)) continue;
            const int dx = D2CLIENT_GetUnitX(object) - player->pPath->xPos;
            const int dy = D2CLIENT_GetUnitY(object) - player->pPath->yPos;
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            const int distance = ax > ay ? ax : ay;
            if (distance < closestDistance) {
                closestDistance = distance;
                closest = object;
                ambiguous = false;
            } else if (distance == closestDistance && closest && closest->dwUnitId != object->dwUnitId) {
                ambiguous = true;
            }
        }
    }
    return closest;
}

}

namespace InventoryActions {
bool GridSize(Inventory* inventory, unsigned storage, unsigned& width, unsigned& height) {
    if (storage != STORAGE_INVENTORY && storage != STORAGE_CUBE && storage != STORAGE_STASH) return false;
    if (!inventory || !inventory->pStores || inventory->dwStoresCount > 32) return false;
    const unsigned index = 2 + storage;
    if (index < inventory->dwStoresCount) {
        const InventoryStore& store = inventory->pStores[index];
        width = store.Width;
        height = store.Height;
        if (width || height) return width > 0 && height > 0 && width <= 32 && height <= 32;
    }
    if ((storage == STORAGE_STASH && D2CLIENT_GetUIState(UI_STASH)) ||
        (storage == STORAGE_CUBE && D2CLIENT_GetUIState(UI_CUBE))) {
        // Empty storage may have no allocated grid. Read loaded Inventory.txt,
        // never infer dimensions: S13 Client 90B78 selects bank12 and 90B9E
        // selects cube9 (the latter populates the native CubeLayout).
        InventoryLayout layout = {};
        D2COMMON_10760_DATATBLS_GetInventoryGridInfo(storage == STORAGE_CUBE ? 9 : 12, 0, &layout);
        width = layout.SlotWidth;
        height = layout.SlotHeight;
    } else return false;
    return width > 0 && height > 0 && width <= 32 && height <= 32;
}
json OpenStash(const json& args) {
    if (!args.is_object()) return Failure("arguments must be an object");
    const bool explicitId = args.contains("object_id");
    DWORD id = 0;
    if (explicitId && !Unsigned(args, "object_id", id))
        return Failure("object_id must be an unsigned 32-bit object GUID");
    Context ctx = Current();
    if (!IsGameReady() || !ctx.inventory || !ctx.player->pPath ||
        !ctx.player->pPath->pRoom1 || !ctx.player->pPath->pRoom1->pRoom2 ||
        !ctx.player->pPath->pRoom1->pRoom2->pLevel)
        return Failure("player town position unavailable");
    if (!IsTown(ctx.player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo))
        return Failure("stash interaction requires a town");
    if (D2CLIENT_GetUIState(UI_STASH)) return Failure("stash is already open");
    if (ctx.inventory->pCursorItem) return Failure("cursor is occupied");
    bool ambiguous = false;
    UnitAny* object = FindStash(ctx.player, id, explicitId, ambiguous);
    if (ambiguous) return Failure("multiple equally near stash objects; specify object_id");
    if (!object) return Failure("no validated stash object found; specify a live object_id");
    const int dx = D2CLIENT_GetUnitX(object) - ctx.player->pPath->xPos;
    const int dy = D2CLIENT_GetUnitY(object) - ctx.player->pPath->yPos;
    if (dx < -6 || dx > 6 || dy < -6 || dy > 6)
        return Failure("stash object is not within six tiles; move closer first");
    BYTE packet[9] = {0x13};
    Put32(packet, 1, UNIT_OBJECT);
    Put32(packet, 5, object->dwUnitId);
    // D2Helpers::Interact sends the same native C2S opcode and flag 1.
    D2NET_SendPacket(sizeof(packet), 1, packet);
    return {{"ok", true}, {"status", "requested"}, {"object_id", object->dwUnitId},
            {"destination", "stash_panel"}};
}

json CloseStash(const json& args) {
    if (!args.is_object() || !args.empty()) return Failure("CloseStash takes no arguments");
    Context ctx = Current();
    if (!IsGameReady() || !ctx.inventory) return Failure("player inventory unavailable");
    if (!D2CLIENT_GetUIState(UI_STASH)) return Failure("stash is not open");
    if (ctx.inventory->pCursorItem) return Failure("place or drop cursor item before closing stash");
    // Native panel-specific closing dispatch performs stash interaction cleanup.
    D2CLIENT_SetUIVar(UI_STASH, 1, 0);
    return {{"ok", true}, {"status", "requested"}, {"destination", "stash_panel_closed"}};
}

json Pickup(const json& args) {
    DWORD id;
    if (!Unsigned(args, "item_id", id)) return Failure("item_id must be an unsigned 32-bit integer");
    Context ctx = Current();
    if (!ctx.inventory) return Failure("player inventory unavailable");
    if (Cursor(ctx) || ctx.inventory->pCursorItem) return Failure("cursor is occupied");
    UnitAny* item = D2CLIENT_FindServerSideUnit(id, UNIT_ITEM);
    if (!item || item->dwUnitId != id || item->dwType != UNIT_ITEM ||
        item->dwMode != ITEM_MODE_ON_GROUND || !item->pItemData || !item->pItemPath ||
        item->pItemData->pOwnerInventory) return Failure("item is not a live ground item");
    if (!ctx.player->pAct || !ctx.player->pPath)
        return Failure("player world position unavailable");
    Room1* room = D2COMMON_GetRoomFromUnit(item);
    if (!room || !room->pRoom2 ||
        !room->pRoom2->pLevel || !ctx.player->pPath->pRoom1 ||
        !ctx.player->pPath->pRoom1->pRoom2 || !ctx.player->pPath->pRoom1->pRoom2->pLevel ||
        room->pRoom2->pLevel->dwLevelNo !=
        ctx.player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo)
        return Failure("ground item is not in the active player's level");
    BYTE packet[13] = {0x16};
    Put32(packet, 1, UNIT_ITEM);
    Put32(packet, 5, id);
    Put32(packet, 9, 1); // 1 = pickup onto cursor; avoid guessing automatic inventory space.
    D2NET_SendPacket(sizeof(packet), 0, packet);
    return {{"ok", true}, {"status", "requested"}, {"item_id", id}, {"destination", "cursor"}};
}

json Drop(const json& args) {
    DWORD id;
    if (!Unsigned(args, "item_id", id)) return Failure("item_id must be an unsigned 32-bit integer");
    Context ctx = Current();
    if (!ctx.inventory) return Failure("player inventory unavailable");
    UnitAny* item = Cursor(ctx);
    if (!item || item->dwUnitId != id) return Failure("specified item is not on the active player's cursor");
    Send5(0x17, id);
    return {{"ok", true}, {"status", "requested"}, {"item_id", id}, {"destination", "ground"}};
}

json Move(const json& args) {
    Context ctx = Current();
    if (!ctx.inventory) return Failure("player inventory unavailable");
    if (!args.is_object() || !args.contains("destination") || !args["destination"].is_string())
        return Failure("destination must be cursor, inventory, or equipment");
    const std::string destination = args["destination"].get<std::string>();
    if (destination == "cursor") {
        DWORD id;
        if (!Unsigned(args, "item_id", id)) return Failure("item_id must be an unsigned 32-bit integer");
        return PickupOwned(ctx, id, STORAGE_INVENTORY);
    }
    if (destination == "inventory") return Place(ctx, args, STORAGE_INVENTORY);
    if (destination == "equipment") {
        DWORD id, slot;
        if (!Unsigned(args, "item_id", id) || !Unsigned(args, "slot", slot) ||
            slot < EQUIP_HEAD || slot > EQUIP_LEFT_SECONDARY)
            return Failure("item_id and valid equipment slot are required");
        UnitAny* item = Cursor(ctx);
        if (!item || item->dwUnitId != id) return Failure("specified item is not on the active player's cursor");
        if (!D2COMMON_ItemCanBeEquipped(item)) return Failure("item cannot be equipped");
        BOOL strength = FALSE, dexterity = FALSE, level = FALSE;
        if (!D2COMMON_10244_CheckRequirements(item, ctx.player, TRUE, &strength, &dexterity, &level))
            return Failure("item requirements are not met");
        if (D2COMMON_GetItemByBodyLoc(ctx.inventory, static_cast<int>(slot)))
            return Failure("equipment slot is occupied; swaps are not supported");
        // Slot compatibility belongs to the native server equipment handler.
        // ItemGetBodyLocation reports the current location, not an allowed slot.
        ToBody(id, slot);
        return {{"ok", true}, {"status", "requested"}, {"item_id", id},
                {"destination", "equipment"}, {"slot", slot}};
    }
    return Failure("destination must be cursor, inventory, or equipment");
}

json Stash(const json& args) {
    // PD2 S13 tab-selection/persistence has no verified SDK binding here.
    // Explicitly scope this action to the already selected visible page;
    // never assert a personal/shared tab number based on S12 offsets.
    if (!args.is_object() || !args.contains("tab") || !args["tab"].is_string() ||
        args["tab"].get<std::string>() != "current")
        return Failure("only the currently visible stash page is supported; tab must be current");
    Context ctx = Current();
    if (!ctx.inventory) return Failure("player inventory unavailable");
    if (!D2CLIENT_GetUIState(UI_STASH)) return Failure("stash must be open");
    if (!args.contains("direction") || !args["direction"].is_string())
        return Failure("direction must be deposit or retrieve");
    const std::string direction = args["direction"].get<std::string>();
    if (direction == "deposit") {
        json result = Place(ctx, args, STORAGE_STASH);
        if (result.value("ok", false)) result["tab"] = "current";
        return result;
    }
    if (direction == "retrieve") {
        DWORD id;
        if (!Unsigned(args, "item_id", id)) return Failure("item_id must be an unsigned 32-bit integer");
        UnitAny* item = Owned(ctx, id);
        if (!item || !item->pItemData || item->dwMode != ITEM_MODE_INV_STASH_CUBE_STORE ||
            item->pItemData->NodePage != NODEPAGE_STORAGE ||
            item->pItemData->ItemLocation != STORAGE_STASH)
            return Failure("item is not in the active player's visible stash inventory");
        json result = PickupOwned(ctx, id, STORAGE_STASH);
        if (result.value("ok", false)) result["tab"] = "current";
        return result;
    }
    return Failure("direction must be deposit or retrieve");
}

json CubeMove(const json& args) {
    DWORD id;
    if (!args.is_object() || !Unsigned(args, "item_id", id) || !args.contains("direction") ||
        !args["direction"].is_string())
        return Failure("item_id and direction deposit or retrieve are required");
    Context ctx = Current();
    if (!IsGameReady() || !ctx.inventory || ctx.inventory->pOwner != ctx.player)
        return Failure("player inventory unavailable");
    if (!D2CLIENT_GetUIState(UI_CUBE)) return Failure("owned cube must be open");
    bool hasCube = false;
    unsigned visited = 0;
    for (UnitAny* item = D2COMMON_GetItemFromInventory(ctx.inventory); item;
         item = D2COMMON_GetNextItemFromInventory(item)) {
        if (++visited > 512) return Failure("owned inventory is incomplete");
        if (item->dwType != UNIT_ITEM || !item->pItemData ||
            item->pItemData->pOwnerInventory != ctx.inventory) return Failure("invalid owned inventory");
        const ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
        if (text && text->szCode[0] == 'b' && text->szCode[1] == 'o' && text->szCode[2] == 'x')
            hasCube = true;
    }
    if (!hasCube) return Failure("genuine owned Horadric Cube is unavailable");
    const std::string direction = args["direction"].get<std::string>();
    if (direction == "deposit") {
        UnitAny* item = Cursor(ctx);
        if (item) {
            const ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
            if (text && text->szCode[0] == 'b' && text->szCode[1] == 'o' && text->szCode[2] == 'x')
                return Failure("a cube cannot contain itself");
        }
        return Place(ctx, args, STORAGE_CUBE);
    }
    if (direction == "retrieve") {
        UnitAny* item = Owned(ctx, id);
        if (!item || !item->pItemData || item->dwMode != ITEM_MODE_INV_STASH_CUBE_STORE ||
            item->pItemData->NodePage != NODEPAGE_STORAGE || item->pItemData->ItemLocation != STORAGE_CUBE)
            return Failure("item is not in the owned cube");
        return PickupOwned(ctx, id, STORAGE_CUBE);
    }
    return Failure("direction must be deposit or retrieve");
}
}
