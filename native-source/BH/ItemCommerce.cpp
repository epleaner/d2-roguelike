#define NOMINMAX
#include "ItemCommerce.h"

#include "Constants.h"
#include "D2Helpers.h"
#include "D2Ptrs.h"
#include "InventoryActions.h"
#include "WorldState.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
using json = nlohmann::json;
constexpr unsigned kMaximumStock = 512;
constexpr DWORD kClientTimestamp = 0x4B95CA3E;
constexpr DWORD kCommonTimestamp = 0x4B95C439;
constexpr DWORD kClientSize = 0x135000;
constexpr DWORD kCommonSize = 0xA9000;
constexpr uintptr_t kMenuRva = 0xF3BA0;
constexpr uintptr_t kTradeCallbackRva = 0x4AEE0;
constexpr uintptr_t kTradeModeRva = 0x1197A8;
constexpr uintptr_t kPriceRva = 0x29D60;
constexpr unsigned kMenuRecords = 48;

json Failure(const char* message) { return {{"ok", false}, {"error", message}}; }

bool Uint32(const json& args, const char* name, DWORD& result) {
    if (!args.is_object() || !args.contains(name)) return false;
    const json& value = args.at(name);
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) {
        const uint64_t n = value.get<uint64_t>();
        if (n > UINT32_MAX) return false;
        result = static_cast<DWORD>(n);
    } else {
        const int64_t n = value.get<int64_t>();
        if (n < 0 || n > UINT32_MAX) return false;
        result = static_cast<DWORD>(n);
    }
    return true;
}

// These are raw S13-only bindings. Reject any other executable BEFORE dereferencing
// the menu table, trade/gamble discriminator, callback, or price ordinal.
const BYTE* ExactImage(const char* name, DWORD timestamp, DWORD imageSize) {
    const BYTE* module = reinterpret_cast<const BYTE*>(GetModuleHandleA(name));
    if (!module) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) ||
        static_cast<DWORD>(dos->e_lfanew) > imageSize - sizeof(IMAGE_NT_HEADERS32)) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != timestamp || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage != imageSize) return nullptr;
    return module;
}

struct Native {
    const BYTE* client;
    const BYTE* common;
};
Native Bind() {
    Native binding = {ExactImage("D2Client.dll", kClientTimestamp, kClientSize),
                      ExactImage("D2Common.dll", kCommonTimestamp, kCommonSize)};
    if (!binding.client || !binding.common) return {};
    // Callback +5 starts at its non-relocated prologue. The first five bytes
    // contain a relocated absolute address and must not be compared to disk.
    constexpr BYTE callback[] = {0x83, 0xEC, 0x10, 0x55, 0x33, 0xED, 0x3B, 0xC5, 0x56, 0x57};
    constexpr BYTE price[] = {0x8B, 0x44, 0x24, 0x18, 0x8B, 0x4C, 0x24, 0x14,
                              0x8B, 0x54, 0x24, 0x10};
    DWORD tradeModeAddress = 0, gambleModeAddress = 0;
    memcpy(&tradeModeAddress, binding.client + kTradeCallbackRva + 0xD1, sizeof(DWORD));
    memcpy(&gambleModeAddress, binding.client + 0x4ABE0 + 0x7A, sizeof(DWORD));
    if (memcmp(binding.client + kTradeCallbackRva + 5, callback, sizeof(callback)) != 0 ||
        binding.client[kTradeCallbackRva + 0xCF] != 0x89 ||
        binding.client[kTradeCallbackRva + 0xD0] != 0x2D ||
        binding.client[0x4ABE0 + 0x78] != 0x89 ||
        binding.client[0x4ABE0 + 0x79] != 0x1D ||
        tradeModeAddress != reinterpret_cast<uintptr_t>(binding.client + kTradeModeRva) ||
        gambleModeAddress != tradeModeAddress ||
        memcmp(binding.common + kPriceRva, price, sizeof(price)) != 0 ||
        reinterpret_cast<const BYTE*>(GetProcAddress(
            reinterpret_cast<HMODULE>(const_cast<BYTE*>(binding.common)),
            MAKEINTRESOURCEA(10107))) != binding.common + kPriceRva) return {};
    return binding;
}

// NPCMenu's 39-byte packed S13 table ends at F42F0. Entry zero is always
// Talk; 0x0D44 (or 0x0D06) is Trade. 0x0D46 is Gamble and is not allowed.
bool TradeMenu(const Native& native, DWORD classId) {
    const auto* menus = reinterpret_cast<const NPCMenu*>(native.client + kMenuRva);
    const DWORD callback = static_cast<DWORD>(reinterpret_cast<uintptr_t>(native.client + kTradeCallbackRva));
    for (unsigned i = 0; i < kMenuRecords; ++i) {
        const NPCMenu& menu = menus[i];
        if (menu.dwNPCClassId != classId) continue;
        if (menu.dwEntryAmount < 2 || menu.dwEntryAmount > 5) return false;
        const WORD ids[] = {menu.wEntryId1, menu.wEntryId2, menu.wEntryId3, menu.wEntryId4};
        const DWORD calls[] = {menu.dwEntryFunc1, menu.dwEntryFunc2, menu.dwEntryFunc3, menu.dwEntryFunc4};
        for (unsigned j = 0; j < 4; ++j)
            if ((ids[j] == 0x0D44 || ids[j] == 0x0D06) && calls[j] == callback) return true;
        return false;
    }
    return false;
}

bool ValidNpc(UnitAny* npc, UnitAny* player) {
    if (!npc || npc->dwType != UNIT_MONSTER || !npc->pMonsterData ||
        !npc->pMonsterData->pMonStatsTxt ||
        !npc->pMonsterData->pMonStatsTxt->dwFlags.bNPC ||
        !npc->pMonsterData->pMonStatsTxt->dwFlags.bInteract ||
        npc->dwMode == NPC_MODE_DEATH || npc->dwMode == NPC_MODE_DEAD ||
        !npc->pPath || !npc->pPath->pRoom1 || !npc->pPath->pRoom1->pRoom2 ||
        !npc->pPath->pRoom1->pRoom2->pLevel || !player || !player->pPath ||
        !player->pPath->pRoom1 || !player->pPath->pRoom1->pRoom2 ||
        !player->pPath->pRoom1->pRoom2->pLevel) return false;
    return npc->pPath->pRoom1->pRoom2->pLevel->dwLevelNo ==
           player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
}

struct Context { UnitAny* player; UnitAny* npc; ::Inventory* stock; ::Inventory* owned; };
Context Current(const Native& native, bool shopRequired) {
    if (!native.client || !native.common || !IsGameReady()) return {};
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || player->dwType != UNIT_PLAYER || !player->pInventory ||
        player->pInventory->pOwner != player) return {};
    UnitAny* npc = D2CLIENT_GetCurrentInteractingNPC();
    if (!ValidNpc(npc, player) || !TradeMenu(native, npc->dwTxtFileNo)) return {};
    UnitAny* live = D2CLIENT_FindServerSideUnit(npc->dwUnitId, UNIT_MONSTER);
    if (live != npc || live->dwTxtFileNo != npc->dwTxtFileNo) return {};
    if (shopRequired && (!D2CLIENT_GetUIState(UI_NPCSHOP) ||
        *reinterpret_cast<const volatile DWORD*>(native.client + kTradeModeRva) != 0 ||
        !npc->pInventory || npc->pInventory->pOwner != npc ||
        npc->pInventory->dwOwnerId != npc->dwUnitId)) return {};
    return {player, npc, npc->pInventory, player->pInventory};
}

void Put32(BYTE* bytes, unsigned offset, DWORD number) {
    bytes[offset] = static_cast<BYTE>(number);
    bytes[offset + 1] = static_cast<BYTE>(number >> 8);
    bytes[offset + 2] = static_cast<BYTE>(number >> 16);
    bytes[offset + 3] = static_cast<BYTE>(number >> 24);
}

// Return a complete stock or fail. A server-side GUID lookup alone does not
// establish ownership; every item must be reached from the active NPC inventory.
json Stock(Context ctx, unsigned bound, DWORD wanted = UINT32_MAX,
           UnitAny** selected = nullptr, DWORD* selectedPrice = nullptr) {
    json items = json::array();
    UnitAny* seen[kMaximumStock] = {};
    unsigned count = 0;
    void* quest = D2CLIENT_GetQuestInfo();
    if (!quest || D2CLIENT_GetDifficulty() > 2) return Failure("native quest/difficulty price context unavailable");
    for (UnitAny* item = D2COMMON_GetItemFromInventory(ctx.stock); item;
         item = D2COMMON_GetNextItemFromInventory(item)) {
        if (count == bound || count == kMaximumStock) return Failure("NPC inventory exceeds the requested bound");
        if (item->dwType != UNIT_ITEM || !item->pItemData ||
            item->pItemData->pOwnerInventory != ctx.stock ||
            item->dwMode != ITEM_MODE_INV_STASH_CUBE_STORE ||
            D2CLIENT_FindServerSideUnit(item->dwUnitId, UNIT_ITEM) != item)
            return Failure("NPC inventory contains unknown or non-stock item");
        for (unsigned i = 0; i < count; ++i)
            if (seen[i] == item || seen[i]->dwUnitId == item->dwUnitId)
                return Failure("NPC inventory repeats an item");
        seen[count++] = item;
        if (!D2COMMON_GetItemText(item->dwTxtFileNo))
            return Failure("NPC inventory contains an item with unknown native text");
        if (selected && item->dwUnitId != wanted) continue;
        // S13 D2Client RVA 47FC2-47FEC computes the trade buy quote with
        // transaction=0 (2 is gamble; 1 is sell), using this exact ordinal.
        const DWORD price = D2COMMON_GetItemPrice(ctx.player, item, D2CLIENT_GetDifficulty(),
            static_cast<DWORD>(reinterpret_cast<uintptr_t>(quest)), ctx.npc->dwTxtFileNo, 0);
        if (!price || price > INT32_MAX) return Failure("native buy price unavailable or invalid");
        if (selected) {
            *selected = item;
            *selectedPrice = price;
        } else {
            json row = WorldState::DescribeItem(item);
            if (!row.value("ok", false)) {
                json failure = Failure("NPC stock identity is incomplete");
                failure["item_id"] = item->dwUnitId;
                failure["class_id"] = item->dwTxtFileNo;
                failure["description_error"] = row.value("error", std::string("unknown item description failure"));
                return failure;
            }
            row["buy_price"] = price;
            items.push_back(std::move(row));
        }
    }
    return {{"ok", true}, {"items", std::move(items)}, {"truncated", false}};
}

// No optimistic placement: only a fully observed empty rectangle is safe.
// Stack/tome merges may allow an engine purchase even without a free rectangle;
// this implementation intentionally declines that case rather than guessing.
bool FreeInventorySpace(Context ctx, UnitAny* item) {
    unsigned width = 0, height = 0;
    if (!InventoryActions::GridSize(ctx.owned, STORAGE_INVENTORY, width, height)) return false;
    ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
    if (!text || !text->binvwidth || !text->binvheight ||
        text->binvwidth > width || text->binvheight > height) return false;
    bool occupied[32 * 32] = {};
    UnitAny* seen[kMaximumStock] = {};
    unsigned count = 0;
    for (UnitAny* own = D2COMMON_GetItemFromInventory(ctx.owned); own;
         own = D2COMMON_GetNextItemFromInventory(own)) {
        if (count == kMaximumStock || !own->pItemData || own->dwType != UNIT_ITEM ||
            own->pItemData->pOwnerInventory != ctx.owned) return false;
        for (unsigned i = 0; i < count; ++i) if (seen[i] == own) return false;
        seen[count++] = own;
        if (own->dwMode != ITEM_MODE_INV_STASH_CUBE_STORE ||
            own->pItemData->NodePage != NODEPAGE_STORAGE ||
            own->pItemData->ItemLocation != STORAGE_INVENTORY) continue;
        ItemsTxt* ownText = D2COMMON_GetItemText(own->dwTxtFileNo);
        if (!ownText || !own->pItemPath || !ownText->binvwidth || !ownText->binvheight ||
            own->pItemPath->dwPosX >= width || own->pItemPath->dwPosY >= height ||
            ownText->binvwidth > width - own->pItemPath->dwPosX ||
            ownText->binvheight > height - own->pItemPath->dwPosY) return false;
        for (unsigned y = own->pItemPath->dwPosY;
             y < own->pItemPath->dwPosY + ownText->binvheight; ++y)
            for (unsigned x = own->pItemPath->dwPosX;
                 x < own->pItemPath->dwPosX + ownText->binvwidth; ++x)
                occupied[y * width + x] = true;
    }
    for (unsigned y = 0; y + text->binvheight <= height; ++y)
        for (unsigned x = 0; x + text->binvwidth <= width; ++x) {
            bool free = true;
            for (unsigned dy = 0; dy < text->binvheight && free; ++dy)
                for (unsigned dx = 0; dx < text->binvwidth; ++dx)
                    if (occupied[(y + dy) * width + x + dx]) { free = false; break; }
            if (free) return true;
        }
    return false;
}
}

namespace ItemCommerce {
json OpenShop(const json& args) {
    DWORD id;
    if (!args.is_object() || args.size() != 1 || !Uint32(args, "npc_id", id))
        return Failure("npc_id must be an unsigned 32-bit NPC GUID");
    const Native native = Bind();
    if (!native.client) return Failure("unrecognized native S13 client/common commerce ABI");
    Context ctx = Current(native, false);
    if (!ctx.npc || ctx.npc->dwUnitId != id || !D2CLIENT_GetUIState(UI_NPCMENU) ||
        D2CLIENT_GetUIState(UI_NPCSHOP)) return Failure("specified live vendor's NPC menu is not open");
    if (ctx.owned->pCursorItem) return Failure("cursor must be empty before opening trade");
    using Callback = int (__cdecl*)();
    const auto callback = reinterpret_cast<Callback>(const_cast<BYTE*>(native.client + kTradeCallbackRva));
    if (!callback()) return Failure("native trade menu callback declined the request");
    return {{"ok", true}, {"status", "requested"}, {"npc_id", id}, {"destination", "trade_shop"}};
}

json ShopState(const json& args) {
    if (!args.is_object() || (args.size() && (args.size() != 1 || !args.contains("max_items"))))
        return Failure("ShopState accepts only optional max_items");
    DWORD maxItems = 128;
    if (args.contains("max_items") && (!Uint32(args, "max_items", maxItems) ||
        !maxItems || maxItems > kMaximumStock)) return Failure("max_items must be between 1 and 512");
    const Native native = Bind();
    if (!native.client) return Failure("unrecognized native S13 client/common commerce ABI");
    Context ctx = Current(native, true);
    if (!ctx.stock) return Failure("active native trade inventory unavailable");
    json result = Stock(ctx, maxItems);
    if (!result.value("ok", false)) return result;
    const DWORD gold = D2COMMON_GetUnitStat(ctx.player, STAT_GOLD, 0);
    const DWORD stash = D2COMMON_GetUnitStat(ctx.player, STAT_GOLDBANK, 0);
    if (gold > INT32_MAX || stash > INT32_MAX) return Failure("native player gold is invalid");
    result["current_npc"] = {{"unit_id", ctx.npc->dwUnitId}, {"class_id", ctx.npc->dwTxtFileNo}};
    result["gold"] = gold;
    result["gold_stash"] = stash;
    return result;
}

json Buy(const json& args) {
    DWORD npcId, itemId, maxPrice;
    if (!args.is_object() || args.size() != 3 || !Uint32(args, "npc_id", npcId) ||
        !Uint32(args, "item_id", itemId) || !Uint32(args, "max_price", maxPrice))
        return Failure("npc_id, item_id and max_price must be unsigned 32-bit integers");
    const Native native = Bind();
    if (!native.client) return Failure("unrecognized native S13 client/common commerce ABI");
    Context ctx = Current(native, true);
    if (!ctx.stock || ctx.npc->dwUnitId != npcId)
        return Failure("specified NPC is not the currently open native trade vendor");
    if (ctx.owned->pCursorItem || D2CLIENT_GetCursorItem()) return Failure("cursor is occupied");
    UnitAny* selected = nullptr;
    DWORD price = 0;
    json snapshot = Stock(ctx, kMaximumStock, itemId, &selected, &price);
    if (!snapshot.value("ok", false)) return snapshot;
    if (!selected) return Failure("item is not in the active vendor inventory");
    if (price > maxPrice) return Failure("native buy quote exceeds max_price");
    const uint64_t gold = D2COMMON_GetUnitStat(ctx.player, STAT_GOLD, 0);
    const uint64_t stash = D2COMMON_GetUnitStat(ctx.player, STAT_GOLDBANK, 0);
    if (gold > INT32_MAX || stash > INT32_MAX || gold + stash < price)
        return Failure("insufficient available player gold");
    if (!FreeInventorySpace(ctx, selected))
        return Failure("no verified empty inventory rectangle for purchased item");
    // Exact S13 D2Game RVA CF220: opcode 0x32, 17 bytes; +9 item mode
    // (uint16), +11 transaction BUY=0 (uint16), high bit of +9 dword is
    // multibuy (zero). +13 is the native quoted price. One packet only.
    BYTE packet[17] = {0x32};
    Put32(packet, 1, npcId);
    Put32(packet, 5, itemId);
    packet[9] = static_cast<BYTE>(selected->dwMode); // native stock mode zero
    Put32(packet, 13, price);
    if (!D2NET_SendPacket(sizeof(packet), 0, packet))
        return Failure("native purchase packet was not accepted for dispatch");
    return {{"ok", true}, {"status", "requested"}, {"npc_id", npcId},
            {"stock_item_id", itemId}, {"item_id", itemId}, {"buy_price", price},
            {"class_id", selected->dwTxtFileNo}, {"npc_class_id", ctx.npc->dwTxtFileNo}};
}
}
