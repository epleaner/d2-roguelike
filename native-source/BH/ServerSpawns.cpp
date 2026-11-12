#include "ServerSpawns.h"
#include "Constants.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "LabProfile.h"
#include "CharacterState.h"
#include "ServerMonsters.h"
#include "ObservationTelemetry.h"
#include "CombatTelemetry.h"
#include "RoguelikeAffixes.h"
#include "RoguelikePortalLabels.h"
#include "RoguelikeProgression.h"
#include "RoguelikeAura.h"
#include "RoguelikeBosses.h"
#include "MerchantQuality.h"
#include "RoguelikeLoot.h"
#include "RoguelikeMerchantPrice.h"
#include "RoguelikeResistance.h"
#include <array>
#include <detours.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <random>
#include <climits>
#include <intrin.h>
#include <charconv>
#include <string_view>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct RunTile { int x = 0, y = 0; };
struct RunLayout {
    std::array<RunTile, 4> actors{};
    std::array<RunTile, 3> portals{};
    unsigned actorCount = 0;
};
namespace ServerSpawns {
    void OfferChoices(void* game, int level, const RunLayout* layout = nullptr);
    void TryCompleteClear(void* game, int level);
    void RestoreRoguelikeZones(bool saveSnapshot);
    void HandleRunDeath();
    bool SelectVanillaEncounterPool(unsigned depth, std::minstd_rand& random,
        const std::array<std::uint16_t, 2>& excludedPrimaries,
        unsigned excludedCount,
        RoguelikeProgression::EncounterSelection& selection,
        int& densityBaseline, std::string& error);
    nlohmann::json RunSnapshotStatus();
}
namespace {
using json = nlohmann::json;
// Exact pinned D2GAME image, source-matched to D2MOO Items.cpp, not its 1.10 addresses.
constexpr DWORD kCreateRva = 0x11490, kRollAffixRva = 0x144D0, kChatRva = 0xCDBB0, kPacketTableRva = 0xFA078;
#pragma pack(push, 1)
struct Drop {
    UnitAny* unit; void* sourceSeed; void* game; int level; DWORD unknown;
    int baseId, spawnType, x, y; Room1* room; WORD initFlags, format;
    BOOL force; int quality, quantity, minDurability, maxDurability, uniqueIndex;
    DWORD flags, seed, itemSeed; int earLevel, quantityOverride; char name[16];
    int prefixes[3], suffixes[3]; DWORD generationFlags;
};
#pragma pack(pop)
static_assert(sizeof(Drop) == 0x84, "Exact item constructor layout required");
using Create = UnitAny* (__stdcall*)(void*, Drop*, int);
using Chat = int (__fastcall*)(void*, UnitAny*, BYTE*, int);
using RollAffix = WORD (__stdcall*)(UnitAny*, int, int, int, int, int, int);
struct Entry { Chat callback; int flag; };
static_assert(sizeof(Entry) == 8, "Exact server callback table required");
struct Property { int id, value; WORD layer; };
struct Pending {
    std::string id, fingerprint, expectedEpoch;
    DWORD playerId = 0, seed = 1;
    int baseId = -1, quality = 2, uniqueId = -1, setId = -1, level = 99;
    std::array<int, 3> prefixes{}, suffixes{};
    std::vector<Property> properties;
    bool running = false, completed = false;
    bool exactAffixes = false;
    json result;
    std::optional<ServerMonsters::Request> monsterJob;
    std::optional<std::pair<int, int>> portalJob;
    std::optional<int> triggerJob;
    bool runArm = false;
    bool runStart = false; // Bound native RPC and plain chat share StartTownRun.
    bool townEntry = false; // Unlinked town trigger; arms before its first one-way warp.
    int triggerSource = 0;
    std::uint64_t runSerial = 0;
    DWORD portalReplacementId = 0; // Internal choice only; never a caller-supplied RPC.
    enum class ZoneKind : BYTE { Monster, Boss, Merchant, Shrine, Chest };
    using RewardFocus = RoguelikeLoot::RewardFocus;
    struct Zone {
        int level, classId, monsterLevel, density, magicFind;
        int depth = 0;
        RoguelikeAffixes::Roll affixes{};
        ZoneKind kind = ZoneKind::Monster;
        BYTE otherShrine = 0;
        RewardFocus reward = RewardFocus::Equipment;
        int bossSuperuniqueId = -1;
        BYTE secondOtherShrine = 0;
        bool testOnly = false;
        bool resumeCleared = false;
        int densityBaseline = 0;
        std::array<int, 4> monsterClasses{};
        unsigned monsterClassCount = 0;
    };
    std::optional<Zone> zoneJob;
    bool retireJob = false, retireCommit = false;
    int retireFrom = 0;
    bool levelInventory = false;
    bool resumeRun = false;
    std::optional<json> resumeSnapshot;
    std::optional<json> traceJob;
};
bool BossZone(Pending::ZoneKind kind) {
    return kind == Pending::ZoneKind::Boss;
}
std::mutex mutex;
std::condition_variable completedRequest;
std::vector<std::shared_ptr<Pending>> requests;
// Portal nonces are never evicted: a timed-out or uncertain mutation must not replay; 4096 is a bounded per-process safety cap.
std::map<std::string, std::pair<std::string, json>> portalReceipts;
// The 64-request queue evicts completed entries during full-room sweeps.
std::map<std::string, std::shared_ptr<Pending>> ownedJobs;
struct ObservedHireNames {
    std::string epoch;
    DWORD playerId = 0;
    int area = 0;
    std::vector<WORD> available;
};
ObservedHireNames observedHireNames; // Guarded by mutex; never trust a caller's guessed merc name.
std::map<int, std::string> selectedTriggerEpochBySource;
std::array<int, 255> runRouteNext{};
std::array<int, 255> runRouteParent{};
struct RunResumeState {
    bool active = false;
    bool rootCleared = false;
    bool starterSettled = false;
    int rootLevel = 20;
    std::string snapshotPath;
    std::vector<int> path;
    std::map<int, Pending::Zone> zones;
    std::map<int, bool> cleared;
    std::map<int, std::array<bool, 3>> chestSettled;
    std::map<int, std::array<int, 12>> chestUniqueIds;
    std::map<int, unsigned> chestUniqueCounts;
    int desiredLevel = 20;
    size_t currentIndex = 0;
};
RunResumeState runResume;
std::mutex runResumeContinuationMutex;
json lastRunSnapshotStatus = {{"attempted", false}, {"reason", "no_exit_observed"}};
// Loaded TXT is process-wide. Keep originals until the owning game ends; never reuse an arena ID.
struct ZoneSlot {
    LevelsTxt original{};
    LevelsTxt configuredRow{};
    bool configured = false;
    int magicFind = 0;
    int depth = 0;
    int densityBaseline = 0;
    DWORD regionDensity = 0;
    bool testOnly = false;
    RoguelikeAffixes::Roll affixes{};
    Pending::ZoneKind kind = Pending::ZoneKind::Monster;
    BYTE otherShrine = 0;
    Pending::RewardFocus reward = Pending::RewardFocus::Equipment;
    BYTE secondOtherShrine = 0;
    int bossSuperuniqueId = -1;
    int selectedClassId = -1;
    std::array<int, 4> selectedMonsterClasses{};
    unsigned selectedMonsterClassCount = 0;
};
std::array<ZoneSlot, 255> zoneSlots{};
LevelsTxt* zoneRows = nullptr;
std::string zoneEpoch;
void* zoneGame = nullptr;
// PD2 stores one active map-affix context per game, not per region. Keep the
// full selected context across its one-way arena until the next choice.
void* mapMonsterGame = nullptr;
std::string mapMonsterEpoch;
WORD mapMonsterCount = 0, mapPlayerCount = 0, mapGlobalCount = 0;
std::array<BYTE, 0x400> mapMonsterBytes{}, mapPlayerBytes{};
std::array<BYTE, 0xC0> mapGlobalBytes{};
int mapPlayerAppliedArea = 0;
int mapPlayerBeforeMagicFind = 0, mapPlayerAfterMagicFind = 0;
unsigned mapMonsterApplied = 0;
unsigned mapMonsterStockExisting = 0;
DWORD mapMonsterSampleUnit = 0;
int mapMonsterSampleArea = 0;
int mapMonsterSampleBefore = 0;
int mapMonsterSampleAfter = 0;
std::atomic<void*> runGame{nullptr};
std::atomic<bool> runDeathHandled{false};
UnitAny* runPlayer = nullptr; // Server-thread only; cleared before game teardown.
std::string runEpoch;
std::string runCharacterName; // Owned while the player record is valid; used during teardown.
std::atomic<int> runStage{0};
// A normal client skill selection is required; keep its temporary skill stable.
std::atomic<bool> runAuraPending{false};
enum class RunAuraCastState : int { Idle, Send, Sent, Failed };
std::atomic<RunAuraCastState> runAuraCastState{RunAuraCastState::Idle};
std::atomic<WORD> runAuraCastSkill{0};
std::atomic<DWORD> runAuraCastTarget{0};
std::atomic<DWORD> runAuraCastTargetType{UNIT_PLAYER};
struct RunAuraStatus {
    void* game = nullptr;
    DWORD playerId = 0;
    const RoguelikeAura::Config* config = nullptr;
    std::string epoch;
    DWORD activationDeadlineFrame = 0;
    DWORD endFrame = 0;
    DWORD itemId = 0;
    Skill* previousFirst = nullptr;
    Skill* previousRight = nullptr;
    Skill* previousUsed = nullptr;
    std::string error;
};
RunAuraStatus runAura; // Server-thread only; client reads the atomic packet state.
Skill runAuraTemporarySkill{};
bool ClearRunAuraSkill(void* game, UnitAny* player);
const SkillsTxt* RunAuraSkill(const RoguelikeAura::Config* config);
std::atomic<bool> runStarterSettled{false};
std::atomic<bool> runBossKillWitness{false}; // Server-certified death; client only requests a later census.
int runArrivalLevel = 0; // Server-thread-only target while the fresh room is constructed.
int runRootLevel = 0; // First active run area; 20 is the authored Tower.
void* runAreaUsageGame = nullptr;
std::string runAreaUsageEpoch;
std::array<bool, 255> runUsedAreas{}; // Consumed during the active run; resume rebuilds this from the saved path.
std::array<bool, 255> runCleared{};
std::array<bool, 255> runRetiredDestinations{}; // Rerolled-away IDs are never offered again.
std::array<bool, 255> runPopulationReady{};
std::array<BYTE, 255> runEncounters{}; // 0 awaiting, 1 natural, 2 seeded, 3 failed.
std::array<DWORD, 255> runBossUnit{};
struct BossDamageSource { DWORD id = 0; UnitAny* unit = nullptr; WORD nativeLevel = 0; };
std::array<BossDamageSource, 32> runBossDamageSources{};
unsigned runBossDamageCount = 0;
unsigned runScaledHits = 0;
unsigned runScaledMissileHits = 0;
unsigned runBossProjectileHits = 0;
int runBossProjectileOriginal = 0, runBossProjectileApplied = 0;
unsigned runBossProjectileComponent = 0;
unsigned runRejectedMissileOwners = 0;
unsigned runObservedMissileDamage = 0;
unsigned runOtherAttackerDamage = 0;
struct MissileDamageSample { DWORD id = 0, classId = 0, type = 0, ownerType = 0, ownerId = 0; };
MissileDamageSample runMissileDamageSample{};
int runDamageOriginal = 0, runDamageApplied = 0;
unsigned runDamageComponent = 0;
WORD runDamageNativeLevel = 0;
bool runDamageMissile = false;
std::array<std::array<DWORD, 3>, 255> runShrineIds{};
std::array<std::array<DWORD, 2>, 255> runClearRewardIds{};
DWORD runStarterRewardId = 0;
std::array<std::array<DWORD, 3>, 255> runChestIds{};
std::array<std::array<std::array<DWORD, 4>, 3>, 255> runChestRewardIds{};
std::array<std::array<BYTE, 3>, 255> runChestRewardCounts{};
std::array<std::array<int, 12>, 255> runChestUniqueIds{};
std::array<unsigned, 255> runChestUniqueCounts{};
std::array<std::array<bool, 3>, 255> runChestSettled{};
std::string runError;
std::string runDropError;
unsigned runDropFailureCount = 0;
std::array<std::uint64_t, 2048> runKillLootLedger{};
int runKillLootLedgerLevel = 0;
unsigned runChoiceSerial = 0;
std::uint64_t runEntrySerial = 0;
std::uint64_t runActiveSerial = 0;
constexpr unsigned kZoneAreaAffix = 0x10000;
const AutoMagicTxt* ZoneAffix(unsigned id) {
    if (id == kZoneAreaAffix) return nullptr;
    D2MagicAffixDataTbl* table = D2COMMON_10492_DATATBLS_GetMagicAffixDataTables();
    return table && table->pMagicAffixTxt && id &&
        id <= static_cast<unsigned>(table->nMagicAffixTxtRecordCount) ?
        &table->pMagicAffixTxt[id - 1] : nullptr;
}
const char* ZoneAffixName(unsigned id) {
    if (id == kZoneAreaAffix) return "Elevated";
    const AutoMagicTxt* affix = ZoneAffix(id);
    return affix ? affix->szName : "";
}
json ZoneAffixReceipt(const RoguelikeAffixes::Roll& roll) {
    json modifiers = json::array(), stats = json::array();
    for (unsigned i = 0; i < roll.count; ++i) {
        const auto& modifier = roll.modifiers[i];
        modifiers.push_back({{"id", modifier.id},
            {"name", ZoneAffixName(modifier.id)}});
    }
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        stats.push_back({{"map_stat", stat.mapStat}, {"layer", stat.layer},
            {"category", stat.category}, {"value", stat.value}});
    }
    return {{"rarity", RoguelikeAffixes::Name(roll.quality)},
        {"affixes", std::move(modifiers)}, {"stats", std::move(stats)},
        {"area_level_bonus", roll.areaLevel},
        {"density_bonus_percent", roll.densityPercent},
        {"magic_find_bonus", roll.magicFind},
        {"monster_health_bonus_percent", roll.monsterHealthPercent}};
}
bool Readable(const void* address, size_t size);
bool PortalArea(UnitAny* player, int expected);
// D2Client S13 RVA A5D90 receives UnitAny in EAX; this getter is not the
// portal-special hover renderer. Never mutate shared ObjectTxt (class 60).
struct ChoiceName {
    DWORD playerId;
    int source;
    std::string epoch;
    wchar_t text[2048];
};
std::map<DWORD, ChoiceName> choiceNames; // Guarded by mutex, as are ownedJobs and receipts.
struct RunOwnedObject {
    void* game = nullptr;
    UnitAny* unit = nullptr;
    ObjectPath* path = nullptr;
    Room1* room = nullptr;
    Room2* room2 = nullptr;
    Level* level = nullptr;
    DWORD unitId = 0, playerId = 0, area = 0;
};
constexpr size_t kRunOwnedObjectCapacity = 512;
std::array<RunOwnedObject, kRunOwnedObjectCapacity> runOwnedObjects{};
size_t runOwnedObjectCount = 0;
bool RemoveOwnedObject(void* game, UnitAny* player, DWORD id, UnitAny* expected,
    const char** failureReason = nullptr);
bool ReleaseSelectedTarget(UnitAny* player);
void* originalUnitName = nullptr;
bool unitNameInstalled = false;
wchar_t* __cdecl NamedChoice(UnitAny* unit) {
    if (!unit || !Readable(unit, sizeof(UnitAny)) || unit->dwType != UNIT_OBJECT ||
        unit->dwTxtFileNo != 60) return nullptr;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || !Readable(player, sizeof(UnitAny)) || player->dwType != UNIT_PLAYER ||
        !Readable(player->pPath, sizeof(Path)) ||
        !Readable(player->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(player->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(player->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
        !Readable(unit->pObjectPath, sizeof(ObjectPath)) ||
        !Readable(unit->pObjectPath->pRoom1, sizeof(Room1)) ||
        !Readable(unit->pObjectPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(unit->pObjectPath->pRoom1->pRoom2->pLevel, sizeof(Level)))
        return nullptr;
    thread_local wchar_t copy[2048];
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = choiceNames.find(unit->dwUnitId);
    if (found == choiceNames.end() || found->second.playerId != player->dwUnitId ||
        !PortalArea(player, found->second.source) ||
        unit->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo != found->second.source ||
        !ObservationTelemetry::MatchesEpoch(found->second.epoch)) return nullptr;
    wcscpy_s(copy, found->second.text);
    return copy; // Both pinned client callers copy this before the next getter invocation.
}
// Preserve the custom EAX input when falling through to Detours' trampoline.
__declspec(naked) wchar_t* UnitNameHook() {
    __asm {
        push eax
        push ecx
        push edx
        push eax
        call NamedChoice
        add esp, 4
        pop edx
        pop ecx
        test eax, eax
        jz original
        add esp, 4
        ret
    original:
        pop eax
        jmp dword ptr [originalUnitName]
    }
}
// Portal-special hover at C1085 resolves the destination byte through
// GetLevelName instead of the generic unit-name path. EBP is the hovered unit.
void* originalHoverCall = nullptr;
BYTE* hoverAfterCall = nullptr;
__declspec(naked) wchar_t* PortalHoverHook() {
    __asm {
        push ecx
        push edx
        push esi
        push ebp
        call NamedChoice
        add esp, 4
        pop esi
        pop edx
        pop ecx
        test eax, eax
        jz original
        jmp dword ptr [hoverAfterCall]
    original:
        jmp dword ptr [originalHoverCall]
    }
}
bool MarketStockCall(void* game, UnitAny* npc, int level, const void* caller);
bool RewardTables(D2ItemDataTbl*& bases, sgptDataTable*& records);
bool RewardBase(const D2ItemDataTbl* table, const sgptDataTable* itemTypes,
    int index, int itemLevel, int playerLevel, bool starter, int sockets,
    RoguelikeLoot::BaseKind kind = RoguelikeLoot::BaseKind::Equipment,
    int charmSize = 0);
static_assert(offsetof(ItemsTxt, dwnormcode) == 0x84 &&
    offsetof(ItemsTxt, dwubercode) == 0x88 &&
    offsetof(ItemsTxt, dwultracode) == 0x8C &&
    offsetof(ItemsTxt, bgemsockets) == 0x138 &&
    offsetof(ItemsTxt, nType) == 0x11E, "pinned S13 item table fields");
void* originalStoreItem = nullptr;
void* originalMarketRandom = nullptr;
using MarketItemPrice = DWORD (__stdcall *)(UnitAny*, UnitAny*, DWORD, DWORD, DWORD, DWORD);
MarketItemPrice originalMarketItemPrice = nullptr;
struct MarketPriceIat {
    const char* moduleName;
    DWORD rva;
    const BYTE* image = nullptr;
    DWORD* slot = nullptr;
    DWORD original = 0;
    bool installed = false;
};
constexpr DWORD kMarketPriceOrdinal = 10107;
MarketPriceIat clientMarketPriceIat{"D2Client.dll", 0xCE8CC};
MarketPriceIat serverMarketPriceIat{"D2Game.dll", 0xF8294};
bool MarketStoreRequest(void* game, UnitAny* npc, int itemLevel,
    const void* caller) {
    const BYTE* image = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2Game.dll"));
    return image && (caller == image + 0xBF464 || caller == image + 0xBF577) &&
        MarketStockCall(game, npc, itemLevel, image + 0xBF352);
}
int MarketCharacterLevel() {
    return Readable(runPlayer, sizeof(UnitAny)) && runPlayer->dwType == UNIT_PLAYER ?
        D2COMMON_GetUnitStat(runPlayer, STAT_LEVEL, 0) : 0;
}
bool RoguelikeMarketVendor(DWORD vendorId) {
    return vendorId == 147 || vendorId == 148 ||
        vendorId == 150 || vendorId == 154;
}
bool RoguelikeMarketPriceContext(UnitAny* player, UnitAny* item,
    DWORD vendorId, DWORD transactionType) {
    if (transactionType != 0 || !RoguelikeMarketVendor(vendorId) ||
        !Readable(player, sizeof(UnitAny)) ||
        !Readable(item, sizeof(UnitAny)) || item->dwType != UNIT_ITEM ||
        !Readable(item->pItemData, sizeof(ItemData)))
        return false;
    const DWORD quality = item->pItemData->dwQuality;
    if (quality != 5 && quality != 7) return false;
    const int area = runStage.load(std::memory_order_acquire);
    return area >= 13 && area < 255 &&
        runGame.load(std::memory_order_acquire) != nullptr &&
        zoneSlots[area].configured &&
        zoneSlots[area].kind == Pending::ZoneKind::Merchant &&
        runCleared[area] && PortalArea(player, area);
}
DWORD SameBaseRareStockPrice(UnitAny* player, UnitAny* item, DWORD difficulty,
    DWORD questInfo, DWORD vendorId, DWORD transactionType) {
    UnitAny* vendor = D2CLIENT_GetCurrentInteractingNPC();
    if (!Readable(vendor, sizeof(UnitAny)) || vendor->dwType != UNIT_MONSTER ||
        vendor->dwTxtFileNo != vendorId ||
        !Readable(vendor->pInventory, sizeof(Inventory)) ||
        vendor->pInventory->pOwner != vendor ||
        vendor->pInventory->dwOwnerId != vendor->dwUnitId)
        return 0;
    Inventory* stockInventory = vendor->pInventory;
    DWORD highest = 0;
    unsigned visited = 0;
    for (UnitAny* rare = D2COMMON_GetItemFromInventory(stockInventory); rare;
         rare = D2COMMON_GetNextItemFromInventory(rare)) {
        if (++visited > 512 || !Readable(rare, sizeof(UnitAny)) ||
            rare->dwType != UNIT_ITEM ||
            !Readable(rare->pItemData, sizeof(ItemData)) ||
            rare->pItemData->pOwnerInventory != stockInventory)
            return 0;
        if (rare == item || rare->dwMode != ITEM_MODE_INV_STASH_CUBE_STORE ||
            rare->dwTxtFileNo != item->dwTxtFileNo ||
            rare->pItemData->dwQuality != 6 ||
            rare->pItemData->dwItemLevel != item->pItemData->dwItemLevel)
            continue;
        const DWORD quote = originalMarketItemPrice(
            player, rare, difficulty, questInfo, vendorId, transactionType);
        if (quote > highest) highest = quote;
    }
    return highest;
}

using SetUnitStat = void (__stdcall*)(UnitAny*, DWORD, int, WORD);
SetUnitStat setVendorItemStat = nullptr;
// Akara's temporary shop Key uses the stack quantity in its native quote.
// Clamp only the actual vendor-owned Key before pricing; leave player stacks untouched.
void ClampAkaraKeyQuote(UnitAny* player, UnitAny* item, DWORD vendorId) {
    if (vendorId != 148 || !setVendorItemStat || !item ||
        !Readable(item, sizeof(UnitAny)) || item->dwType != UNIT_ITEM ||
        !Readable(item->pItemData, sizeof(ItemData)))
        return;
    const ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
    if (!Readable(text, sizeof(ItemsTxt)) || memcmp(text->szCode, "key", 3) ||
        (text->szCode[3] != ' ' && text->szCode[3] != '\0'))
        return;
    if (!player || !Readable(player, sizeof(UnitAny)))
        return;
    const int area = runStage.load(std::memory_order_acquire);
    if (area < 13 || area >= 255 ||
        runGame.load(std::memory_order_acquire) == nullptr ||
        !zoneSlots[area].configured ||
        zoneSlots[area].kind != Pending::ZoneKind::Merchant ||
        !runCleared[area] || !PortalArea(player, area))
        return;
    UnitAny* vendor = D2CLIENT_GetCurrentInteractingNPC();
    if (!Readable(vendor, sizeof(UnitAny)) || vendor->dwType != UNIT_MONSTER ||
        vendor->dwTxtFileNo != vendorId ||
        !Readable(vendor->pInventory, sizeof(Inventory)) ||
        vendor->pInventory->pOwner != vendor ||
        vendor->pInventory->dwOwnerId != vendor->dwUnitId ||
        item->pItemData->pOwnerInventory != vendor->pInventory)
        return;
    if (D2COMMON_GetUnitStat(item, STAT_AMMOQUANTITY, 0) <= 1)
        return;
    setVendorItemStat(item, STAT_AMMOQUANTITY, 1, 0);
}
DWORD __stdcall MarketItemPriceHook(UnitAny* player, UnitAny* item,
    DWORD difficulty, DWORD questInfo, DWORD vendorId, DWORD transactionType) {
    if (!originalMarketItemPrice) return 0;
    ClampAkaraKeyQuote(player, item, vendorId);
    const DWORD nativePrice = originalMarketItemPrice(
        player, item, difficulty, questInfo, vendorId, transactionType);
    if (!RoguelikeMarketPriceContext(player, item, vendorId, transactionType))
        return nativePrice;
    // Quote a rare-quality copy without mutating the live vendor item.
    ItemData rareData = *item->pItemData;
    rareData.dwQuality = 6;
    rareData.dwFileIndex = 0;
    UnitAny rareItem = *item;
    rareItem.pItemData = &rareData;
    const DWORD rarePrice = originalMarketItemPrice(
        player, &rareItem, difficulty, questInfo, vendorId, transactionType);
    const DWORD stockRarePrice = SameBaseRareStockPrice(
        player, item, difficulty, questInfo, vendorId, transactionType);
    return RoguelikeMerchantPrice::SpecialQualityFloor(
        item->pItemData->dwQuality, nativePrice,
        (std::max)(rarePrice, stockRarePrice));
}
bool PrepareMarketPriceIat(MarketPriceIat& binding, std::string& error) {
    static_assert(sizeof(void*) == sizeof(DWORD), "pinned S13 PE32 IAT");
    HMODULE module = GetModuleHandleA(binding.moduleName);
    HMODULE common = GetModuleHandleA("D2Common.dll");
    FARPROC nativePrice = common ? GetProcAddress(
        common, MAKEINTRESOURCEA(kMarketPriceOrdinal)) : nullptr;
    if (!module || !nativePrice) {
        error = std::string("offline ") + binding.moduleName +
            " or D2Common item-price export unavailable";
        return false;
    }
    const BYTE* image = reinterpret_cast<const BYTE*>(module);
    const BYTE* entry = image + binding.rva;
    if (!Readable(entry, sizeof(DWORD))) {
        error = std::string("pinned S13 ") + binding.moduleName +
            " item-price IAT is unreadable";
        return false;
    }
    DWORD target = 0;
    memcpy(&target, entry, sizeof(target));
    const DWORD expected = static_cast<DWORD>(
        reinterpret_cast<uintptr_t>(nativePrice));
    if (!expected || target != expected) {
        error = std::string("pinned S13 ") + binding.moduleName +
            " item-price IAT changed (target " + std::to_string(target) +
            ", expected " + std::to_string(expected) + ")";
        return false;
    }
    binding.image = image;
    binding.slot = reinterpret_cast<DWORD*>(
        const_cast<BYTE*>(entry));
    binding.original = target;
    return true;
}
bool PatchMarketPriceIat(MarketPriceIat& binding, std::string& error) {
    if (!originalMarketItemPrice || !binding.slot ||
        !binding.original || binding.installed) {
        error = std::string("invalid market price IAT patch state for ") +
            binding.moduleName;
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(binding.slot, sizeof(DWORD),
            PAGE_READWRITE, &oldProtection)) {
        error = std::string("cannot make ") + binding.moduleName +
            " item-price IAT writable";
        return false;
    }
    const PVOID expected = reinterpret_cast<PVOID>(
        static_cast<uintptr_t>(binding.original));
    const PVOID hook = reinterpret_cast<PVOID>(MarketItemPriceHook);
    const PVOID observed = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(binding.slot), hook, expected);
    DWORD ignoredProtection = 0;
    const bool protectionRestored = VirtualProtect(binding.slot,
        sizeof(DWORD), oldProtection, &ignoredProtection) != 0;
    if (observed != expected) {
        error = std::string("item-price IAT changed after validation in ") +
            binding.moduleName;
        return false;
    }
    binding.installed = true;
    if (!protectionRestored) {
        error = std::string("item-price IAT patched but page protection restore failed in ") +
            binding.moduleName;
        return false;
    }
    return true;
}
bool RestoreMarketPriceIat(MarketPriceIat& binding, std::string& error) {
    if (!binding.installed) return true;
    HMODULE module = GetModuleHandleA(binding.moduleName);
    if (!module || reinterpret_cast<const BYTE*>(module) != binding.image) {
        binding.image = nullptr;
        binding.slot = nullptr;
        binding.original = 0;
        binding.installed = false;
        return true;
    }
    if (!Readable(binding.slot, sizeof(DWORD))) {
        error = std::string("item-price IAT became unreadable in ") +
            binding.moduleName;
        return false;
    }
    DWORD oldProtection = 0;
    if (!VirtualProtect(binding.slot, sizeof(DWORD),
            PAGE_READWRITE, &oldProtection)) {
        error = std::string("cannot make item-price IAT writable for restore in ") +
            binding.moduleName;
        return false;
    }
    const PVOID expected = reinterpret_cast<PVOID>(
        static_cast<uintptr_t>(binding.original));
    const PVOID hook = reinterpret_cast<PVOID>(MarketItemPriceHook);
    const PVOID observed = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(binding.slot), expected, hook);
    DWORD ignoredProtection = 0;
    const bool protectionRestored = VirtualProtect(binding.slot,
        sizeof(DWORD), oldProtection, &ignoredProtection) != 0;
    binding.installed = false;
    if (observed != hook && observed != expected) {
        error = std::string("item-price IAT belongs to another hook in ") +
            binding.moduleName + "; left unchanged";
        return false;
    }
    if (!protectionRestored) {
        error = std::string("item-price IAT restored but page protection restore failed in ") +
            binding.moduleName;
        return false;
    }
    return true;
}
bool EnsureMarketPriceIats(std::string& error) {
    if (!originalMarketItemPrice) {
        error = "native item-price wrapper unavailable";
        return false;
    }
    MarketPriceIat* const imports[] = {
        &clientMarketPriceIat, &serverMarketPriceIat};
    std::array<MarketPriceIat*, 2> patched{};
    size_t patchedCount = 0;
    auto rollback = [&](const std::string& cause) {
        error = cause;
        while (patchedCount) {
            std::string restoreError;
            if (!RestoreMarketPriceIat(*patched[--patchedCount], restoreError))
                error += "; rollback failed: " + restoreError;
        }
        return false;
    };
    for (MarketPriceIat* binding : imports) {
        HMODULE module = GetModuleHandleA(binding->moduleName);
        if (!module)
            return rollback(std::string("offline image unavailable: ") +
                binding->moduleName);
        if (binding->image &&
            reinterpret_cast<const BYTE*>(module) != binding->image) {
            binding->image = nullptr;
            binding->slot = nullptr;
            binding->original = 0;
            binding->installed = false;
        }
        if (binding->installed) continue;
        if (!binding->slot && !PrepareMarketPriceIat(*binding, error))
            return rollback(error);
        patched[patchedCount++] = binding;
        if (!PatchMarketPriceIat(*binding, error))
            return rollback(error);
    }
    return true;
}
void RestoreMarketPriceIatsOrLog() {
    MarketPriceIat* const imports[] = {
        &serverMarketPriceIat, &clientMarketPriceIat};
    for (MarketPriceIat* binding : imports) {
        std::string error;
        if (!RestoreMarketPriceIat(*binding, error))
            OutputDebugStringA(("Roguelike market-price IAT restore failed: " +
                error + "\n").c_str());
    }
}
const ItemsTxt* MarketEquipmentBase(D2ItemDataTbl* bases,
    const sgptDataTable* itemTypes, DWORD code,
    int itemLevel, int playerLevel, int& baseId) {
    const ItemsTxt* base = D2COMMON_GetItemTextFromItemCode(code, &baseId);
    const int count = bases->nWeaponsTxtRecordCount + bases->nArmorTxtRecordCount;
    return base && baseId >= 0 && baseId < count &&
        base == bases->pItemsTxt + baseId && base->dwcode == code &&
        RewardBase(bases, itemTypes, baseId, itemLevel, playerLevel, false, 0) ? base : nullptr;
}
DWORD MarketStoreCode(void*, UnitAny*, DWORD code, int, const void*) {
    // Keep native stock bases unchanged; special tiers substitute only a validated matching base.
    return code;
}

bool MarketUniqueBaseEligible(const sgptDataTable* records, const ItemsTxt& base,
    int itemLevel, int playerLevel) {
    if (!records->dwUniqItemsRecs || records->dwUniqItemsRecs > 32768 ||
        !Readable(records->pUniqueItemsTxt,
            size_t(records->dwUniqItemsRecs) * sizeof(UniqueItemsTxt)))
        return false;
    for (unsigned i = 0; i < records->dwUniqItemsRecs; ++i) {
        const UniqueItemsTxt& row = records->pUniqueItemsTxt[i];
        if (!(row.dwUniqueItemFlags & 1) || row.wLvl > itemLevel ||
            row.wLvlReq > playerLevel) continue;
        if (row.dwCode == base.dwcode) return true;
    }
    return false;
}

bool MarketSetBaseEligible(const sgptDataTable* records, const ItemsTxt& base,
    int itemLevel, int playerLevel) {
    if (!records->dwSetItemsRecs || records->dwSetItemsRecs > 32768 ||
        !records->dwSetsRecs || records->dwSetsRecs > 32768 ||
        !Readable(records->pSetItemsTxt,
            size_t(records->dwSetItemsRecs) * sizeof(SetItemsTxt)) ||
        !Readable(records->pSetsTxt,
            size_t(records->dwSetsRecs) * sizeof(SetsTxt)))
        return false;
    for (unsigned i = 0; i < records->dwSetItemsRecs; ++i) {
        const SetItemsTxt& row = records->pSetItemsTxt[i];
        if (row.nSetId >= records->dwSetsRecs || row.wLvl > itemLevel ||
            row.wLvlReq > playerLevel) continue;
        if (row.dwCode == base.dwcode) return true;
    }
    return false;
}

bool MarketSpecialEligible(const sgptDataTable* records, int quality,
    const ItemsTxt& base, int itemLevel, int playerLevel) {
    if (!Readable(records, sizeof(*records))) return false;
    if (quality == 7)
        return MarketUniqueBaseEligible(records, base, itemLevel, playerLevel);
    if (quality == 5)
        return MarketSetBaseEligible(records, base, itemLevel, playerLevel);
    return false;
}
// The pinned S13 RNG returns values in [lower, upper); upper is exclusive.
__declspec(naked) int __stdcall MarketNativeRandom(DWORD* seed, int lower, int upper) {
    __asm {
        // D2Game uses EDI for its lower bound; preserve the C++ caller's EDI.
        push edi
        mov ecx, [esp + 8]
        mov edi, [esp + 12]
        mov eax, [esp + 16]
        call dword ptr [originalMarketRandom]
        pop edi
        ret 12
    }
}
DWORD* MarketStoreSeed(void* game) {
    const BYTE* bytes = static_cast<const BYTE*>(game);
    if (!Readable(bytes + 0x1D24, sizeof(void*))) return nullptr;
    const BYTE* control = *reinterpret_cast<const BYTE* const*>(bytes + 0x1D24);
    if (!Readable(control, 16)) return nullptr;
    DWORD* seed = reinterpret_cast<DWORD*>(const_cast<BYTE*>(control + 8));
    MEMORY_BASIC_INFORMATION memory{};
    const DWORD protection = VirtualQuery(seed, &memory, sizeof(memory)) ?
        memory.Protect & 0xff : 0;
    return memory.State == MEM_COMMIT &&
        (protection & (PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ? seed : nullptr;
}
template <typename Visit>
bool ForEachMarketSpecialBase(const D2ItemDataTbl* bases,
    const sgptDataTable* records, int quality, int itemLevel, int playerLevel,
    bool weapons, Visit visit) {
    const int count = bases->nWeaponsTxtRecordCount + bases->nArmorTxtRecordCount;
    const auto accept = [&](const char* itemCode) {
        DWORD code = 0;
        memcpy(&code, itemCode, sizeof(code));
        int baseId = -1;
        const ItemsTxt* base = D2COMMON_GetItemTextFromItemCode(code, &baseId);
        if (!base || baseId < 0 || baseId >= count ||
            base != bases->pItemsTxt + baseId || base->dwcode != code ||
            (baseId < bases->nWeaponsTxtRecordCount) != weapons ||
            !RewardBase(bases, records, baseId, itemLevel, playerLevel, false, 0))
            return;
        visit(code);
    };
    if (quality == 7) {
        if (!records->dwUniqItemsRecs || records->dwUniqItemsRecs > 32768 ||
            !Readable(records->pUniqueItemsTxt,
                size_t(records->dwUniqItemsRecs) * sizeof(UniqueItemsTxt)))
            return false;
        for (unsigned i = 0; i < records->dwUniqItemsRecs; ++i) {
            const UniqueItemsTxt& row = records->pUniqueItemsTxt[i];
            if ((row.dwUniqueItemFlags & 1) && row.wLvl <= itemLevel &&
                row.wLvlReq <= playerLevel) accept(row.szCode);
        }
        return true;
    }
    if (quality == 5) {
        if (!records->dwSetItemsRecs || records->dwSetItemsRecs > 32768 ||
            !records->dwSetsRecs || records->dwSetsRecs > 32768 ||
            !Readable(records->pSetItemsTxt,
                size_t(records->dwSetItemsRecs) * sizeof(SetItemsTxt)) ||
            !Readable(records->pSetsTxt,
                size_t(records->dwSetsRecs) * sizeof(SetsTxt)))
            return false;
        for (unsigned i = 0; i < records->dwSetItemsRecs; ++i) {
            const SetItemsTxt& row = records->pSetItemsTxt[i];
            if (row.nSetId < records->dwSetsRecs && row.wLvl <= itemLevel &&
                row.wLvlReq <= playerLevel) accept(row.szCode);
        }
        return true;
    }
    return false;
}
DWORD MarketSpecialBase(D2ItemDataTbl* bases, const sgptDataTable* records,
    int quality, int itemLevel, int playerLevel, bool weapons, DWORD* seed) {
    unsigned count = 0;
    if (!ForEachMarketSpecialBase(bases, records, quality, itemLevel, playerLevel,
            weapons, [&](DWORD) { ++count; }) || !count)
        return 0;
    const int selected = MarketNativeRandom(seed, 0, static_cast<int>(count));
    if (selected < 0 || static_cast<unsigned>(selected) >= count) return 0;
    unsigned index = 0;
    DWORD code = 0;
    ForEachMarketSpecialBase(bases, records, quality, itemLevel, playerLevel,
        weapons, [&](DWORD candidate) {
            if (index++ == static_cast<unsigned>(selected)) code = candidate;
        });
    return code;
}

DWORD MarketCompatibleSpecialBase(D2ItemDataTbl* bases,
    const sgptDataTable* records, int quality, int itemLevel,
    int playerLevel, bool weapons, DWORD* seed) {
    const DWORD replacement = MarketSpecialBase(bases, records, quality,
        itemLevel, playerLevel, weapons, seed);
    if (!replacement) return 0;
    int replacementId = -1;
    const ItemsTxt* replacementBase = MarketEquipmentBase(
        bases, records, replacement, itemLevel, playerLevel, replacementId);
    return replacementBase && MarketSpecialEligible(records, quality,
        *replacementBase, itemLevel, playerLevel) ? replacement : 0;
}

int MarketStoreQuality(void* game, UnitAny* npc, DWORD code, int itemLevel,
    int playerLevel, int nativeQuality, const void* caller, DWORD* outputCode) {
    if (!MarketStoreRequest(game, npc, itemLevel, caller) ||
        playerLevel < 1 || playerLevel > 99 || !originalMarketRandom || !outputCode)
        return nativeQuality;
    D2ItemDataTbl* bases = nullptr;
    sgptDataTable* records = nullptr;
    if (!RewardTables(bases, records)) return nativeQuality;
    int baseId = -1;
    const ItemsTxt* base = MarketEquipmentBase(
        bases, records, code, itemLevel, playerLevel, baseId);
    DWORD* seed = MarketStoreSeed(game);
    if (!base || !seed) return nativeQuality;
    const int roll = MarketNativeRandom(seed, 0, static_cast<int>(MerchantQuality::kBasisPoints));
    if (roll < 0 || roll >= static_cast<int>(MerchantQuality::kBasisPoints))
        return nativeQuality;
    const auto tier = MerchantQuality::Select(playerLevel,
        static_cast<unsigned>(roll));
    int quality = MerchantQuality::ToItemQuality(tier,
        nativeQuality);
    if (quality == 5 || quality == 7) {
        if (!MarketSpecialEligible(records, quality, *base, itemLevel, playerLevel)) {
            const DWORD replacement = MarketCompatibleSpecialBase(bases, records,
                quality, itemLevel, playerLevel,
                baseId < bases->nWeaponsTxtRecordCount, seed);
            if (replacement) *outputCode = replacement;
            else quality = 6;
        }
    }
    return quality;
}
// Preserve the special ECX code and EAX game inputs to the native constructor.
// The fourth stock argument is the native item level at [ESP+16].
__declspec(naked) void StoreItemHook() {
    __asm {
        pushad
        mov edx, [esp + 32]
        mov ebx, [esp + 48]
        push edx
        push ebx
        push ecx
        push dword ptr [esp + 48]
        push eax
        call MarketStoreCode
        add esp, 20
        mov [esp + 24], eax
        lea eax, [esp + 24]
        push eax
        mov eax, [esp + 32]
        mov ebx, [esp + 40]
        mov ecx, [esp + 28]
        mov edx, [esp + 52]
        mov esi, [esp + 56]
        mov edi, [esp + 48]
        mov ebp, [esp + 36]
        push ebp
        push edi
        push esi
        push edx
        push ecx
        push ebx
        push eax
        call MarketStoreQuality
        add esp, 32
        mov [esp + 44], eax
        popad
        jmp dword ptr [originalStoreItem]
    }
}
// S13 NPC store inventory at BF34D calls the shared normal-difficulty
// stock-level cap at BCB20 with ECX=game, EAX=NPC and EDI=player level+5.
// Uncap only that exact call in an owned market; all other callers retain
// stock behavior, including native hire price/level calculations.
void* originalStockLevel = nullptr;
bool MarketStockCall(void* game, UnitAny* npc, int level, const void* caller) {
    const BYTE* image = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2Game.dll"));
    if (!image || caller != image + 0xBF352 || level < 1 || level > 104 ||
        game != runGame.load(std::memory_order_acquire) || !npc ||
        !Readable(npc, sizeof(UnitAny)) ||
        (npc->dwTxtFileNo != 148 && npc->dwTxtFileNo != 154 &&
         npc->dwTxtFileNo != 147) || !Readable(npc->pPath, sizeof(Path)) ||
        !Readable(npc->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(npc->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(npc->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)))
        return false;
    const int area = npc->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
    return area >= 13 && area < 255 && zoneGame == game &&
        runStage.load(std::memory_order_acquire) == area &&
        zoneSlots[area].configured &&
        zoneSlots[area].kind == Pending::ZoneKind::Merchant &&
        runCleared[area];
}
__declspec(naked) void StockLevelHook() {
    __asm {
        pushad
        push dword ptr [esp + 32]
        push edi
        push eax
        push ecx
        call MarketStockCall
        add esp, 16
        test al, al
        popad
        jnz uncap
        jmp dword ptr [originalStockLevel]
    uncap:
        mov eax, edi
        cmp eax, 99
        jle done
        mov eax, 99
    done:
        ret
    }
}
using StockFill = void (__stdcall*)(void*, UnitAny*, UnitAny*, void*);
StockFill originalStockFill = nullptr;
unsigned observedAkaraPerms = 0, observedAkaraPotions = 0;
unsigned filteredAkaraPotions = 0;
bool observedAkaraTrade = false;
void __stdcall StockFillHook(void* game, UnitAny* player, UnitAny* npc,
    void* trade) {
    const BYTE* image = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2Game.dll"));
    const bool ownedAkaraMarket = image && player && Readable(player, sizeof(UnitAny)) &&
        npc && Readable(npc, sizeof(UnitAny)) && npc->dwTxtFileNo == 148 &&
        MarketStockCall(game, npc,
            D2COMMON_GetUnitStat(player, STAT_LEVEL, 0) + 5, image + 0xBF352);
    if (ownedAkaraMarket) {
        const BYTE* bytes = reinterpret_cast<const BYTE*>(game);
        if (Readable(bytes + 0x1D24, sizeof(void*))) {
            const BYTE* control = *reinterpret_cast<const BYTE* const*>(bytes + 0x1D24);
            if (Readable(control, 8)) {
                const BYTE* records = *reinterpret_cast<const BYTE* const*>(control + 4);
                if (Readable(records, 64 * 0x44))
                    for (unsigned i = 0; i < 64; ++i) {
                        const BYTE* record = records + i * 0x44;
                        if (*reinterpret_cast<const DWORD*>(record) != 148) continue;
                        observedAkaraTrade = trade == record + 0x20;
                        const unsigned count = *reinterpret_cast<const DWORD*>(record + 0x38);
                        const DWORD* codes = *reinterpret_cast<const DWORD* const*>(record + 0x34);
                        if (observedAkaraTrade && count < 128 &&
                            Readable(codes, count * sizeof(DWORD))) {
                            observedAkaraPerms = count;
                            observedAkaraPotions = 0;
                            for (unsigned n = 0; n < count; ++n) {
                                const char* code = reinterpret_cast<const char*>(codes + n);
                                if ((code[0] == 'h' || code[0] == 'm') &&
                                    code[1] == 'p' && code[2] >= '1' && code[2] <= '5')
                                    ++observedAkaraPotions;
                            }
                            const int level = D2COMMON_GetUnitStat(
                                player, STAT_LEVEL, 0);
                            MEMORY_BASIC_INFORMATION writable{};
                            if (observedAkaraPotions == 10 &&
                                level >= 1 && level <= 99 &&
                                VirtualQuery(record + 0x34, &writable, sizeof(writable)) &&
                                writable.State == MEM_COMMIT &&
                                (writable.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                                std::array<DWORD, 128> filtered;
                                unsigned kept = 0, removed = 0;
                                for (unsigned n = 0; n < count; ++n) {
                                    const char* code = reinterpret_cast<const char*>(codes + n);
                                    const bool potion = (code[0] == 'h' || code[0] == 'm') &&
                                        code[1] == 'p' && code[2] >= '1' && code[2] <= '5';
                                    if (potion && code[2] - '1' > level / 10)
                                        ++removed;
                                    else
                                        filtered[kept++] = codes[n];
                                }
                                if (removed) {
                                    BYTE* writableRecord = const_cast<BYTE*>(record);
                                    *reinterpret_cast<const DWORD**>(writableRecord + 0x34) =
                                        filtered.data();
                                    *reinterpret_cast<DWORD*>(writableRecord + 0x38) = kept;
                                    originalStockFill(game, player, npc, trade);
                                    *reinterpret_cast<const DWORD**>(writableRecord + 0x34) = codes;
                                    *reinterpret_cast<DWORD*>(writableRecord + 0x38) = count;
                                    filteredAkaraPotions += removed;
                                    return;
                                }
                            }
                        }
                        break;
                    }
            }
        }
    }
    originalStockFill(game, player, npc, trade);
}
Create originalNaturalDrop = nullptr;
UnitAny* __stdcall NaturalDropHook(void* game, Drop* drop, int flags);
bool InstallChoiceNames(std::string& error) {
    if (unitNameInstalled) return true;
    if (!LabProfile::VerifyClient(error) || !LabProfile::VerifyServer(error) ||
        !LabProfile::VerifyCommon(error)) return false;
    HMODULE common = GetModuleHandleA("D2Common.dll");
    BYTE* commonImage = reinterpret_cast<BYTE*>(common);
    FARPROC quantitySetter = common ?
        GetProcAddress(common, MAKEINTRESOURCEA(10887)) : nullptr;
    if (!commonImage || !quantitySetter ||
        quantitySetter != reinterpret_cast<FARPROC>(commonImage + 0x3A740)) {
        error = "unexpected pinned S13 vendor quantity setter";
        return false;
    }
    setVendorItemStat = reinterpret_cast<SetUnitStat>(quantitySetter);
    BYTE* client = reinterpret_cast<BYTE*>(GetModuleHandleA("D2Client.dll"));
    const BYTE signature[] = {0x83,0xec,0x08,0x56,0x8b,0xf0,0x85,0xf6};
    BYTE* gameImage = reinterpret_cast<BYTE*>(GetModuleHandleA("D2Game.dll"));
    const BYTE capSignature[] = {0x8a,0x51,0x6d,0x83,0xec,0x18,0x84,0xd2};
    const BYTE stockCall[] = {0xe8,0xce,0xd7,0xff,0xff};
    const BYTE randomSignature[] = {
        0x56,0x8b,0xf0,0x3b,0xf7,0x7f,0x04,0x8b,0xc7,0x5e,0xc3};
    if (!gameImage ||
        !Readable(gameImage + 0x1780, sizeof(randomSignature)) ||
        memcmp(gameImage + 0x1780, randomSignature, sizeof(randomSignature)) ||
        !Readable(gameImage + 0xBCB20, sizeof(capSignature)) ||
        memcmp(gameImage + 0xBCB20, capSignature, sizeof(capSignature)) ||
        !Readable(gameImage + 0xBF34D, sizeof(stockCall)) ||
        memcmp(gameImage + 0xBF34D, stockCall, sizeof(stockCall))) {
        error = "unrecognized pinned S13 store level cap, RNG, or caller";
        return false;
    }
    originalStockLevel = gameImage + 0xBCB20;
    originalMarketRandom = gameImage + 0x1780;
    const BYTE constructor[] = {0x83,0xec,0x08,0x56,0x57,0x8b,0xf8,0x85,0xff};
    // Only normal/magic equipment calls are covered; the permanent-stock
    // call at BF5C9 differs from the pinned image at runtime.
    const BYTE itemCalls[][5] = {
        {0xe8,0xcc,0xf9,0xff,0xff}, {0xe8,0xb9,0xf8,0xff,0xff}};
    const uintptr_t itemCallRvas[] = {0xBF45F, 0xBF572};
    if (!Readable(gameImage + 0xBEE30, sizeof(constructor)) ||
        memcmp(gameImage + 0xBEE30, constructor, sizeof(constructor))) {
        error = "unrecognized pinned S13 store item constructor"; return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!Readable(gameImage + itemCallRvas[i], sizeof(itemCalls[i]))) {
            error = "unreadable pinned S13 store item caller " + std::to_string(i);
            return false;
        }
        if (memcmp(gameImage + itemCallRvas[i], itemCalls[i], sizeof(itemCalls[i]))) {
            error = "unrecognized pinned S13 store item caller " +
                std::to_string(i) + " opcode " +
                std::to_string(gameImage[itemCallRvas[i]]);
            return false;
        }
    }
    originalStoreItem = gameImage + 0xBEE30;
    BYTE* naturalDrop = gameImage + 0x11490;
    const BYTE naturalPrefix[] = {
        0x55,0x8b,0x6c,0x24,0x0c,0x57,0x8b,0x7c,0x24,0x0c,0x8b,0x47,0x70};
    if (!Readable(naturalDrop, sizeof(naturalPrefix)) ||
        memcmp(naturalDrop, naturalPrefix, sizeof(naturalPrefix))) {
        error = "unrecognized pinned S13 natural item constructor"; return false;
    }
    originalNaturalDrop = reinterpret_cast<Create>(naturalDrop);
    BYTE* filler = gameImage + 0xBF2E0;
    const BYTE fillPrefix[] = {0x83,0xec,0x20,0x53,0x55,0x8b,0x6c,0x24,0x2c};
    if (!Readable(filler, sizeof(fillPrefix)) ||
        memcmp(filler, fillPrefix, sizeof(fillPrefix))) {
        error = "unrecognized pinned S13 stock filler"; return false;
    }
    originalStockFill = reinterpret_cast<StockFill>(filler);
    BYTE* getter = client ? client + 0xA5D90 : nullptr;
    BYTE* hover = client ? client + 0xC1085 : nullptr;
    const BYTE before[] = {0x8b,0x4d,0x14,0x0f,0xb6,0x71,0x04};
    const BYTE call[] = {0xe8,0xb6,0xd1,0xff,0xff};
    const BYTE after[] = {0x8b,0xd0,0x8d,0x8c,0x24,0x98,0x01,0x00,0x00};
    if (!getter || !Readable(getter, sizeof(signature)) ||
        memcmp(getter, signature, sizeof(signature))) {
        error = "unrecognized pinned S13 client unit-name getter"; return false;
    }
    if (!hover || !Readable(hover - sizeof(before),
            sizeof(before) + sizeof(call) + sizeof(after))) {
        error = "pinned S13 client hover callsite unreadable"; return false;
    }
    if (memcmp(hover - sizeof(before), before, sizeof(before))) {
        error = "pinned S13 client hover preceding bytes changed"; return false;
    }
    // Other pinned offline runtime hooks can rewrite this relative call.
    // Preserve its existing target through Detours' trampoline on fallthrough.
    const BYTE* destination = hover + sizeof(call) +
        *reinterpret_cast<const int32_t*>(hover + 1);
    MEMORY_BASIC_INFORMATION target{};
    const DWORD protection = VirtualQuery(destination, &target, sizeof(target))
        ? target.Protect & 0xff : 0;
    if (hover[0] != 0xe8 || target.State != MEM_COMMIT ||
        (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
         protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)) {
        error = "unrecognized pinned S13 client portal hover call target"; return false;
    }
    if (memcmp(hover + sizeof(call), after, sizeof(after))) {
        error = "pinned S13 client hover following bytes changed"; return false;
    }
    originalHoverCall = hover;
    hoverAfterCall = hover + sizeof(call);
    originalUnitName = getter;
    if (!D2COMMON_GetItemPrice) {
        error = "pinned S13 native item price function unavailable";
        return false;
    }
    originalMarketItemPrice = D2COMMON_GetItemPrice;
    if (!EnsureMarketPriceIats(error)) return false;
    if (DetourTransactionBegin() != NO_ERROR) {
        RestoreMarketPriceIatsOrLog();
        error = "cannot begin client name detour"; return false;
    }
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        DetourTransactionAbort(); RestoreMarketPriceIatsOrLog();
        error = "cannot update current thread for name detour"; return false;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        DetourTransactionAbort(); RestoreMarketPriceIatsOrLog();
        error = "cannot enumerate threads for name detour"; return false;
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
        DetourAttach(&originalUnitName, reinterpret_cast<void*>(UnitNameHook)) != NO_ERROR ||
        DetourAttach(&originalHoverCall, reinterpret_cast<void*>(PortalHoverHook)) != NO_ERROR ||
        DetourAttach(&originalStockLevel, reinterpret_cast<void*>(StockLevelHook)) != NO_ERROR ||
        DetourAttach(&originalStoreItem, reinterpret_cast<void*>(StoreItemHook)) != NO_ERROR ||
        DetourAttach(reinterpret_cast<void**>(&originalMarketItemPrice),
            reinterpret_cast<void*>(MarketItemPriceHook)) != NO_ERROR ||
        DetourAttach(reinterpret_cast<void**>(&originalNaturalDrop),
            reinterpret_cast<void*>(NaturalDropHook)) != NO_ERROR ||
        DetourAttach(reinterpret_cast<void**>(&originalStockFill),
            reinterpret_cast<void*>(StockFillHook)) != NO_ERROR) {
        DetourTransactionAbort();
        for (size_t i = 0; i < used; ++i) CloseHandle(threads[i]);
        originalUnitName = nullptr;
        originalHoverCall = nullptr;
        originalStockLevel = nullptr;
        originalStoreItem = nullptr;
        originalNaturalDrop = nullptr;
        originalStockFill = nullptr;
        RestoreMarketPriceIatsOrLog();
        error = "cannot attach pinned client, drop, and market-price detours"; return false;
    }
    const LONG committed = DetourTransactionCommit();
    for (size_t i = 0; i < used; ++i) CloseHandle(threads[i]);
    if (committed != NO_ERROR) {
        originalUnitName = nullptr;
        originalHoverCall = nullptr;
        originalStockLevel = nullptr;
        originalStoreItem = nullptr;
        originalNaturalDrop = nullptr;
        originalStockFill = nullptr;
        RestoreMarketPriceIatsOrLog();
        error = "cannot commit pinned client, drop, and market-price detours"; return false;
    }
    unitNameInstalled = true;
    return true;
}
Chat originalChat = nullptr;
Chat originalClick = nullptr;
// Only the pinned offline server's inventory (0x20), belt (0x26), and object (0x13) callbacks are replaced.
Chat originalUseItem = nullptr;
Chat originalUseBeltItem = nullptr;
std::atomic<bool> hookPairReady{false};
struct ArmedReroll {
    void* game = nullptr;
    DWORD playerId = 0;
    DWORD itemId = 0;
    int source = 0;
    std::string epoch;
    RoguelikeLoot::RerollEffect effect = RoguelikeLoot::RerollEffect::None;
};
ArmedReroll armedReroll; // Server-thread only. No client-supplied destination or object ID.
unsigned runItemUsePackets = 0;
int runItemUseStatus = 0; // 1 seen, 2 item, 3 table, 4 profile, 5 owner, 6 area, 7 run, 8 location, 9 clear, 10 reroll armed, 11 aura used, 12 aura rejected.
} // namespace
namespace ServerSpawns {
bool RerollOfferedChoice(void* game, UnitAny* player,
    const std::shared_ptr<Pending>& choice, DWORD objectId, DWORD itemId,
    RoguelikeLoot::RerollEffect effect);
}
namespace {
Create createItem = nullptr;
RollAffix rollAffix = nullptr;

json Failure(const std::string& reason) { return {{"ok", false}, {"error", reason}}; }
bool Integer(const json& value, int64_t minimum, int64_t maximum, int64_t& output) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) {
        uint64_t n = value.get<uint64_t>();
        if (n > static_cast<uint64_t>(maximum)) return false;
        output = static_cast<int64_t>(n);
    } else output = value.get<int64_t>();
    return output >= minimum && output <= maximum;
}
bool Readable(const void* pointer, size_t size) {
    MEMORY_BASIC_INFORMATION memory = {};
    if (!pointer || !VirtualQuery(pointer, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    uintptr_t begin = reinterpret_cast<uintptr_t>(pointer), end = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return begin <= end && size <= end - begin;
}
bool SetSelectorReady() {
    HMODULE server = GetModuleHandleA("D2Game.dll");
    if (!server) return false;
    static const BYTE selector[] = {0x8b,0x8c,0x24,0x24,0x08,0x00,0x00,0x8b,
        0x49,0x40,0x85,0xc9,0x76,0x09,0x49,0x3b,0xca,0x75,0x04,0x89,0x54,0x24,0x10};
    static const BYTE identity[] = {0x8b,0x44,0xc4,0x1c,0x8b,0xb4,0x24,0x20,
        0x08,0x00,0x00,0x50,0x56,0xe8,0x01,0x74,0xff,0xff};
    const BYTE* base = reinterpret_cast<const BYTE*>(server);
    return Readable(base + 0x13cc7, sizeof(selector)) &&
        Readable(base + 0x13d97, sizeof(identity)) &&
        !std::memcmp(base + 0x13cc7, selector, sizeof(selector)) &&
        !std::memcmp(base + 0x13d97, identity, sizeof(identity));
}
bool ExecutableOwned(const void* pointer, HMODULE server) {
    MEMORY_BASIC_INFORMATION memory = {};
    if (!VirtualQuery(pointer, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    return memory.AllocationBase == server || memory.AllocationBase == GetModuleHandleA("ProjectDiablo.dll");
}
std::string Code(const ItemsTxt& item) { return GetItemCode(item.szCode); }
sgptDataTable* Tables() {
    if (!Readable(p_D2COMMON_sgptDataTable, sizeof(sgptDataTable*))) return nullptr;
    sgptDataTable* tables = *p_D2COMMON_sgptDataTable;
    return Readable(tables, sizeof(*tables)) ? tables : nullptr;
}
bool ExactAffixes(UnitAny* item, const Pending& request) {
    // The native rare constructor supplies valid quality/name/base state but
    // fills unrequested slots. Rebuild only its modifier list using native
    // removal and affix application, retaining the native base and automod.
    StatList* modifiers = D2COMMON_GetItemPropertyList(item, 0, 0x40);
    if (modifiers) {
        if (!Readable(modifiers, 0x2C) || modifiers->wStatCount1 > 256) return false;
        while (modifiers->wStatCount1) {
            if (!Readable(modifiers->pStat, sizeof(Stat))) return false;
            const WORD before = modifiers->wStatCount1;
            const WORD stat = modifiers->pStat[0].wStatIndex;
            const WORD layer = modifiers->pStat[0].wSubIndex;
            // Measured ordinal10188 backend3A280 removes the stored node at0.
            D2COMMON_SetPropertyStat(modifiers, stat, 0, layer);
            if (modifiers->wStatCount1 >= before) return false;
        }
    }
    D2MagicAffixDataTbl* tables = D2COMMON_10492_DATATBLS_GetMagicAffixDataTables();
    if (!tables || !tables->pMagicAffixTxt || !tables->pMagicPrefix ||
        !tables->pMagicSuffix || !tables->pAutoMagic) return false;
    for (unsigned i = 0; i < 3; ++i)
        item->pItemData->wPrefix[i] = item->pItemData->wSuffix[i] = 0;
    const unsigned automatic = item->pItemData->wAutoPrefix;
    if (automatic) {
        if (automatic > static_cast<unsigned>(tables->nMagicAffixTxtRecordCount)) return false;
        const AutoMagicTxt& record = tables->pMagicAffixTxt[automatic - 1];
        const int offset = static_cast<int>(tables->pAutoMagic - tables->pMagicAffixTxt);
        if (rollAffix(item, 1, 1, 1, 1, automatic - offset, record.dwGroup) != automatic) return false;
    }
    for (bool prefix : {true, false}) {
        const auto& wanted = prefix ? request.prefixes : request.suffixes;
        const int offset = static_cast<int>((prefix ? tables->pMagicPrefix : tables->pMagicSuffix) - tables->pMagicAffixTxt);
        for (unsigned i = 0; i < 3 && wanted[i]; ++i) {
            const WORD actual = rollAffix(item, 1, 1, 1, prefix ? 1 : 0, wanted[i] - offset, 0);
            if (actual != wanted[i]) return false;
            (prefix ? item->pItemData->wPrefix : item->pItemData->wSuffix)[i] = actual;
        }
    }
    return true;
}
json Generate(void* game, UnitAny* player, const Pending& request,
    UnitAny* dropper = nullptr, bool forceSockets = false,
    DWORD socketSeed = 0) {
    if (!request.expectedEpoch.empty() &&
        !ObservationTelemetry::MatchesContext(request.expectedEpoch, game, player))
        return {{"ok", false}, {"code", "stale_epoch"}, {"error", "session epoch changed"},
            {"request_id", request.id}};
    if (!game || !player || player->dwType != UNIT_PLAYER || player->dwUnitId != request.playerId ||
        !player->pPath || !player->pPath->pRoom1) return Failure("server player context changed");
    UnitAny* location = dropper ? dropper : player;
    if (dropper && (!Readable(dropper, sizeof(UnitAny)) ||
        dropper->dwType != UNIT_MONSTER || !dropper->pPath ||
        !Readable(dropper->pPath, sizeof(Path)) || !dropper->pPath->pRoom1))
        return Failure("server monster drop anchor changed");
    Room1* room = D2COMMON_GetRoomFromUnit(location);
    if (!room || !room->Coll) return Failure("server collision room unavailable");
    CollMap* collision = room->Coll;
    uint64_t cells = uint64_t(collision->dwSizeGameX) * collision->dwSizeGameY;
    if (!cells || cells > 65536 || !Readable(collision->pMapStart, static_cast<size_t>(cells) * sizeof(WORD)))
        return Failure("server collision map invalid");
    int x = location->pPath->xPos, y = location->pPath->yPos;
    bool placement = false;
    for (int radius = 0; radius <= 4 && !placement; ++radius) {
        for (int dy = -radius; dy <= radius && !placement; ++dy) {
            for (int dx = -radius; dx <= radius && !placement; ++dx) {
                int px = x + dx, py = y + dy;
                if (px < static_cast<int>(collision->dwPosGameX) || py < static_cast<int>(collision->dwPosGameY)) continue;
                unsigned cx = px - collision->dwPosGameX, cy = py - collision->dwPosGameY;
                if (cx >= collision->dwSizeGameX || cy >= collision->dwSizeGameY ||
                    (collision->pMapStart[cy * collision->dwSizeGameX + cx] & 0x3E01)) continue;
                x = px; y = py; placement = true;
            }
        }
    }
    if (!placement) return Failure("no free nearby ground tile in the loaded server room");
    Drop drop = {};
    drop.unit = location; drop.game = game; drop.level = request.level;
    drop.baseId = request.baseId; drop.spawnType = ITEM_MODE_ON_GROUND;
    drop.x = x; drop.y = y; drop.room = room;
    // Match native create-then-drop ordering: complete socket stats before room registration.
    drop.initFlags = 1;
    // Every measured constructor wrapper reads this WORD from Game + 0x78.
    drop.format = *reinterpret_cast<WORD*>(static_cast<BYTE*>(game) + 0x78);
    if (request.setId >= 0 && drop.format < 1) return Failure("set item constructor requires expansion item format");
    drop.quality = request.quality; drop.uniqueIndex = (request.quality == 5 ? request.setId : request.uniqueId) + 1;
    if (forceSockets) {
        // Use the pinned native forced-drop path so socket seeds and flags are applied before publication.
        drop.force = TRUE;
        drop.flags = ITEM_HASSOCKETS;
        // The forced constructor takes durability from these fields; zero makes it indestructible.
        const ItemsTxt* baseText = D2COMMON_GetItemText(request.baseId);
        if (!Readable(baseText, sizeof(*baseText)))
            return Failure("forced socket base item data unavailable");
        const unsigned maximumDurability =
            baseText->bnodurability ? 0u : baseText->bdurability;
        drop.maxDurability = static_cast<int>(maximumDurability);
        drop.minDurability = static_cast<int>(
            RoguelikeLoot::StartingDurability(maximumDurability,
                socketSeed));
    }
    // Keep both constructor seed fields aligned for maximum-socket drops.
    drop.seed = forceSockets ? socketSeed : request.seed;
    drop.itemSeed = forceSockets ? socketSeed : request.seed;
    drop.generationFlags = RoguelikeLoot::DropGenerationFlags(forceSockets);
    if (request.quality == 4 || request.quality == 6) {
        D2MagicAffixDataTbl* affixes = D2COMMON_10492_DATATBLS_GetMagicAffixDataTables();
        if (!affixes || !affixes->pMagicAffixTxt || !affixes->pMagicPrefix || !affixes->pMagicSuffix)
            return Failure("server affix tables unavailable");
        // Public IDs and stored item affixes are global one-based records.
        // The constructor's preferred indices are relative to each kind table.
        const int prefixOffset = static_cast<int>(affixes->pMagicPrefix - affixes->pMagicAffixTxt);
        const int suffixOffset = static_cast<int>(affixes->pMagicSuffix - affixes->pMagicAffixTxt);
        for (unsigned i = 0; i < 3; ++i) {
            drop.prefixes[i] = request.prefixes[i] ? request.prefixes[i] - prefixOffset : 0;
            drop.suffixes[i] = request.suffixes[i] ? request.suffixes[i] - suffixOffset : 0;
        }
    }
    if (request.quality == 5 && !SetSelectorReady())
        return Failure("S13 set selector bindings changed before construction");
    Create constructor = originalNaturalDrop ? originalNaturalDrop : createItem;
    if (!constructor) return Failure("native server item constructor unavailable");
    UnitAny* item = constructor(game, &drop, 1);
    if (!item || !item->pItemData) return Failure("native server constructor returned no item");
    D2COMMON_SetItemFlag(item, 0x2000, FALSE); // not a vendor-store item
    D2COMMON_SetItemFlag(item, 0x10, TRUE);
    json result = {{"ok", true}, {"status", "spawned"}, {"request_id", request.id},
        {"item_id", item->dwUnitId}, {"class_id", item->dwTxtFileNo}, {"quality", item->pItemData->dwQuality},
        {"file_index", item->pItemData->dwFileIndex}, {"server_thread_id", GetCurrentThreadId()},
        {"position", {{"x", x}, {"y", y}}}, {"prefixes", item->pItemData->wPrefix}, {"suffixes", item->pItemData->wSuffix}};
    const ItemsTxt* base = D2COMMON_GetItemText(item->dwTxtFileNo);
    result["code"] = base ? json(Code(*base)) : json(nullptr);
    if (item->dwMode != ITEM_MODE_ON_GROUND || item->pItemData->dwQuality != static_cast<DWORD>(request.quality) ||
        ((request.uniqueId >= 0 && item->pItemData->dwFileIndex != static_cast<DWORD>(request.uniqueId)) ||
         (request.setId >= 0 && item->pItemData->dwFileIndex != static_cast<DWORD>(request.setId)))) {
        result["ok"] = false; result["error"] = "created item identity or ground mode does not match; inspect item_id, do not retry";
        return result;
    }
    if (request.setId >= 0) {
        result["set_item_id"] = request.setId;
        sgptDataTable* tables = Tables();
        if (!tables || request.setId >= static_cast<int>(tables->dwSetItemsRecs) ||
            !Readable(tables->pSetItemsTxt, size_t(tables->dwSetItemsRecs) * sizeof(SetItemsTxt))) {
            result["ok"] = false; result["error"] = "created set item table no longer readable; inspect item_id";
            return result;
        }
        const SetItemsTxt& selected = tables->pSetItemsTxt[request.setId];
        result["set_item_name"] = std::string(selected.szName, strnlen(selected.szName, sizeof(selected.szName)));
        if (!Readable(base, sizeof(*base)) || Code(*base) != GetItemCode(selected.szCode)) {
            result["ok"] = false; result["error"] = "created set item base does not match selected table row; inspect item_id, do not retry";
            return result;
        }
    }
    if (request.exactAffixes) {
        if (!ExactAffixes(item, request)) {
            result["ok"] = false;
            result["error"] = "native exact-affix application failed; inspect item_id, do not retry";
            return result;
        }
        result["prefixes"] = item->pItemData->wPrefix;
        result["suffixes"] = item->pItemData->wSuffix;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if ((request.exactAffixes && item->pItemData->wPrefix[i] != request.prefixes[i]) ||
            (request.exactAffixes && item->pItemData->wSuffix[i] != request.suffixes[i])) {
            result["ok"] = false; result["error"] = "constructor did not materialize the requested affix set; inspect item_id, do not retry";
            return result;
        }
    }
    if (!request.properties.empty()) {
        StatList* list = D2COMMON_GetItemPropertyList(item, 0, 0x40);
        if (!list) {
            list = D2COMMON_AllocatePropertyList(reinterpret_cast<void*>(item->_1), 0x40, 0, UNIT_ITEM, item->dwUnitId);
            if (list) D2COMMON_AttachPropertyList(item, list, TRUE);
        }
        if (!list) { result["ok"] = false; result["error"] = "created item modifier allocation failed; inspect item_id"; return result; }
        for (const auto& property : request.properties)
            D2COMMON_SetPropertyStat(list, property.id, property.value, property.layer);
    }
    result["properties"] = json::array();
    for (const auto& property : request.properties)
        result["properties"].push_back({{"stat_id", property.id}, {"layer", property.layer},
            {"value", D2COMMON_GetUnitStat(item, property.id, property.layer)}});
    return result;
}
std::string_view ChatMessageText(BYTE* packet, int size) {
    if (!packet || size < 4 || size > 128 || packet[0] != 0x15 ||
        packet[1] != 1 || packet[2] != 0) return {};
    const char* text = reinterpret_cast<const char*>(packet + 3);
    const char* end = static_cast<const char*>(memchr(text, 0, size - 3));
    return end ? std::string_view(text, static_cast<size_t>(end - text)) : std::string_view{};
}
bool SelectSpecificItemSpawn(Pending& request, std::string_view selector, uint32_t id, std::string& code) {
    sgptDataTable* tables = Tables();
    if (!tables) return false;
    if (selector == "unique") {
        if (!tables->dwUniqItemsRecs || tables->dwUniqItemsRecs > 32768 || id >= tables->dwUniqItemsRecs ||
            !Readable(tables->pUniqueItemsTxt, tables->dwUniqItemsRecs * sizeof(UniqueItemsTxt))) return false;
        const UniqueItemsTxt& item = tables->pUniqueItemsTxt[id];
        if (!(item.dwUniqueItemFlags & 1) || item.wLvl > request.level) return false;
        request.quality = 7;
        request.uniqueId = static_cast<int>(id);
        code = GetItemCode(item.szCode);
        return true;
    }
    if (selector != "set" || !tables->dwSetItemsRecs || tables->dwSetItemsRecs > 32768 ||
        !tables->dwSetsRecs || tables->dwSetsRecs > 32768 || id >= tables->dwSetItemsRecs ||
        !Readable(tables->pSetItemsTxt, size_t(tables->dwSetItemsRecs) * sizeof(SetItemsTxt)) ||
        !Readable(tables->pSetsTxt, size_t(tables->dwSetsRecs) * sizeof(SetsTxt))) return false;
    const SetItemsTxt& item = tables->pSetItemsTxt[id];
    if (item.nSetId >= tables->dwSetsRecs || item.wLvl > request.level) return false;
    request.quality = 5;
    request.setId = static_cast<int>(id);
    code = GetItemCode(item.szCode);
    return true;
}
bool SelectBaseItemSpawn(Pending& request, std::string_view selector, std::string_view argument,
    std::string& code) {
    if (selector.size() < 3 || selector.size() > 4) return false;
    for (unsigned char c : selector) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
        code.push_back(static_cast<char>(c));
    }
    if (argument.empty()) return true;
    if (argument == "magic") request.quality = 4;
    else if (argument == "rare") request.quality = 6;
    else return false;
    return true;
}
int SpawnableItemBaseId(const std::string& code) {
    D2ItemDataTbl* bases = D2COMMON_10535_DATATBLS_GetItemDataTables();
    if (!bases || bases->nItemsTxtRecordCount <= 0 || bases->nItemsTxtRecordCount > 32768 ||
        !Readable(bases->pItemsTxt, bases->nItemsTxtRecordCount * sizeof(ItemsTxt))) return -1;
    int baseId = -1;
    for (int i = 0; i < bases->nItemsTxtRecordCount; ++i) {
        if (Code(bases->pItemsTxt[i]) != code) continue;
        if (baseId >= 0) return -1;
        baseId = i;
    }
    return baseId >= 0 && bases->pItemsTxt[baseId].bspawnable ? baseId : -1;
}
bool SpawnItemFromChat(void* game, UnitAny* player, std::string_view command) {
    if (!player || player->dwType != UNIT_PLAYER || command.empty()) return false;
    const size_t separator = command.find(' ');
    const std::string_view selector = command.substr(0, separator);
    const std::string_view argument = separator == std::string_view::npos ?
        std::string_view{} : command.substr(separator + 1);
    if (selector.empty() || (separator != std::string_view::npos &&
        (argument.empty() || argument.find(' ') != std::string_view::npos))) return false;
    Pending request;
    request.id.assign(32, '0');
    request.playerId = player->dwUnitId;
    std::string code;
    if (selector == "unique" || selector == "set") {
        uint32_t id = 0;
        if (argument.empty()) return false;
        const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size()) return false;
        if (!SelectSpecificItemSpawn(request, selector, id, code)) return false;
    } else if (!SelectBaseItemSpawn(request, selector, argument, code)) return false;
    request.baseId = SpawnableItemBaseId(code);
    if (request.baseId < 0) return false;
    return Generate(game, player, request).value("ok", false);
}
bool SpawnMonsterFromChat(void* game, UnitAny* player, std::string_view text) {
    uint32_t classId = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), classId);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
    json spec = {{"class_id", classId}};
    json args = {{"request_id", std::string(32, '0')}, {"enemies", json::array({spec})}};
    ServerMonsters::Request request;
    std::string error;
    if (!ServerMonsters::Parse(args, ServerMonsters::Operation::Spawn, request, error)) return false;
    return ServerMonsters::Execute(game, player, request).value("ok", false);
}
bool SetPlayerFromChat(void* game, UnitAny* player, std::string_view command) {
    const size_t separator = command.find(' ');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == command.size() ||
        command.find(' ', separator + 1) != std::string_view::npos) return false;
    const std::string_view name = command.substr(0, separator);
    const std::string_view number = command.substr(separator + 1);
    CharacterState::ProgressionField field;
    if (name == "level") field = CharacterState::ProgressionField::Level;
    else if (name == "stats") field = CharacterState::ProgressionField::StatPoints;
    else if (name == "skills") field = CharacterState::ProgressionField::SkillPoints;
    else return false;
    uint32_t value = 0;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() &&
        CharacterState::SetProgression(game, player, field, value);
}
bool HandleGameChatCommand(void* game, UnitAny* player, BYTE* packet, int size) {
    const std::string_view text = ChatMessageText(packet, size);
    constexpr std::string_view itemPrefix = "/spawn-item ";
    constexpr std::string_view monsterPrefix = "/spawn-monster ";
    constexpr std::string_view playerPrefix = "/player ";
    if (text.size() > itemPrefix.size() &&
        text.substr(0, itemPrefix.size()) == itemPrefix)
        return SpawnItemFromChat(game, player, text.substr(itemPrefix.size()));
    if (text.size() > monsterPrefix.size() &&
        text.substr(0, monsterPrefix.size()) == monsterPrefix)
        return SpawnMonsterFromChat(game, player, text.substr(monsterPrefix.size()));
    if (text.size() > playerPrefix.size() &&
        text.substr(0, playerPrefix.size()) == playerPrefix)
        return SetPlayerFromChat(game, player, text.substr(playerPrefix.size()));
    return false;
}
UnitAny* FindRolledItem(void* game, DWORD id);
// Use zone item levels for drops; starter gear remains character-level eligible.
bool RewardLevelsValid(int itemLevel, int playerLevel) {
    return itemLevel >= 1 && itemLevel <= 99 &&
        playerLevel >= 1 && playerLevel <= 99;
}
bool RewardBaseRowEligible(const ItemsTxt& row, int itemLevel,
    int playerLevel, bool starter, RoguelikeLoot::BaseKind kind) {
    if (!row.bspawnable || row.bquest || row.bstackable ||
        (kind == RoguelikeLoot::BaseKind::Equipment && row.buseable))
        return false;
    if (!row.binvwidth || !row.binvheight) return false;
    if (!RoguelikeLoot::RewardBaseLevelsEligible(
            row.blevel, row.blevelreq, itemLevel, playerLevel, starter,
            kind == RoguelikeLoot::BaseKind::Charm))
        return false;
    return !starter || row.dwnormcode == row.dwcode;
}
const ItemsTxt* RewardBaseRow(const D2ItemDataTbl* table, int index,
    int itemLevel, int playerLevel, bool starter,
    RoguelikeLoot::BaseKind kind) {
    if (!table || !table->pItemsTxt || index < 0 ||
        index >= table->nItemsTxtRecordCount)
        return nullptr;
    if (!RewardLevelsValid(itemLevel, playerLevel)) return nullptr;
    const ItemsTxt* row = table->pItemsTxt + index;
    if (!Readable(row, sizeof(*row)) ||
        !RewardBaseRowEligible(*row, itemLevel, playerLevel, starter, kind))
        return nullptr;
    return row;
}
bool RewardBaseItemTypeValid(const sgptDataTable* itemTypes,
    const ItemsTxt& row) {
    if (!itemTypes || !itemTypes->pItemsTypeTxt ||
        !itemTypes->dwItemsTypeRecs || itemTypes->dwItemsTypeRecs > 32768 ||
        row.nType >= itemTypes->dwItemsTypeRecs)
        return false;
    return Readable(&itemTypes->pItemsTypeTxt[row.nType],
        sizeof(D2ItemTypesTxt));
}
bool RewardBase(const D2ItemDataTbl* table, const sgptDataTable* itemTypes,
    int index, int itemLevel, int playerLevel, bool starter, int sockets,
    RoguelikeLoot::BaseKind kind, int charmSize) {
    const ItemsTxt* base = RewardBaseRow(
        table, index, itemLevel, playerLevel, starter, kind);
    if (!base || !RewardBaseItemTypeValid(itemTypes, *base)) return false;
    const int equipmentCount =
        table->nWeaponsTxtRecordCount + table->nArmorTxtRecordCount;
    if (kind == RoguelikeLoot::BaseKind::Equipment)
        return index < equipmentCount &&
            (!sockets || base->bgemsockets >= sockets) &&
            (!starter || (base->wreqstr <= 25 && base->wreqdex <= 25));
    if (index < equipmentCount) return false;
    return RoguelikeLoot::RewardSpecialBaseCode(base->szCode, kind, charmSize);
}
bool RewardItemTypesValid(const sgptDataTable* records) {
    if (!records || !Readable(records, sizeof(*records))) return false;
    if (!records->pItemsTypeTxt || !records->dwItemsTypeRecs ||
        records->dwItemsTypeRecs > 32768)
        return false;
    return Readable(records->pItemsTypeTxt,
        size_t(records->dwItemsTypeRecs) * sizeof(D2ItemTypesTxt));
}
bool RewardBaseTableValid(const D2ItemDataTbl* bases) {
    if (!bases || bases->nWeaponsTxtRecordCount < 1 ||
        bases->nArmorTxtRecordCount < 1 ||
        bases->nItemsTxtRecordCount > 32768)
        return false;
    const int equipmentCount =
        bases->nWeaponsTxtRecordCount + bases->nArmorTxtRecordCount;
    return bases->nItemsTxtRecordCount >= equipmentCount &&
        Readable(bases->pItemsTxt,
            size_t(bases->nItemsTxtRecordCount) * sizeof(ItemsTxt)) &&
        bases->pWeapons == bases->pItemsTxt &&
        bases->pArmor == bases->pItemsTxt + bases->nWeaponsTxtRecordCount &&
        bases->pMisc == bases->pItemsTxt + equipmentCount;
}
bool RewardTables(D2ItemDataTbl*& bases, sgptDataTable*& records) {
    bases = D2COMMON_10535_DATATBLS_GetItemDataTables();
    records = Tables();
    return RewardBaseTableValid(bases) && RewardItemTypesValid(records);
}
unsigned MaximumNativeSockets(const D2ItemDataTbl* bases,
    const sgptDataTable* itemTypes, int baseId, int itemLevel) {
    if (!bases || !bases->pItemsTxt || !itemTypes ||
        !itemTypes->pItemsTypeTxt || baseId < 0 ||
        baseId >= bases->nItemsTxtRecordCount ||
        !itemTypes->dwItemsTypeRecs || itemLevel < 1 || itemLevel > 99)
        return 0;
    const ItemsTxt& base = bases->pItemsTxt[baseId];
    if (base.nType >= itemTypes->dwItemsTypeRecs) return 0;
    const D2ItemTypesTxt& type = itemTypes->pItemsTypeTxt[base.nType];
    const unsigned typeMaximum = RoguelikeLoot::SocketTypeMaximum(
        itemLevel, type.nMaxSock1, type.nMaxSock25, type.nMaxSock40);
    return RoguelikeLoot::MaximumSockets(base.bgemsockets, typeMaximum,
        RoguelikeLoot::kNormalSocketDifficultyCap);
}
bool RewardQualityAllowed(const D2ItemDataTbl* bases,
    const sgptDataTable* itemTypes, int baseId, int quality) {
    if (!bases || !bases->pItemsTxt || !itemTypes ||
        !itemTypes->pItemsTypeTxt || baseId < 0 ||
        baseId >= bases->nItemsTxtRecordCount) return false;
    const ItemsTxt& base = bases->pItemsTxt[baseId];
    if (base.nType >= itemTypes->dwItemsTypeRecs) return false;
    const D2ItemTypesTxt& type = itemTypes->pItemsTypeTxt[base.nType];
    return RoguelikeLoot::TypeAllowsQuality(
        quality, type.nNormal != 0, type.nMagic != 0, type.nRare != 0);
}
bool ConsiderRewardBaseId(int baseId, std::minstd_rand& random,
    unsigned& eligible, int& selectedBase) {
    if (random() % ++eligible != 0) return false;
    selectedBase = baseId;
    return true;
}
void ConsiderRewardBase(Pending& reward, std::minstd_rand& random,
    unsigned& eligible, int baseId, int specialId) {
    if (!ConsiderRewardBaseId(
            baseId, random, eligible, reward.baseId))
        return;
    reward.uniqueId = reward.quality == 7 ? specialId : -1;
    reward.setId = reward.quality == 5 ? specialId : -1;
}
int MatchingRewardBase(const D2ItemDataTbl* bases,
    const sgptDataTable* records, const char* code, int itemLevel,
    int playerLevel, RoguelikeLoot::BaseKind kind, int charmSize) {
    for (int base = 0; base < bases->nItemsTxtRecordCount; ++base)
        if (RewardBase(bases, records, base, itemLevel, playerLevel, false, 0,
                kind, charmSize) &&
            !memcmp(code, bases->pItemsTxt[base].szCode, 3))
            return base;
    return -1;
}
bool UniqueRewardRowEligible(const UniqueItemsTxt& row, int itemLevel,
    int playerLevel, int uniqueLevelBonus) {
    return (row.dwUniqueItemFlags & 1) &&
        row.wLvl <= RoguelikeLoot::WssUniqueSearchLevel(
            itemLevel, uniqueLevelBonus) &&
        row.wLvlReq <= playerLevel;
}
bool SelectUniqueRewardBases(Pending& reward, const D2ItemDataTbl* bases,
    const sgptDataTable* records, int itemLevel, int playerLevel,
    RoguelikeLoot::BaseKind kind, int charmSize,
    const int* excludedUniqueIds, unsigned excludedUniqueCount,
    int uniqueLevelBonus, std::minstd_rand& random, unsigned& eligible) {
    if (!records->dwUniqItemsRecs || records->dwUniqItemsRecs > 32768 ||
        !Readable(records->pUniqueItemsTxt,
            size_t(records->dwUniqItemsRecs) * sizeof(UniqueItemsTxt)))
        return false;
    for (unsigned i = 0; i < records->dwUniqItemsRecs; ++i) {
        const UniqueItemsTxt& row = records->pUniqueItemsTxt[i];
        if (!UniqueRewardRowEligible(row, itemLevel, playerLevel,
                uniqueLevelBonus) ||
            RoguelikeLoot::WssUniqueIdExcluded(
                static_cast<int>(i), excludedUniqueIds, excludedUniqueCount))
            continue;
        const int base = MatchingRewardBase(bases, records, row.szCode,
            itemLevel, playerLevel, kind, charmSize);
        if (base >= 0)
            ConsiderRewardBase(reward, random, eligible, base,
                static_cast<int>(i));
    }
    return true;
}
bool SetRewardRowEligible(const SetItemsTxt& row,
    const sgptDataTable* records, int itemLevel, int playerLevel) {
    return row.nSetId < records->dwSetsRecs &&
        row.wLvl <= itemLevel && row.wLvlReq <= playerLevel;
}
bool SelectSetRewardBases(Pending& reward, const D2ItemDataTbl* bases,
    const sgptDataTable* records, int itemLevel, int playerLevel,
    RoguelikeLoot::BaseKind kind, int charmSize,
    std::minstd_rand& random, unsigned& eligible) {
    if (!records->dwSetItemsRecs || records->dwSetItemsRecs > 32768 ||
        !records->dwSetsRecs || records->dwSetsRecs > 32768 ||
        !Readable(records->pSetItemsTxt,
            size_t(records->dwSetItemsRecs) * sizeof(SetItemsTxt)))
        return false;
    for (unsigned i = 0; i < records->dwSetItemsRecs; ++i) {
        const SetItemsTxt& row = records->pSetItemsTxt[i];
        if (!SetRewardRowEligible(row, records, itemLevel, playerLevel))
            continue;
        const int base = MatchingRewardBase(bases, records, row.szCode,
            itemLevel, playerLevel, kind, charmSize);
        if (base >= 0)
            ConsiderRewardBase(reward, random, eligible, base,
                static_cast<int>(i));
    }
    return true;
}
void SelectOrdinaryRewardBases(Pending& reward, const D2ItemDataTbl* bases,
    const sgptDataTable* records, int itemLevel, int playerLevel,
    bool starter, int sockets, RoguelikeLoot::BaseKind kind,
    int charmSize, std::minstd_rand& random, unsigned& eligible) {
    const int preferredFloor = RoguelikeLoot::RewardBaseLevelFloor(
        itemLevel, 99, kind == RoguelikeLoot::BaseKind::Equipment, starter);
    int highestEligibleLevel = -1;
    int highestEligibleBase = -1;
    int preferredBase = -1;
    unsigned highestEligible = 0;
    unsigned preferredEligible = 0;
    for (int base = 0; base < bases->nItemsTxtRecordCount; ++base) {
        if (!RewardBase(bases, records, base, itemLevel, playerLevel,
                starter, sockets, kind, charmSize)) continue;
        if (sockets && MaximumNativeSockets(
                bases, records, base, itemLevel) < static_cast<unsigned>(sockets))
            continue;
        if (!RewardQualityAllowed(bases, records, base, reward.quality))
            continue;
        const int baseLevel = bases->pItemsTxt[base].blevel;
        if (baseLevel > highestEligibleLevel) {
            highestEligibleLevel = baseLevel;
            highestEligible = 0;
        }
        if (baseLevel == highestEligibleLevel)
            ConsiderRewardBaseId(
                base, random, highestEligible, highestEligibleBase);
        if (baseLevel >= preferredFloor)
            ConsiderRewardBaseId(
                base, random, preferredEligible, preferredBase);
    }
    const int floor = RoguelikeLoot::RewardBaseLevelFloor(
        itemLevel, highestEligibleLevel,
        kind == RoguelikeLoot::BaseKind::Equipment, starter);
    const bool usePreferredBand = floor == preferredFloor;
    eligible = usePreferredBand ? preferredEligible : highestEligible;
    if (eligible) {
        reward.baseId = usePreferredBand ? preferredBase : highestEligibleBase;
        reward.uniqueId = -1;
        reward.setId = -1;
    }
}
bool SelectReward(Pending& reward, int itemLevel, int playerLevel, bool starter,
    DWORD entropy, int sockets = 0,
    RoguelikeLoot::BaseKind kind = RoguelikeLoot::BaseKind::Equipment,
    int charmSize = 0, int* maximumSockets = nullptr,
    int uniqueLevelBonus = 0, const int* excludedUniqueIds = nullptr,
    unsigned excludedUniqueCount = 0) {
    D2ItemDataTbl* bases = nullptr;
    sgptDataTable* records = nullptr;
    if (!RewardTables(bases, records)) return false;
    std::minstd_rand random(entropy ? entropy : 1);
    unsigned eligible = 0;
    bool recordsValid = true;
    if (reward.quality == 7) {
        recordsValid = SelectUniqueRewardBases(reward, bases, records,
            itemLevel, playerLevel, kind, charmSize, excludedUniqueIds,
            excludedUniqueCount, uniqueLevelBonus, random, eligible);
        if (recordsValid && !eligible && excludedUniqueCount) {
            recordsValid = SelectUniqueRewardBases(reward, bases, records,
                itemLevel, playerLevel, kind, charmSize, nullptr, 0,
                uniqueLevelBonus, random, eligible);
        }
    } else if (reward.quality == 5) {
        recordsValid = SelectSetRewardBases(reward, bases, records,
            itemLevel, playerLevel, kind, charmSize, random, eligible);
    } else {
        const int requestedQuality = reward.quality;
        for (unsigned attempt = 0;; ++attempt) {
            const int quality = RoguelikeLoot::RewardQualityAttempt(
                requestedQuality, attempt, sockets != 0);
            if (quality < 0) break;
            reward.quality = quality;
            reward.baseId = -1;
            eligible = 0;
            SelectOrdinaryRewardBases(reward, bases, records, itemLevel,
                playerLevel, starter, sockets, kind, charmSize, random, eligible);
            if (eligible) break;
        }
    }
    if (!recordsValid || !eligible || reward.baseId < 0) return false;
    const unsigned maximum = sockets ?
        MaximumNativeSockets(bases, records, reward.baseId, itemLevel) : 0;
    if (maximumSockets) *maximumSockets = static_cast<int>(maximum);
    return !sockets || maximum != 0;
}
enum class RunMaterial : BYTE { Gem, Rune };
int SelectMaterialBase(D2ItemDataTbl* bases, int itemLevel, int playerLevel,
    RunMaterial kind, DWORD entropy);
bool RemoveRolledItem(void* game, UnitAny* player, UnitAny* item);
unsigned naturalEquipmentCalls = 0;
unsigned naturalEquipmentForced = 0, naturalEquipmentRejected = 0;
int naturalEquipmentQuality = 0, naturalEquipmentBase = 0;
int RunDropArea(Drop* drop) {
    if (!Readable(drop, sizeof(Drop)) || !drop->unit ||
        !Readable(drop->unit, sizeof(UnitAny)) ||
        drop->unit->dwType != UNIT_MONSTER ||
        !Readable(drop->unit->pPath, sizeof(Path)) ||
        !Readable(drop->unit->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(drop->unit->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(drop->unit->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)))
        return -1;
    return drop->unit->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
}
bool RoguelikeMonsterDropContext(void* game, Drop* drop, int& area) {
    if (!game || game != runGame.load(std::memory_order_acquire) ||
        !Readable(drop, sizeof(Drop)) || drop->game != game ||
        drop->spawnType != ITEM_MODE_ON_GROUND)
        return false;
    area = RunDropArea(drop);
    if (area < 13 || area >= 255 ||
        runStage.load(std::memory_order_acquire) != area)
        return false;
    if (area == 20) return true;
    return zoneSlots[area].configured &&
        (zoneSlots[area].kind == Pending::ZoneKind::Monster ||
         zoneSlots[area].kind == Pending::ZoneKind::Boss);
}
bool ApplyRunDropIdentificationPolicy(UnitAny* item) {
    if (!Readable(item, sizeof(UnitAny)) || item->dwType != UNIT_ITEM ||
        !Readable(item->pItemData, sizeof(ItemData)))
        return false;
    const bool identified = RoguelikeLoot::ShouldIdentifyDrop(
        item->pItemData->dwQuality);
    D2COMMON_SetItemFlag(item, RoguelikeLoot::kIdentifiedItemFlag,
        identified ? TRUE : FALSE);
    return RoguelikeLoot::IsIdentified(item->pItemData->dwFlags) == identified;
}

UnitAny* RejectNaturalRunDrop(void* game, UnitAny* item,
    const std::string& reason) {
    const bool removed = RemoveRolledItem(game, runPlayer, item);
    runError = reason + (removed ? " removed" : " cleanup uncertain");
    return removed ? nullptr : item;
}
UnitAny* PassNaturalRunDrop(void* game, Drop* drop, int flags) {
    UnitAny* item = originalNaturalDrop(game, drop, flags);
    if (item && !ApplyRunDropIdentificationPolicy(item))
        return RejectNaturalRunDrop(
            game, item, "natural Roguelike drop identification policy failed");
    return item;
}
bool RoguelikeOwnedChestDropContext(void* game, Drop* drop) {
    if (!game || game != runGame.load(std::memory_order_acquire) ||
        !Readable(drop, sizeof(Drop)) || drop->game != game ||
        drop->spawnType != ITEM_MODE_ON_GROUND ||
        !Readable(drop->unit, sizeof(UnitAny)) ||
        drop->unit->dwType != UNIT_OBJECT || drop->unit->dwTxtFileNo != 397)
        return false;
    const int area = runStage.load(std::memory_order_acquire);
    if (area < 13 || area >= 255 || !zoneSlots[area].configured ||
        zoneSlots[area].kind != Pending::ZoneKind::Chest ||
        !PortalArea(runPlayer, area))
        return false;
    for (const DWORD chestId : runChestIds[area])
        if (chestId && chestId == drop->unit->dwUnitId) return true;
    return false;
}
bool ItemTypeAncestry(const sgptDataTable* records, WORD root,
    std::array<WORD, 64>& types, unsigned& count) {
    if (!records || !records->pItemsTypeTxt ||
        !records->dwItemsTypeRecs || records->dwItemsTypeRecs > 32768 ||
        root >= records->dwItemsTypeRecs)
        return false;
    types.fill(0);
    types[0] = root;
    count = 1;
    for (unsigned at = 0; at < count; ++at) {
        if (types[at] >= records->dwItemsTypeRecs ||
            !Readable(&records->pItemsTypeTxt[types[at]],
                sizeof(D2ItemTypesTxt)))
            return false;
        const D2ItemTypesTxt& row = records->pItemsTypeTxt[types[at]];
        const WORD parents[2] = {row.nEquiv1, row.nEquiv2};
        for (const WORD parent : parents) {
            if (!parent) continue;
            bool visited = false;
            for (unsigned index = 0; index < count; ++index)
                visited = visited || types[index] == parent;
            if (visited) continue;
            if (count == types.size()) return false;
            types[count++] = parent;
        }
    }
    return true;
}
bool RewardBaseIsPotion(const D2ItemDataTbl* bases,
    const sgptDataTable* records, int baseId) {
    if (!bases || baseId < 0 ||
        baseId >= bases->nItemsTxtRecordCount)
        return false;
    const ItemsTxt& base = bases->pItemsTxt[baseId];
    std::array<WORD, 64> types{};
    unsigned count = 0;
    if (!ItemTypeAncestry(records, base.nType, types, count)) return false;
    for (unsigned index = 0; index < count; ++index)
        if (!memcmp(records->pItemsTypeTxt[types[index]].szCode,
                "poti", 4))
            return true;
    return false;
}
UnitAny* __stdcall NaturalDropHook(void* game, Drop* drop, int flags) {
    if (RoguelikeOwnedChestDropContext(game, drop)) {
        bool potion = false;
        if (drop->quality != 7) {
            D2ItemDataTbl* bases = nullptr;
            sgptDataTable* records = nullptr;
            potion = RewardTables(bases, records) &&
                RewardBaseIsPotion(bases, records, drop->baseId);
        }
        if (!RoguelikeLoot::ChestNaturalDropAllowed(
                drop->quality, potion))
            return nullptr;
        return PassNaturalRunDrop(game, drop, flags);
    }
    int area = -1;
    if (!RoguelikeMonsterDropContext(game, drop, area))
        return originalNaturalDrop(game, drop, flags);
    const bool monsterZone = zoneSlots[area].configured &&
        zoneSlots[area].kind == Pending::ZoneKind::Monster;
    const bool combatZone = monsterZone ||
        (zoneSlots[area].configured && BossZone(zoneSlots[area].kind));
    if (combatZone &&
        zoneSlots[area].reward != Pending::RewardFocus::Equipment)
        return nullptr;
    D2ItemDataTbl* bases = nullptr;
    sgptDataTable* records = nullptr;
    if (!monsterZone || !RewardTables(bases, records) ||
        drop->baseId < 0 ||
        drop->baseId >= bases->nWeaponsTxtRecordCount +
            bases->nArmorTxtRecordCount)
        return PassNaturalRunDrop(game, drop, flags);
    ++naturalEquipmentCalls;
    naturalEquipmentQuality = drop->quality;
    naturalEquipmentBase = drop->baseId;
    const DWORD entropy = GetTickCount() ^ drop->unit->dwUnitId ^
        (drop->baseId * 2654435761u) ^
        (++naturalEquipmentForced * 2246822519u);
    const int quality =
        zoneSlots[area].affixes.quality == RoguelikeAffixes::Quality::Magic ? 4 :
        zoneSlots[area].affixes.quality == RoguelikeAffixes::Quality::Rare ? 6 : 2;
    drop->quality = quality;
    UnitAny* item = originalNaturalDrop(game, drop, flags);
    if (!item) {
        ++naturalEquipmentRejected;
        return nullptr;
    }
    const bool validItem = Readable(item, sizeof(UnitAny));
    const bool validData = validItem &&
        Readable(item->pItemData, sizeof(ItemData));
    if (!validData ||
        item->dwTxtFileNo != static_cast<DWORD>(drop->baseId) ||
        item->pItemData->dwQuality != static_cast<DWORD>(quality)) {
        ++naturalEquipmentRejected;
        const std::string mismatch = "natural equipment mismatch base " +
            std::to_string(validItem ? item->dwTxtFileNo : 0) + "/" +
            std::to_string(drop->baseId) + " quality " +
            std::to_string(validData ? item->pItemData->dwQuality : 0) +
            "/" + std::to_string(quality);
        return RejectNaturalRunDrop(game, item, mismatch);
    }
    if (!ApplyRunDropIdentificationPolicy(item)) {
        ++naturalEquipmentRejected;
        return RejectNaturalRunDrop(
            game, item, "natural Roguelike equipment identification policy failed");
    }
    return item;
}
#include "RoguelikeWssCorruption.h"
json DropRunReward(void* game, UnitAny* player, int itemLevel, int playerLevel,
    int quality, bool starter, DWORD entropy, int sockets = 0,
    UnitAny* dropper = nullptr,
    RoguelikeLoot::BaseKind baseKind = RoguelikeLoot::BaseKind::Equipment,
    int charmSize = 0, bool corruptedUnique = false,
    int uniqueLevelBonus = 0, const int* excludedUniqueIds = nullptr,
    unsigned excludedUniqueCount = 0) {
    if (sockets < 0 || sockets > 3)
        return Failure("requested socket count is outside the supported range");
    if (corruptedUnique && quality != 7)
        return Failure("boss corruption requires a unique item");
    Pending reward;
    int maximumSockets = 0;
    reward.id = "run-reward";
    reward.playerId = player->dwUnitId;
    reward.level = itemLevel;
    reward.quality = quality;
    reward.baseId = reward.uniqueId = reward.setId = -1;
    reward.seed = entropy ? entropy : 1;
    reward.expectedEpoch = runEpoch;
    if (!SelectReward(reward, itemLevel, playerLevel, starter, entropy, sockets,
            baseKind, charmSize, &maximumSockets, uniqueLevelBonus,
            excludedUniqueIds, excludedUniqueCount))
        return Failure("no compiled level-eligible reward base and quality");
    WssCorruptionSelection corruption{};
    std::string corruptionError;
    const DWORD socketSeed = sockets ?
        RoguelikeLoot::SeedForMaximumSockets(reward.seed,
            static_cast<std::uint32_t>(maximumSockets)) : 0;
    json result = Generate(game, player, reward,
        dropper, sockets != 0, socketSeed);
    if (!result.value("ok", false)) {
        if (result.contains("item_id") &&
            result["item_id"].is_number_unsigned()) {
            UnitAny* rejected =
                FindRolledItem(game, result["item_id"].get<DWORD>());
            if (rejected && !RemoveRolledItem(game, player, rejected))
                result["cleanup"] = "failed generated item cleanup uncertain";
            else if (rejected)
                result["cleanup"] = "failed generated item removed";
        }
        return result;
    }
    if (!result.contains("item_id") ||
        !result["item_id"].is_number_unsigned()) {
        result["ok"] = false;
        result["error"] = "generated Roguelike reward lacks item identity";
        return result;
    }
    UnitAny* item = FindRolledItem(game, result["item_id"].get<DWORD>());
    const auto reject = [&](const char* reason) {
        const bool present = item != nullptr;
        const bool removed = !present || RemoveRolledItem(game, player, item);
        result["ok"] = false;
        result["error"] = std::string(reason) +
            (!present ? "; item absent" :
                removed ? "; item removed" : "; cleanup uncertain");
    };
    const bool itemDataReadable =
        item && Readable(item->pItemData, sizeof(ItemData));
    if (!itemDataReadable ||
        item->dwTxtFileNo != static_cast<DWORD>(reward.baseId) ||
        item->pItemData->dwQuality != static_cast<DWORD>(reward.quality)) {
        reject("generated Roguelike reward identity or quality changed");
        return result;
    }
    if (corruptedUnique &&
        !ApplyWssCorruption(game, item, entropy,
            corruption, corruptionError)) {
        reject(corruptionError.c_str());
        return result;
    }
    if (!ApplyRunDropIdentificationPolicy(item)) {
        reject("generated Roguelike reward identification policy failed");
        return result;
    }
    result["identified"] =
        RoguelikeLoot::IsIdentified(item->pItemData->dwFlags);
    if (quality == 7) {
        std::string error;
        if (!ValidateRewardUnique(item, reward, itemLevel, playerLevel,
                baseKind, charmSize, uniqueLevelBonus, corruptedUnique,
                corruptedUnique ? &corruption : nullptr,
                result, error)) {
            reject(error.c_str());
            return result;
        }
    }
    if (!sockets) return result;
    result["max_sockets"] = maximumSockets;
    const int actualSockets = D2COMMON_GetUnitStat(item, STAT_SOCKETS, 0);
    result["sockets"] = actualSockets;
    const DWORD itemFlags = item->pItemData->dwFlags;
    const bool socketFlag = (itemFlags & ITEM_HASSOCKETS) != 0;
    if (actualSockets != static_cast<int>(maximumSockets) || !socketFlag) {
        reject("native maximum socket count or flag did not materialize");
    }
    return result;
}
json DropBossCorruptedUnique(void* game, UnitAny* player,
    int itemLevel, int playerLevel, DWORD seed, UnitAny* dropper,
    const int* excludedUniqueIds, unsigned excludedUniqueCount) {
    return DropRunReward(game, player, itemLevel, playerLevel, 7, false,
        seed, 0, dropper, RoguelikeLoot::BaseKind::Equipment, 0, true, 4,
        excludedUniqueIds, excludedUniqueCount);
}
void ReportCorruptionTest(void* game, UnitAny* player) {
    if (!game || !player || player->dwType != UNIT_PLAYER) return;
    const int playerLevel = MarketCharacterLevel();
    json result = playerLevel >= 1 && playerLevel <= 99 ?
        DropBossCorruptedUnique(game, player, playerLevel, playerLevel,
            GetTickCount() ^ player->dwUnitId ^
                (playerLevel * 2654435761u), nullptr, nullptr, 0) :
        Failure("corruption test has no valid character level");
    std::string text;
    if (!result.value("ok", false)) {
        text = "Corrupt-drop test failed: " +
            result.value("error", std::string("reward generation failed"));
    } else {
        const json& corruption = result["corruption"];
        text = "Corrupt drop created: item " +
            std::to_string(result.value("item_id", 0u)) +
            ", unique " + std::to_string(result.value("unique_id", -1)) +
            ", ilvl " + std::to_string(result.value("item_level", 0)) +
            ", recipe row " + std::to_string(
                corruption.value("recipe_row", 0u)) +
            ", roll " + std::to_string(corruption.value("roll", 0)) +
            ", marker " + std::to_string(
                corruption.value("marker", 0));
    }
    if (text.size() > 120) text.resize(120);
    BYTE message[128] = {0x15, 1, 0};
    memcpy(message + 3, text.data(), text.size());
    originalChat(game, player, message, static_cast<int>(text.size() + 4));
}
int GemTier(const char* code) {
    if (code[0] != 'g') return -1;
    switch (code[1]) {
    case 'c': return 0;
    case 'f': return 1;
    case 's': return 2;
    case 'l': return 3;
    case 'p': return 4;
    default: return -1;
    }
}
int GemColor(const char* code) {
    const char* colors = "vybgrw";
    for (int i = 0; i < 6; ++i)
        if (code[2] == colors[i]) return i;
    return -1;
}
int RuneTier(const char* code) {
    if (code[0] != 'r' || code[1] < '0' || code[1] > '3' ||
        code[2] < '0' || code[2] > '9') return -1;
    const int rank = (code[1] - '0') * 10 + code[2] - '0';
    return rank >= 1 && rank <= 33 ? rank - 1 : -1;
}
template <size_t N>
int ClosestAvailableTier(const std::array<int, N>& bases, unsigned target) {
    for (unsigned distance = 0; distance < N; ++distance) {
        if (target >= distance && bases[target - distance] >= 0)
            return static_cast<int>(target - distance);
        const unsigned upper = target + distance;
        if (distance && upper < N && bases[upper] >= 0)
            return static_cast<int>(upper);
    }
    return -1;
}
int SelectMaterialBase(D2ItemDataTbl* bases, int itemLevel, int playerLevel,
    RunMaterial kind, DWORD entropy) {
    if (!bases || !bases->pItemsTxt || !bases->pArmor || !bases->pMisc ||
        itemLevel < 1 || itemLevel > 99 || playerLevel < 1 || playerLevel > 99 ||
        bases->nMiscTxtRecordCount < 1 || bases->nMiscTxtRecordCount > 32768)
        return -1;
    const int miscOffset =
        bases->nWeaponsTxtRecordCount + bases->nArmorTxtRecordCount;
    if (bases->pMisc != bases->pItemsTxt + miscOffset ||
        bases->nItemsTxtRecordCount < miscOffset + bases->nMiscTxtRecordCount)
        return -1;
    std::array<int, 33> runeBases;
    runeBases.fill(-1);
    std::array<std::array<int, 6>, 5> gemBases{};
    for (auto& tier : gemBases) tier.fill(-1);
    for (int index = 0; index < bases->nMiscTxtRecordCount; ++index) {
        const ItemsTxt& row = bases->pMisc[index];
        // Gem codes respect base level; rune tier is already depth-scaled.
        if (!row.bspawnable || row.bquest ||
            !RoguelikeLoot::MaterialBaseLevelEligible(
                row.blevel, itemLevel, kind == RunMaterial::Rune) ||
            (row.szCode[3] != 0 && row.szCode[3] != ' '))
            continue;
        if (kind == RunMaterial::Rune) {
            const int tier = RuneTier(row.szCode);
            if (tier >= 0 && runeBases[tier] < 0) runeBases[tier] = index;
        } else {
            const int tier = GemTier(row.szCode);
            const int color = GemColor(row.szCode);
            if (tier >= 0 && color >= 0 && gemBases[tier][color] < 0)
                gemBases[tier][color] = index;
        }
    }
    std::array<int, 5> availableGemTiers;
    availableGemTiers.fill(-1);
    for (unsigned tier = 0; tier < gemBases.size(); ++tier)
        for (unsigned color = 0; color < gemBases[tier].size(); ++color)
            if (gemBases[tier][color] >= 0) {
                availableGemTiers[tier] = static_cast<int>(tier);
                break;
            }
    std::minstd_rand random(entropy ? entropy : 1);
    if (kind == RunMaterial::Rune) {
        const unsigned target = RoguelikeLoot::ScaledTier(itemLevel,
            static_cast<unsigned>(runeBases.size()),
            static_cast<unsigned>(random()) % RoguelikeLoot::kBasisPoints);
        const int tier = ClosestAvailableTier(runeBases, target);
        return tier < 0 ? -1 : miscOffset + runeBases[tier];
    }
    const unsigned target = RoguelikeLoot::ScaledTier(itemLevel,
        static_cast<unsigned>(gemBases.size()),
        static_cast<unsigned>(random()) % RoguelikeLoot::kBasisPoints);
    const int tier = ClosestAvailableTier(availableGemTiers, target);
    if (tier < 0) return -1;
    const unsigned firstColor = static_cast<unsigned>(random()) % 6u;
    for (unsigned offset = 0; offset < 6; ++offset) {
        const unsigned color = (firstColor + offset) % 6u;
        if (gemBases[tier][color] >= 0)
            return miscOffset + gemBases[tier][color];
    }
    return -1;
}
json DropRunMaterial(void* game, UnitAny* player, int itemLevel, int playerLevel,
    RunMaterial kind, DWORD entropy, UnitAny* dropper = nullptr) {
    D2ItemDataTbl* bases = nullptr;
    sgptDataTable* records = nullptr;
    if (!RewardTables(bases, records))
        return Failure("compiled misc reward rows unavailable");
    const int base = SelectMaterialBase(
        bases, itemLevel, playerLevel, kind, entropy);
    if (base < 0) return Failure("no compiled level-eligible gem or rune");
    Pending reward;
    reward.id = "run-material";
    reward.playerId = player->dwUnitId;
    reward.level = itemLevel;
    reward.quality = 2;
    reward.seed = entropy ? entropy : 1;
    reward.expectedEpoch = runEpoch;
    reward.baseId = base;
    json result = Generate(game, player, reward, dropper);
    if (!result.value("ok", false)) return result;
    if (!result.contains("item_id") ||
        !result["item_id"].is_number_unsigned()) {
        result["ok"] = false;
        result["error"] = "generated Roguelike material lacks item identity";
        return result;
    }
    UnitAny* item = FindRolledItem(game, result["item_id"].get<DWORD>());
    const auto reject = [&](const char* reason) {
        const bool present = item != nullptr;
        const bool removed = !present || RemoveRolledItem(game, player, item);
        result["ok"] = false;
        result["error"] = std::string(reason) +
            (!present ? "; item absent" :
                removed ? "; item removed" : "; cleanup uncertain");
    };
    if (!item || !Readable(item->pItemData, sizeof(ItemData)) ||
        item->dwTxtFileNo != static_cast<DWORD>(base) ||
        item->pItemData->dwQuality != 2) {
        reject("generated Roguelike material identity or quality changed");
        return result;
    }
    if (!ApplyRunDropIdentificationPolicy(item)) {
        reject("generated Roguelike material identification policy failed");
        return result;
    }
    result["identified"] =
        RoguelikeLoot::IsIdentified(item->pItemData->dwFlags);
    result["material"] = kind == RunMaterial::Gem ? "gem" : "rune";
    return result;
}
int MonsterDropQuality(int itemLevel, DWORD entropy) {
    std::minstd_rand random(entropy ? entropy : 1);
    const unsigned roll = static_cast<unsigned>(random()) %
        MerchantQuality::kBasisPoints;
    return MerchantQuality::ToItemQuality(
        MerchantQuality::Select(itemLevel, roll), 2);
}
json DropRunCategory(void* game, UnitAny* player, UnitAny* monster,
    int itemLevel, int playerLevel, RoguelikeLoot::BaseKind kind,
    DWORD entropy, int charmSize = 0) {
    const DWORD qualitySeed = entropy ^ 0xa511e9b3u;
    const DWORD baseSeed = entropy ^ 0x63d83595u;
    int quality = MonsterDropQuality(itemLevel, qualitySeed);
    const bool charm = kind == RoguelikeLoot::BaseKind::Charm;
    if (charm && (quality == 5 || quality == 6)) quality = 4;
    if (charm && !charmSize)
        charmSize = static_cast<int>(
            RoguelikeLoot::CharmSizeForRewardIndex(entropy));
    const auto spawn = [&](int itemQuality, int selectedCharmSize) {
        return DropRunReward(game, player, itemLevel, playerLevel,
            itemQuality, false, baseSeed, 0, monster, kind, selectedCharmSize);
    };
    const auto noEligibleRewardBase = [](const json& drop) {
        return !drop.value("ok", false) &&
            drop.value("error", "") ==
                "no compiled level-eligible reward base and quality";
    };
    json result = spawn(quality, charmSize);
    if (noEligibleRewardBase(result) && (quality == 5 || quality == 7))
        result = spawn(charm ? 4 : 6, charm ? charmSize : 0);
    if (kind == RoguelikeLoot::BaseKind::Jewel &&
        result.value("ok", false))
        result["material"] = "jewel";
    return result;
}
json DropRunJewel(void* game, UnitAny* player, UnitAny* monster,
    int itemLevel, int playerLevel, DWORD entropy) {
    return DropRunCategory(game, player, monster, itemLevel, playerLevel,
        RoguelikeLoot::BaseKind::Jewel, entropy);
}
json DropRunSocketable(void* game, UnitAny* player, UnitAny* monster,
    int itemLevel, int playerLevel, unsigned materialSlot, DWORD entropy) {
    switch (materialSlot % 3u) {
    case 0:
        return DropRunMaterial(game, player, itemLevel, playerLevel,
            RunMaterial::Gem, entropy, monster);
    case 1:
        return DropRunMaterial(game, player, itemLevel, playerLevel,
            RunMaterial::Rune, entropy, monster);
    default:
        return DropRunJewel(game, player, monster, itemLevel, playerLevel, entropy);
    }
}
bool RewardHasCode(const json& reward, const char* expected) {
    if (!reward.contains("code") || !reward["code"].is_string()) return false;
    const std::string& code = reward["code"].get_ref<const std::string&>();
    return code.size() == 3 && !memcmp(code.data(), expected, 3);
}
bool RewardHasMaterial(const json& reward, const char* expected) {
    if (!reward.contains("material") || !reward["material"].is_string())
        return false;
    return reward["material"].get_ref<const std::string&>() == expected;
}
json DropMonsterItem(void* game, UnitAny* player, UnitAny* monster,
    int itemLevel, int playerLevel, RoguelikeLoot::DropKind kind,
    DWORD entropy, unsigned materialSlot) {
    switch (kind) {
    case RoguelikeLoot::DropKind::Equipment: {
        const int quality = MonsterDropQuality(playerLevel, entropy ^ 0xa511e9b3u);
        json reward = DropRunReward(game, player, itemLevel, playerLevel,
            quality, false, entropy, 0, monster);
        if (!reward.value("ok", false) && (quality == 5 || quality == 7) &&
            reward.value("error", "") ==
                "no compiled level-eligible reward base and quality")
            reward = DropRunReward(game, player, itemLevel, playerLevel,
                6, false, entropy, 0, monster);
        return reward;
    }
    case RoguelikeLoot::DropKind::SocketedEquipment:
        return DropRunReward(game, player, itemLevel, playerLevel,
            2, false, entropy, 1, monster);
    case RoguelikeLoot::DropKind::Gem:
        return DropRunMaterial(game, player, itemLevel, playerLevel,
            RunMaterial::Gem, entropy, monster);
    case RoguelikeLoot::DropKind::Rune:
        return DropRunMaterial(game, player, itemLevel, playerLevel,
            RunMaterial::Rune, entropy, monster);
    case RoguelikeLoot::DropKind::Socketable:
        return DropRunSocketable(game, player, monster, itemLevel, playerLevel,
            materialSlot, entropy);
    case RoguelikeLoot::DropKind::Jewelry:
        return DropRunCategory(game, player, monster, itemLevel, playerLevel,
            RoguelikeLoot::BaseKind::Jewelry, entropy);
    case RoguelikeLoot::DropKind::Charms:
        return DropRunCategory(game, player, monster, itemLevel, playerLevel,
            RoguelikeLoot::BaseKind::Charm, entropy,
            static_cast<int>(RoguelikeLoot::CharmSizeForRewardIndex(materialSlot)));
    case RoguelikeLoot::DropKind::Unique:
        return DropRunReward(game, player, itemLevel, playerLevel,
            7, false, entropy, 0, monster);
    case RoguelikeLoot::DropKind::Set:
        return DropRunReward(game, player, itemLevel, playerLevel,
            5, false, entropy, 0, monster);
    case RoguelikeLoot::DropKind::None:
        break;
    }
    return Failure("invalid monster-drop plan");
}
bool ValidMonsterReward(const json& reward) {
    const int quality = reward.value("quality", 0);
    const bool expectedIdentified =
        RoguelikeLoot::ShouldIdentifyDrop(quality);
    return reward.value("ok", false) && reward.contains("item_id") &&
        reward["item_id"].is_number_unsigned() && reward.contains("quality") &&
        reward.value("identified", !expectedIdentified) == expectedIdentified;
}
bool StandardRewardQuality(int quality) {
    return quality == 2 || quality == 4 || quality == 5 ||
        quality == 6 || quality == 7;
}
bool CharmRewardQuality(int quality) {
    return quality == 2 || quality == 4 || quality == 7;
}
bool RewardHasSocketableMaterial(const json& reward) {
    return RewardHasMaterial(reward, "gem") ||
        RewardHasMaterial(reward, "rune") ||
        RewardHasMaterial(reward, "jewel");
}
bool RewardIsSocketedEquipment(const json& reward, int quality) {
    const unsigned sockets = reward.value("sockets", 0u);
    return quality == 2 && sockets > 0 &&
        sockets == reward.value("max_sockets", 0u);
}
bool RewardIsJewelry(const json& reward, int quality) {
    const bool jewelryCode = RewardHasCode(reward, "rin") ||
        RewardHasCode(reward, "amu");
    return jewelryCode && StandardRewardQuality(quality);
}
bool RewardIsCharm(const json& reward, int quality, int charmSize = 0) {
    const bool charmCode = charmSize == 1 ? RewardHasCode(reward, "cm1") :
        charmSize == 2 ? RewardHasCode(reward, "cm2") :
        charmSize == 3 ? RewardHasCode(reward, "cm3") :
        RewardHasCode(reward, "cm1") || RewardHasCode(reward, "cm2") ||
            RewardHasCode(reward, "cm3");
    return charmCode && CharmRewardQuality(quality);
}
bool MonsterDropMatches(RoguelikeLoot::DropKind kind, const json& reward,
    int charmSize = 0) {
    if (!ValidMonsterReward(reward)) return false;
    const int quality = reward.value("quality", 0);
    switch (kind) {
    case RoguelikeLoot::DropKind::Equipment:
        return StandardRewardQuality(quality);
    case RoguelikeLoot::DropKind::SocketedEquipment:
        return RewardIsSocketedEquipment(reward, quality);
    case RoguelikeLoot::DropKind::Gem:
        return RewardHasMaterial(reward, "gem");
    case RoguelikeLoot::DropKind::Rune:
        return RewardHasMaterial(reward, "rune");
    case RoguelikeLoot::DropKind::Socketable:
        return RewardHasSocketableMaterial(reward);
    case RoguelikeLoot::DropKind::Jewelry:
        return RewardIsJewelry(reward, quality);
    case RoguelikeLoot::DropKind::Charms:
        return RewardIsCharm(reward, quality, charmSize);
    case RoguelikeLoot::DropKind::Unique:
        return quality == 7;
    case RoguelikeLoot::DropKind::Set:
        return quality == 5;
    case RoguelikeLoot::DropKind::None:
        return false;
    }
    return false;
}
bool DropMonsterGroup(void* game, UnitAny* player, UnitAny* monster,
    int itemLevel, int playerLevel, RoguelikeLoot::DropGroup group,
    unsigned firstIndex, std::minstd_rand& random) {
    if (group.count > 4 ||
        (group.count == 0) != (group.kind == RoguelikeLoot::DropKind::None)) {
        runDropError = "invalid monster-drop group";
        return false;
    }
    for (unsigned index = 0; index < group.count; ++index) {
        const unsigned materialSlot = firstIndex + index;
        const DWORD entropy = static_cast<DWORD>(random()) ^
            monster->dwUnitId ^ (materialSlot * 2246822519u);
        const int charmSize = group.kind == RoguelikeLoot::DropKind::Charms ?
            static_cast<int>(
                RoguelikeLoot::CharmSizeForRewardIndex(materialSlot)) : 0;
        const json reward = DropMonsterItem(
            game, player, monster, itemLevel, playerLevel, group.kind,
            entropy, materialSlot);
        if (!MonsterDropMatches(group.kind, reward, charmSize)) {
            runDropError = "monster drop violated its focus or identification contract: " +
                reward.value("error", reward.dump());
            return false;
        }
    }
    return true;
}
bool DropMonsterLoot(void* game, UnitAny* player, UnitAny* monster,
    int itemLevel, int runDepth, Pending::RewardFocus focus) {
    const int playerLevel = MarketCharacterLevel();
    if (playerLevel < 1 || playerLevel > 99) {
        runDropError = "monster loot has no valid character level";
        return false;
    }
    const DWORD seed = GetTickCount() ^ (monster->dwUnitId * 2654435761u);
    std::minstd_rand random(seed ? seed : 1);
    const auto primaryCount = static_cast<std::uint8_t>(
        RoguelikeLoot::MonsterDropCountAtDepth(runDepth,
            static_cast<std::uint32_t>(random()) %
                RoguelikeLoot::kBasisPoints));
    const RoguelikeLoot::MonsterDropPlan plan =
        RoguelikeLoot::PlanMonsterDrops(focus, primaryCount);
    if (!plan.primary.count || plan.primary.count > 4 ||
        plan.primary.kind == RoguelikeLoot::DropKind::None ||
        (plan.secondary.count != 0 && plan.secondary.count != 3)) {
        runDropError = "zone has no valid monster-drop plan";
        return false;
    }
    if (!DropMonsterGroup(game, player, monster, itemLevel, playerLevel,
            plan.primary, 0, random) ||
        !DropMonsterGroup(game, player, monster, itemLevel, playerLevel,
            plan.secondary, plan.primary.count, random))
        return false;
    if (focus == Pending::RewardFocus::Equipment &&
        RoguelikeLoot::DropsRune(playerLevel,
            static_cast<unsigned>(random()) % RoguelikeLoot::kBasisPoints)) {
        const DWORD entropy = static_cast<DWORD>(random()) ^
            monster->dwUnitId ^ 0x9e3779b9u;
        const json rune = DropRunMaterial(game, player, itemLevel, playerLevel,
            RunMaterial::Rune, entropy, monster);
        if (!rune.value("ok", false)) {
            if (rune.value("error", "") ==
                "no compiled level-eligible gem or rune")
                return true;
            runDropError = "monster rune drop uncertain: " +
                rune.value("error", rune.dump());
            return false;
        }
        if (!MonsterDropMatches(RoguelikeLoot::DropKind::Rune, rune)) {
            runDropError = "monster rune drop violated its identification policy";
            return false;
        }
    }
    return true;
}
// S13 D2Game RVA A24C0: EAX=actor, stack=(game, room, x, y, destination,
// source-object-out, object-class, permanent), callee RET 20h. Null actor
// bypasses the town-only Cow/Uber gate at A24FC; never call with a player.
using PortalConstructor = int (__stdcall*)(void*, Room1*, int, int, int, UnitAny**, int, int);
const BYTE* VerifiedPortalEntry(std::string& error) {
    if (!LabProfile::VerifyServer(error)) return nullptr;
    HMODULE server = GetModuleHandleA("D2Game.dll");
    if (!server) { error = "offline server is not loaded"; return nullptr; }
    const BYTE* entry = reinterpret_cast<const BYTE*>(server) + 0xA24C0;
    const BYTE signature[] = {0x83,0xec,0x0c,0x55,0x8b,0x6c,0x24,0x18,
        0x85,0xed,0x56,0x8b,0xf0,0x75,0x1f};
    if (!Readable(entry, sizeof(signature)) || memcmp(entry, signature, sizeof(signature))) {
        error = "unrecognized S13 portal constructor";
        return nullptr;
    }
    std::string digest;
    if (!LabProfile::ModFileIdentity(digest) ||
        digest != "538a77b7ccef3d5334e56c4e9e57a4d8fc69a1e27c46beb694c0dedfcfbf9cb3") {
        error = "unrecognized S13 PD2 module";
        return nullptr;
    }
    return entry;
}
// S13 D2Game RVA E03A0: ECX=game, EDX=unit ID, EAX=unit type.
UnitAny* FindServerObject(void* game, DWORD id, DWORD expectedClass = 60) {
    HMODULE module = GetModuleHandleA("D2Game.dll");
    if (!module || !game) return nullptr;
    const BYTE* entry = reinterpret_cast<const BYTE*>(module) + 0xE03A0;
    const BYTE signature[] = {0x85, 0xc9, 0x75, 0x1f, 0x68};
    if (!Readable(entry, sizeof(signature)) || memcmp(entry, signature, sizeof(signature)))
        return nullptr;
    UnitAny* found = nullptr;
    __asm {
        mov ecx, game
        mov edx, id
        mov eax, 2
        call entry
        mov found, eax
    }
    return found && Readable(found, sizeof(UnitAny)) && found->dwType == UNIT_OBJECT &&
        found->dwUnitId == id && found->dwTxtFileNo == expectedClass ? found : nullptr;
}
// The same pinned D2Game lookup is used for spawned monsters. A GUID alone
// is not an owner: compare the live unit pointer with the spawn-time identity.
UnitAny* FindServerMonster(void* game, DWORD id) {
    HMODULE module = GetModuleHandleA("D2Game.dll");
    if (!module || !game || !id) return nullptr;
    const BYTE* entry = reinterpret_cast<const BYTE*>(module) + 0xE03A0;
    const BYTE signature[] = {0x85, 0xc9, 0x75, 0x1f, 0x68};
    if (!Readable(entry, sizeof(signature)) || memcmp(entry, signature, sizeof(signature)))
        return nullptr;
    UnitAny* found = nullptr;
    __asm {
        mov ecx, game
        mov edx, id
        mov eax, 1
        call entry
        mov found, eax
    }
    return found && Readable(found, sizeof(UnitAny)) &&
        found->dwType == UNIT_MONSTER && found->dwUnitId == id ? found : nullptr;
}
// D2Common S13 GetLevel caches one Level per numeric ID in ActMisc+0x47C.
// An allocated Level stub is still fresh until its Room2 list is generated.
struct TargetStatus {
    bool sourceFound = false;
    bool targetFound = false;
    bool generated = false;
    bool listVerified = false;
    const Level* target = nullptr;
};
bool CaveBaseline(const LevelsTxt& row, int level, DWORD monsterCount);
bool TargetUnused(UnitAny* player, int target, TargetStatus* status = nullptr) {
    sgptDataTable* table = Tables();
    if (!(target == 20 || (target >= 13 && target <= 16) ||
          (target >= 138 && target <= 254)) ||
        !table || !table->pLevelsTxt || !table->pLvlDefs || table->dwLevelsRecs <= target ||
        !Readable(table->pLevelsTxt + target, sizeof(LevelsTxt)) ||
        !Readable(table->pLvlDefs + size_t(target) * 0x9C, 0x9C)) return false;
    const LevelsTxt& row = table->pLevelsTxt[target];
    if (target == 20 ? row.wLevelNo != 20 :
        (!table->pMonStatsTxt || !table->dwMonStatsRecs ||
         (!CaveBaseline(row, target, table->dwMonStatsRecs) &&
          !(zoneGame && zoneRows == table->pLevelsTxt && zoneSlots[target].configured &&
            !memcmp(&row, &zoneSlots[target].configuredRow, sizeof(row))))))
        return false;
    if (!player || !Readable(player, sizeof(UnitAny)) || !player->pPath ||
        !Readable(player->pPath, sizeof(Path)) || !player->pPath->pRoom1 ||
        !Readable(player->pPath->pRoom1, sizeof(Room1))) return false;
    Room2* room = player->pPath->pRoom1->pRoom2;
    if (!Readable(room, sizeof(Room2)) || !Readable(room->pLevel, sizeof(Level)))
        return false;
    Level* source = room->pLevel;
    ActMisc* misc = source->pMisc;
    if (!Readable(misc, sizeof(ActMisc))) return false;
    Level* level = misc->pLevelFirst;
    bool foundSource = false;
    bool foundTarget = false;
    bool generated = false;
    unsigned count = 0;
    for (; level && count < 512; level = level->pNextLevel, ++count) {
        if (!Readable(level, sizeof(Level)) || level->pMisc != misc) return false;
        if (level == source) foundSource = true;
        if (level->dwLevelNo == target) {
            if (foundTarget) return false;
            foundTarget = true;
            if (status) status->target = level;
            generated = level->pRoom2First != nullptr;
        }
    }
    if (status) {
        status->sourceFound = foundSource;
        status->targetFound = foundTarget;
        status->generated = generated;
        status->listVerified = !level;
    }
    return !level && foundSource && !generated;
}
json TargetFailure(UnitAny* player, int target, const std::string& id) {
    TargetStatus status;
    TargetUnused(player, target, &status);
    json candidates = json::array();
    for (int cave = 13; cave <= 254; ++cave) {
        if (cave == 17) cave = 138;
        TargetStatus current;
        const bool unused = TargetUnused(player, cave, &current);
        candidates.push_back({{"level", cave}, {"unused", unused},
            {"present", current.targetFound}, {"generated", current.generated},
            {"list_verified", current.listVerified}});
    }
    const int source = player && Readable(player, sizeof(UnitAny)) && player->pPath &&
        Readable(player->pPath, sizeof(Path)) && player->pPath->pRoom1 &&
        Readable(player->pPath->pRoom1, sizeof(Room1)) && player->pPath->pRoom1->pRoom2 &&
        Readable(player->pPath->pRoom1->pRoom2, sizeof(Room2)) &&
        Readable(player->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ?
        player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo : 0;
    sgptDataTable* table = Tables();
    json compiled = {{"levels_count", table ? json(table->dwLevelsRecs) : json(nullptr)},
        {"row", nullptr}, {"level_def_readable", false}};
    if (table && target >= 0 && table->dwLevelsRecs > static_cast<DWORD>(target)) {
        if (table->pLevelsTxt && Readable(table->pLevelsTxt + target, sizeof(LevelsTxt))) {
            const LevelsTxt& row = table->pLevelsTxt[target];
            compiled["row"] = {{"id", row.wLevelNo}, {"act", row.nAct},
                {"normal_level", row.wMonLvl[0]}, {"expansion_level", row.wMonLvlEx[0]},
                {"density", row.dwMonDen[0]}, {"num_mon", row.dwNumMon}};
        }
        compiled["level_def_readable"] = table->pLvlDefs &&
            Readable(table->pLvlDefs + size_t(target) * 0x9C, 0x9C);
    }
    return {{"ok", false}, {"code", "destination_not_proven_unused"},
        {"request_id", id}, {"source_level", source}, {"target_level", target},
        {"source_found", status.sourceFound}, {"target_found", status.targetFound},
        {"target_generated", status.generated}, {"list_verified", status.listVerified},
        {"compiled_target", std::move(compiled)}, {"candidates", std::move(candidates)}};
}
// Observe Kashya's generated hire candidates on the owning server thread. The
// pinned S13 hire service uses these same NPC-record and Hireling.txt offsets;
// never manufacture a name from localized text or a guessed numeric range.
json InspectMarketHireNames(void* game, int source) {
    if (source >= 255 || zoneGame != game || !zoneSlots[source].configured ||
        zoneSlots[source].kind != Pending::ZoneKind::Merchant ||
        runGame.load(std::memory_order_acquire) != game)
        return nullptr;
    const BYTE* module = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2Game.dll"));
    // The absolute IAT operand is relocated with D2Game's image base.
    if (!module || !Readable(module + 0xAC40, 6) ||
        module[0xAC40] != 0xFF || module[0xAC41] != 0x25 ||
        *reinterpret_cast<const DWORD*>(module + 0xAC42) !=
            reinterpret_cast<DWORD>(module + 0xF8814))
        return {{"ok", false}, {"reason", "pinned hireling thunk unavailable"}};
    if (!Readable(game, 0x1D28))
        return {{"ok", false}, {"reason", "game NPC context unavailable"}};
    using HirelingRow = const BYTE* (__stdcall*)(int, int, int, int);
    const int expansion = *reinterpret_cast<const int*>(
        reinterpret_cast<const BYTE*>(game) + 0x70);
    if (expansion != 1) return {{"ok", false}, {"reason", "not an expansion game"}};
    const BYTE* row = reinterpret_cast<HirelingRow>(
        const_cast<BYTE*>(module) + 0xAC40)(expansion, 150, 0, 0);
    if (!Readable(row, 0x118))
        return {{"ok", false}, {"reason", "compiled Kashya hireling row unavailable"}};
    const WORD first = *reinterpret_cast<const WORD*>(row + 0x114);
    const WORD last = *reinterpret_cast<const WORD*>(row + 0x116);
    if (!first || last < first || last - first >= 41)
        return {{"ok", false}, {"reason", "unexpected compiled Kashya name range"}};
    const BYTE* gameBytes = reinterpret_cast<const BYTE*>(game);
    if (!Readable(gameBytes + 0x1D24, sizeof(void*)))
        return {{"ok", false}, {"reason", "NPC control slot unavailable"}};
    const BYTE* control = *reinterpret_cast<const BYTE* const*>(gameBytes + 0x1D24);
    if (!Readable(control, 8))
        return {{"ok", false}, {"reason", "NPC control unavailable"}};
    const BYTE* records = *reinterpret_cast<const BYTE* const*>(control + 4);
    if (!Readable(records, 64 * 0x44))
        return {{"ok", false}, {"reason", "NPC records unavailable"}};
    const BYTE* record = nullptr;
    for (unsigned i = 0; i < 64; ++i) {
        const BYTE* candidate = records + i * 0x44;
        if (*reinterpret_cast<const DWORD*>(candidate) == 150) {
            record = candidate; break;
        }
    }
    if (!record)
        return {{"ok", false}, {"reason", "Kashya NPC record absent"}};
    const BYTE* mercs = *reinterpret_cast<const BYTE* const*>(record + 0x10);
    const unsigned count = last - first + 1;
    if (!Readable(mercs, count * 16))
        return {{"ok", false}, {"reason", "generated mercenary list unavailable"}};
    json available = json::array();
    for (unsigned i = 0; i < count; ++i) {
        const BYTE* candidate = mercs + i * 16;
        const WORD name = *reinterpret_cast<const WORD*>(candidate);
        const DWORD hired = *reinterpret_cast<const DWORD*>(candidate + 8);
        const DWORD enabled = *reinterpret_cast<const DWORD*>(candidate + 12);
        if (name != first + i || hired > 1 || enabled > 1)
            return {{"ok", false}, {"reason", "generated mercenary record mismatch"}};
        if (enabled && !hired) available.push_back(name);
    }
    return {{"ok", true}, {"names", std::move(available)},
        {"name_first", first}, {"name_last", last}};
}

std::string ArenaRegionMismatchDetail(int cave, const BYTE* region,
    DWORD expectedDensity, bool contextCurrent, const ZoneSlot& slot) {
    if (!Readable(region, 0x2C4))
        return "server arena MonsterRegion unreadable (area " +
            std::to_string(cave) + ")";
    const DWORD observedLevel =
        *reinterpret_cast<const DWORD*>(region + 0x2C0);
    const DWORD observedDensity =
        *reinterpret_cast<const DWORD*>(region + 0x2B8);
    return "server arena MonsterRegion level or density mismatch (area " +
        std::to_string(cave) + ", level " + std::to_string(observedLevel) +
        "/" + std::to_string(cave) + ", density " +
        std::to_string(observedDensity) + "/" +
        std::to_string(expectedDensity) + ", configured " +
        std::to_string(slot.configured) + ", context current " +
        std::to_string(contextCurrent) + ", stored density " +
        std::to_string(slot.regionDensity) + ", row density " +
        std::to_string(slot.configuredRow.dwMonDen[0]) + ")";
}

json InspectLevels(void* game, UnitAny* player, const Pending& request) {
    if (!ObservationTelemetry::MatchesContext(request.expectedEpoch, game, player))
        return {{"ok", false}, {"code", "stale_epoch"}, {"request_id", request.id}};
    if (!player || !Readable(player, sizeof(UnitAny)) || player->dwUnitId != request.playerId ||
        !Readable(player->pPath, sizeof(Path)) ||
        !Readable(player->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(player->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(player->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)))
        return Failure("server area unavailable");
    std::string imageError;
    if (!VerifiedPortalEntry(imageError)) return Failure(imageError);
    const int source = player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
    if (source != 1 && source != 20 &&
        !((source >= 13 && source <= 16) || (source >= 138 && source <= 254)))
        return Failure("not in an arena, tower room, or starting town");
    // Pinned S13 MonsterRegion.cpp RVA CAB30 allocates one region per level ID:
    // game+0xF0[level] points to a 0x2E4-byte region, whose density at 0x2B8
    // is copied from Levels.txt when constructed, before its rooms generate.
    const auto* indexed = reinterpret_cast<void* const*>(
        reinterpret_cast<const BYTE*>(game) + 0xF0);
    if (!Readable(indexed + source, sizeof(void*)))
        return Failure("server monster region index unavailable");
    const auto* region = reinterpret_cast<const BYTE*>(indexed[source]);
    json population = {{"allocated", region != nullptr}};
    if (region) {
        if (!Readable(region, 0x2C4))
            return Failure("server monster region unreadable");
        const DWORD regionLevel = *reinterpret_cast<const DWORD*>(region + 0x2C0);
        if (regionLevel != static_cast<DWORD>(source))
            return Failure("server monster region level differs from player area");
        population["density"] = *reinterpret_cast<const DWORD*>(region + 0x2B8);
        population["selected_counts"] = {region[0x10], region[0x11], region[0x12]};
        population["selection_state"] = *reinterpret_cast<const DWORD*>(region + 0x0C);
    }
    sgptDataTable* table = Tables();
    if (!table || !table->pLevelsTxt || table->dwLevelsRecs <= source ||
        !Readable(table->pLevelsTxt + source, sizeof(LevelsTxt)))
        return Failure("loaded level row unavailable for region comparison");
    const LevelsTxt& authored = table->pLevelsTxt[source];
    if (authored.wLevelNo != source)
        return Failure("loaded level row differs from player area");
    population["row_density"] = authored.dwMonDen[0];
    population["row_monster_counts"] = authored.dwNumMon;
    population["row_normal_monster"] = authored.wMon[0];
    json levels = json::array();
    for (int cave = 13; cave <= 254; ++cave) {
        if (cave == 17) cave = 138;
        TargetStatus status;
        const bool fresh = TargetUnused(player, cave, &status) && !zoneSlots[cave].configured;
        if (!status.listVerified || !status.sourceFound)
            return Failure("server level linked list or compiled arena row unverified (area " + std::to_string(cave) + ")");
        if (!Readable(indexed + cave, sizeof(void*)))
            return Failure("server arena monster region index unavailable (area " + std::to_string(cave) + ")");
        const auto* candidateRegion = reinterpret_cast<const BYTE*>(indexed[cave]);
        const bool contextCurrent = zoneEpoch == request.expectedEpoch &&
            zoneGame == game;
        const bool rolled = contextCurrent && zoneSlots[cave].configured;
        const DWORD expectedDensity = rolled ? zoneSlots[cave].regionDensity : 600;
        if (!candidateRegion && (status.targetFound || status.generated))
            return Failure("server arena MonsterRegion absent for allocated area " +
                std::to_string(cave));
        if (candidateRegion &&
            (!Readable(candidateRegion, 0x2C4) ||
             *reinterpret_cast<const DWORD*>(candidateRegion + 0x2C0) !=
                static_cast<DWORD>(cave) ||
             *reinterpret_cast<const DWORD*>(candidateRegion + 0x2B8) != expectedDensity))
            return Failure(ArenaRegionMismatchDetail(cave, candidateRegion,
                expectedDensity, contextCurrent, zoneSlots[cave]));
        json entry = {{"level", cave}, {"allocated", status.targetFound},
            {"room2_generated", status.generated}, {"fresh", fresh},
            {"monster_region_allocated", candidateRegion != nullptr}};
        entry["consumed"] = rolled;
        entry["magic_find"] = rolled ? json(zoneSlots[cave].magicFind) : json(nullptr);
        entry["run_level"] = rolled && zoneSlots[cave].depth ?
            json(zoneSlots[cave].depth) : json(nullptr);
        json monsterClasses = json::array();
        if (rolled)
            for (unsigned index = 0;
                 index < zoneSlots[cave].selectedMonsterClassCount; ++index)
                monsterClasses.push_back(
                    zoneSlots[cave].selectedMonsterClasses[index]);
        entry["monster_classes"] = std::move(monsterClasses);
        entry["zone_affixes"] = rolled && zoneSlots[cave].depth ?
            ZoneAffixReceipt(zoneSlots[cave].affixes) : json(nullptr);
        if (status.targetFound) {
            // S13 D2Common DRLG RVA 2E2C0 reads these two idle-unload gates.
            // They are observations, not proof that adjacent rooms can unload.
            const auto* bytes = reinterpret_cast<const BYTE*>(status.target);
            if (!Readable(bytes, 0x1D8))
                return Failure("server Level idle-unload fields unreadable");
            entry["drlg_room_refs"] = *reinterpret_cast<const DWORD*>(bytes + 0x0C);
            entry["drlg_unload_ticks"] = *reinterpret_cast<const DWORD*>(bytes + 0x1D4);
            unsigned roomCount = 0, directBlockers = 0;
            const Room2* firstBlocker = nullptr;
            for (const Room2* room = status.target->pRoom2First; room; room = room->pRoom2Next) {
                if (++roomCount > 512 || !Readable(room, sizeof(Room2)) ||
                    room->pLevel != status.target)
                    return Failure("server Room2 list unverified");
                if (reinterpret_cast<const BYTE*>(room)[0x44] < 4 ||
                    (room->dwRoomFlags & 0x100000)) {
                    ++directBlockers;
                    if (!firstBlocker) firstBlocker = room;
                }
            }
            entry["drlg_room2_count"] = roomCount;
            entry["drlg_direct_blockers"] = directBlockers;
            entry["drlg_first_blocker"] = firstBlocker
                ? json{{"x", firstBlocker->dwPosX}, {"y", firstBlocker->dwPosY},
                    {"flags", firstBlocker->dwRoomFlags},
                    {"state", reinterpret_cast<const BYTE*>(firstBlocker)[0x44]},
                    {"room1_active", firstBlocker->pRoom1 != nullptr}}
                : json(nullptr);
            json blockerPortals = json::array();
            if (firstBlocker && firstBlocker->pRoom1) {
                const Room1* active = firstBlocker->pRoom1;
                if (!Readable(active, sizeof(Room1)) || active->pRoom2 != firstBlocker)
                    return Failure("server blocking Room1 unverified");
                unsigned units = 0;
                for (UnitAny* unit = active->pUnitFirst; unit; unit = unit->pRoomNext) {
                    if (++units > 2048 || !Readable(unit, sizeof(UnitAny)))
                        return Failure("server blocking room unit chain unverified");
                    if (unit->dwType == UNIT_OBJECT && unit->dwTxtFileNo == 60) {
                        blockerPortals.push_back({{"object_id", unit->dwUnitId},
                            {"owner_type", unit->dwOwnerType}, {"owner_id", unit->dwOwnerId}});
                    }
                }
            }
            entry["drlg_first_blocker_portal"] =
                blockerPortals.size() == 1 ? blockerPortals[0] : json(nullptr);
            entry["drlg_first_blocker_portals"] = std::move(blockerPortals);
        } else {
            entry["drlg_room_refs"] = nullptr;
            entry["drlg_unload_ticks"] = nullptr;
            entry["drlg_room2_count"] = nullptr;
            entry["drlg_direct_blockers"] = nullptr;
            entry["drlg_first_blocker"] = nullptr;
            entry["drlg_first_blocker_portal"] = nullptr;
            entry["drlg_first_blocker_portals"] = nullptr;
        }
        levels.push_back(std::move(entry));
    }
    // Return authoritative identities, not hover strings: same-category
    // rerolls may retain their exact label while changing destination.
    json offeredChoices = json::array();
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [id, choice] : ownedJobs) {
            if (!choice->completed || !choice->triggerJob ||
                choice->triggerSource != source ||
                choice->expectedEpoch != request.expectedEpoch ||
                choice->runSerial != runActiveSerial ||
                choice->playerId != player->dwUnitId) continue;
            const auto receipt = portalReceipts.find(id);
            if (receipt == portalReceipts.end() ||
                !receipt->second.second.value("ok", false) ||
                !receipt->second.second.contains("object_id")) continue;
            const json& result = receipt->second.second;
            offeredChoices.push_back({{"object_id", result["object_id"]},
                {"destination_level", result["destination_level"]},
                {"zone_kind", result.value("zone_kind", -1)},
                {"reward_focus", result.value("reward_focus", -1)},
                {"reroll", result.value("reroll", json(nullptr))}});
        }
    }
    const UnitAny* target = player->pPath->pTargetUnit;
    json hireNames = InspectMarketHireNames(game, source);
    if (hireNames.is_object() && hireNames.value("ok", false)) {
        std::lock_guard<std::mutex> lock(mutex);
        observedHireNames.epoch = request.expectedEpoch;
        observedHireNames.playerId = player->dwUnitId;
        observedHireNames.area = source;
        observedHireNames.available.clear();
        for (const auto& name : hireNames["names"])
            observedHireNames.available.push_back(name.get<WORD>());
    }
    const bool auraContext = runAura.game == game &&
        runAura.playerId == player->dwUnitId && runAura.epoch == runEpoch &&
        runAura.config != nullptr;
    const SkillsTxt* auraSkill = auraContext ? RunAuraSkill(runAura.config) : nullptr;
    const DWORD auraFrame = *reinterpret_cast<const DWORD*>(
        reinterpret_cast<const BYTE*>(game) + 0xA8);
    StatList* auraState = auraSkill ?
        D2COMMON_GetStateStatList(player, auraSkill->wAuraState) : nullptr;
    const bool auraStatePresent = auraState != nullptr;
    const bool auraActive = auraContext &&
        runAuraPending.load(std::memory_order_acquire) &&
        static_cast<LONG>(auraFrame - runAura.endFrame) < 0;
    return {{"ok", true}, {"status", "observed"}, {"request_id", request.id},
        {"source", "offline_server"}, {"source_level", source},
        {"player_mode", player->dwMode},
        {"player_target_id", target && Readable(target, sizeof(UnitAny)) ?
            json(target->dwUnitId) : json(nullptr)},
        {"levels", std::move(levels)}, {"offered_choices", std::move(offeredChoices)},
        {"population_region", std::move(population)},
        {"market_hire_names", std::move(hireNames)},
        {"run_snapshot_status", ServerSpawns::RunSnapshotStatus()},
        {"native_run", {{"armed", runGame.load(std::memory_order_acquire) == game},
            {"run_serial", runActiveSerial},
            {"root_level", runRootLevel},
            {"stage", runStage.load(std::memory_order_acquire)},
            {"cleared", source < 255 && runCleared[source]},
            {"population_ready", source < 255 && runPopulationReady[source]},
            {"encounter", source < 255 ? runEncounters[source] : 0},
            {"starter_white_item_id", runStarterRewardId ?
                json(runStarterRewardId) : json(nullptr)},
            {"clear_reward_item_ids", source < 255 ?
                json(runClearRewardIds[source]) : json(nullptr)},
            {"reward_focus", source < 255 && zoneSlots[source].configured ?
                json(static_cast<int>(zoneSlots[source].reward)) : json(nullptr)},
            {"locked_chest_ids", source < 255 ?
                json(runChestIds[source]) : json(nullptr)},
            {"chest_reward_item_ids", source < 255 ?
                json(runChestRewardIds[source]) : json(nullptr)},
            {"chest_reward_counts", source < 255 ?
                json(runChestRewardCounts[source]) : json(nullptr)},
            {"armed_reroll_item_id", armedReroll.game == game &&
                armedReroll.playerId == player->dwUnitId &&
                armedReroll.epoch == runEpoch && armedReroll.itemId ?
                json(armedReroll.itemId) : json(nullptr)},
            {"run_item_use_packets", runItemUsePackets},
            {"run_item_use_status", runItemUseStatus},
            {"aura_active", auraActive},
            {"aura_state_present", auraStatePresent},
            {"aura_prayer_state_hitpoints_stat", auraState ?
                json(D2COMMON_GetStatValueFromStatList(auraState, 6, 0)) : json(nullptr)},
            {"aura_prayer_state_hpregen_stat", auraState ?
                json(D2COMMON_GetStatValueFromStatList(auraState, 74, 0)) : json(nullptr)},
            {"aura_prayer_effective_hpregen_stat", auraContext ?
                json(D2COMMON_GetUnitStat(player, 74, 0)) : json(nullptr)},
            {"aura_frame", auraContext ? json(auraFrame) : json(nullptr)},
            {"aura_skill_id", auraActive ? json(runAura.config->skillId) : json(nullptr)},
            {"aura_skill_level", auraActive ? json(runAura.config->skillLevel) : json(nullptr)},
            {"aura_expires_frame", auraContext ? json(runAura.endFrame) : json(nullptr)},
            {"aura_last_error", runAura.error},
            {"extra_boss_unit_id", source < 255 && runBossUnit[source] ?
                json(runBossUnit[source]) : json(nullptr)},
            {"natural_drop_failures", runDropFailureCount},
            {"last_natural_drop_error", runDropError},
            {"last_error", runError},
            {"akara_permanent_cache_count", observedAkaraPerms},
            {"akara_potion_cache_count", observedAkaraPotions},
            {"akara_trade_matches_record", observedAkaraTrade},
            {"akara_filtered_potions", filteredAkaraPotions},
            {"natural_equipment_constructor_calls", naturalEquipmentCalls},
            {"natural_equipment_last_quality", naturalEquipmentQuality},
            {"natural_equipment_last_base", naturalEquipmentBase},
            {"natural_equipment_forced", naturalEquipmentForced},
            {"natural_equipment_rejected", naturalEquipmentRejected},
            {"boss_damage_hits", runScaledHits},
            {"boss_missile_damage_hits", runScaledMissileHits},
            {"boss_owner_projectile_damage_hits", runBossProjectileHits},
            {"boss_owner_projectile_sample", runBossProjectileOriginal ?
                json{{"boss_id", runBossDamageSources[0].id},
                    {"native_level", runBossDamageSources[0].nativeLevel},
                    {"area_level", source < 255 ? zoneSlots[source].configuredRow.wMonLvl[0] : 0},
                    {"component_offset", runBossProjectileComponent},
                    {"before_raw", runBossProjectileOriginal},
                    {"after_raw", runBossProjectileApplied}} : json(nullptr)},
            {"boss_missile_owner_rejected_hits", runRejectedMissileOwners},
            {"boss_observed_missile_damage", runObservedMissileDamage},
            {"boss_other_attacker_damage", runOtherAttackerDamage},
            {"boss_missile_owner_sample", runMissileDamageSample.id ?
                json{{"id", runMissileDamageSample.id},
                    {"class_id", runMissileDamageSample.classId},
                    {"attacker_type", runMissileDamageSample.type},
                    {"owner_type", runMissileDamageSample.ownerType},
                    {"owner_id", runMissileDamageSample.ownerId}} : json(nullptr)},
            {"boss_damage_sample", runDamageNativeLevel ?
                json{{"native_level", runDamageNativeLevel},
                    {"area_level", source < 255 ? zoneSlots[source].configuredRow.wMonLvl[0] : 0},
                    {"component_offset", runDamageComponent},
                    {"before_raw", runDamageOriginal}, {"after_raw", runDamageApplied},
                    {"missile", runDamageMissile}} : json(nullptr)},
            {"native_map_monster", {{"applied", mapMonsterApplied},
                {"stock_existing", mapMonsterStockExisting},
                {"unit_id", mapMonsterSampleUnit}, {"area", mapMonsterSampleArea},
                {"before_raw", mapMonsterSampleBefore},
                {"after_raw", mapMonsterSampleAfter}}},
            {"native_map_player", {{"area", mapPlayerAppliedArea},
                {"before_magic_find", mapPlayerBeforeMagicFind},
                {"after_magic_find", mapPlayerAfterMagicFind}}},
            {"native_map_context", {{"monster_stats", mapMonsterCount},
                {"player_stats", mapPlayerCount}, {"global_stats", mapGlobalCount}}}}},
        {"natural_hook", ServerMonsters::NaturalCounters()}};
}
static_assert(sizeof(LevelsTxt) == 0x220, "S13 Levels.txt row stride");
static_assert(offsetof(LevelsTxt, wMonLvl) == 0x10 &&
    offsetof(LevelsTxt, wMonLvlEx) == 0x16 &&
    offsetof(LevelsTxt, dwMonDen) == 0x1C &&
    offsetof(LevelsTxt, dwNumMon) == 0x32 &&
    offsetof(LevelsTxt, wMon) == 0x36 &&
    offsetof(LevelsTxt, wNMon) == 0x68 &&
    offsetof(LevelsTxt, wUMon) == 0x9A, "S13 loaded level columns");
bool CaveBaseline(const LevelsTxt& row, int level, DWORD monsterCount) {
    constexpr WORD sourceLevels[] = {2, 4, 5, 7};
    const WORD expected = level <= 16 ? sourceLevels[level - 13] : 2;
    // S13 packs the normal and alternate monster counts into this legacy DWORD.
    if (row.wLevelNo != level || row.nAct != 0 || row.wMonLvl[0] != expected ||
        row.wMonLvlEx[0] != expected || (row.dwNumMon & 0xff) != 3 ||
        (level >= 138 && (row.wMonLvlEx[1] != 37 || row.wMonLvlEx[2] != 78)))
        return false;
    for (unsigned difficulty = 0; difficulty < 3; ++difficulty)
        if (row.dwMonDen[difficulty] != 600) return false;
    for (unsigned index = 0; index < 3; ++index)
        if (row.wMon[index] >= monsterCount) return false;
    return true;
}
// Pinned D2Common ordinal 10459 / RVA 2D2C0 is __stdcall DWORD(DWORD);
// the stock numeric act thresholds classify every 138..254 as Act V, even
// when authenticated Levels.txt says Act I. The portal constructor _exit's
// on a cross-act pair, so this scoped detour must precede any high-ID portal.
using ActLookup = DWORD (__stdcall*)(DWORD);
ActLookup originalActLookup = nullptr;
bool actLookupInstalled = false;
DWORD __stdcall ScopedArenaAct(DWORD level) {
    if (level >= 138 && level <= 254) {
        sgptDataTable* table = Tables();
        if (table && table->pLevelsTxt && table->dwLevelsRecs > level &&
            table->pMonStatsTxt && table->dwMonStatsRecs &&
            Readable(table->pLevelsTxt + level, sizeof(LevelsTxt))) {
            const LevelsTxt& row = table->pLevelsTxt[level];
            if (CaveBaseline(row, level, table->dwMonStatsRecs) ||
                (zoneRows == table->pLevelsTxt && zoneSlots[level].configured &&
                 !memcmp(&row, &zoneSlots[level].configuredRow, sizeof(row))))
                return 0;
        }
    }
    return originalActLookup(level);
}
bool InstallArenaActLookup(std::string& error) {
    if (actLookupInstalled) return true;
    if (!LabProfile::VerifyCommon(error)) return false;
    HMODULE common = GetModuleHandleA("D2Common.dll");
    const BYTE* entry = common ? reinterpret_cast<const BYTE*>(common) + 0x2D2C0 : nullptr;
    const BYTE signature[] = {0x8b,0x4c,0x24,0x04,0xb8,0x01,0,0,0,
        0x8d,0xa4,0x24,0,0,0,0};
    const bool readable = entry && Readable(entry, 17);
    const bool ordinalMatches = readable &&
        GetProcAddress(common, MAKEINTRESOURCEA(10459)) ==
            reinterpret_cast<FARPROC>(const_cast<BYTE*>(entry));
    bool knownHook = false;
    // The pinned offline runtime may replace this act prologue with an E8
    // thunk ending in RET 4. Chain only that observed executable call target.
    if (readable && !memcmp(entry, signature, 9) && entry[9] == 0xe8 &&
        entry[14] == 0xc2 && entry[15] == 0x04 && entry[16] == 0) {
        int32_t displacement;
        memcpy(&displacement, entry + 10, sizeof(displacement));
        const BYTE* destination = reinterpret_cast<const BYTE*>(
            reinterpret_cast<uintptr_t>(entry) + 14 +
            static_cast<uintptr_t>(displacement));
        MEMORY_BASIC_INFORMATION target{};
        const DWORD protection = VirtualQuery(destination, &target, sizeof(target))
            ? target.Protect & 0xff : 0;
        knownHook = target.State == MEM_COMMIT &&
            (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
             protection == PAGE_EXECUTE_READWRITE ||
             protection == PAGE_EXECUTE_WRITECOPY);
    }
    if (!ordinalMatches || (memcmp(entry, signature, sizeof(signature)) && !knownHook)) {
        error = readable && !ordinalMatches
            ? "pinned S13 act lookup ordinal resolves to a different entry"
            : "pinned S13 act lookup entry prologue is modified or unreadable";
        return false;
    }
    sgptDataTable* table = Tables();
    if (!table || !table->pLevelsTxt || !table->pLvlDefs ||
        !table->pMonStatsTxt || !table->dwMonStatsRecs ||
        table->dwLevelsRecs < 255 || table->dwLevelsRecs > 1024 ||
        !Readable(table->pLevelsTxt, size_t(table->dwLevelsRecs) * sizeof(LevelsTxt)) ||
        !Readable(table->pLvlDefs, 255 * 0x9C))
        { error = "expanded arena compiler rows unavailable"; return false; }
    for (int level = 138; level <= 254; ++level)
        if (!CaveBaseline(table->pLevelsTxt[level], level, table->dwMonStatsRecs)) {
            error = "expanded arena act profile differs at " + std::to_string(level); return false;
        }
    originalActLookup = reinterpret_cast<ActLookup>(const_cast<BYTE*>(entry));
    if (DetourTransactionBegin() != NO_ERROR) {
        error = "cannot begin scoped act lookup detour"; return false;
    }
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        DetourTransactionAbort(); error = "cannot update current thread for act detour"; return false;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        DetourTransactionAbort(); error = "cannot enumerate threads for act detour"; return false;
    }
    std::array<HANDLE, 1024> threads{};
    size_t used = 0;
    THREADENTRY32 thread{}; thread.dwSize = sizeof(thread);
    bool ok = Thread32First(snapshot, &thread) != 0;
    const DWORD process = GetCurrentProcessId(), current = GetCurrentThreadId();
    do {
        if (!ok) break;
        if (thread.th32OwnerProcessID != process || thread.th32ThreadID == current) continue;
        if (used == threads.size()) { ok = false; break; }
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
            THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread.th32ThreadID);
        if (!handle) { ok = false; break; }
        if (DetourUpdateThread(handle) != NO_ERROR) { CloseHandle(handle); ok = false; break; }
        threads[used++] = handle;
    } while (Thread32Next(snapshot, &thread));
    const DWORD endError = GetLastError();
    CloseHandle(snapshot);
    if (!ok || endError != ERROR_NO_MORE_FILES ||
        DetourAttach(reinterpret_cast<void**>(&originalActLookup),
            reinterpret_cast<void*>(ScopedArenaAct)) != NO_ERROR) {
        DetourTransactionAbort();
        for (size_t i = 0; i < used; ++i) CloseHandle(threads[i]);
        error = "cannot attach scoped act lookup detour"; return false;
    }
    const LONG committed = DetourTransactionCommit();
    for (size_t i = 0; i < used; ++i) CloseHandle(threads[i]);
    if (committed != NO_ERROR) {
        error = "cannot commit scoped act lookup detour"; return false;
    }
    actLookupInstalled = true;
    return true;
}
bool UpdateLevelRow(LevelsTxt& row, const LevelsTxt& expected) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(&row, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    DWORD previous = 0;
    if (!VirtualProtect(&row, sizeof(row), PAGE_READWRITE, &previous)) return false;
    row = expected;
    DWORD unused = 0;
    if (VirtualProtect(&row, sizeof(row), previous, &unused)) return true;
    // The write already occurred; never claim a successful configuration.
    return false;
}
bool ResetZoneRows(std::string& error) {
    if (zoneRows && !Readable(zoneRows, 255 * sizeof(LevelsTxt))) {
        error = "prior game level table became unreadable"; return false;
    }
    if (zoneRows) {
        for (int i = 13; i <= 254; ++i) {
            const ZoneSlot& slot = zoneSlots[i];
            if (!slot.configured) continue;
            const LevelsTxt& current = zoneRows[i];
            if (std::memcmp(&current, &slot.configuredRow, sizeof(LevelsTxt)) &&
                std::memcmp(&current, &slot.original, sizeof(LevelsTxt))) {
                error = "prior cave row changed unexpectedly"; return false;
            }
        }
        for (int i = 13; i <= 254; ++i) {
            const ZoneSlot& slot = zoneSlots[i];
            if (slot.configured &&
                std::memcmp(&zoneRows[i], &slot.original, sizeof(LevelsTxt)) &&
                !UpdateLevelRow(zoneRows[i], slot.original)) {
                error = "cannot restore prior game's cave row"; return false;
            }
        }
    }
    zoneSlots = {};
    zoneRows = nullptr;
    zoneGame = nullptr;
    zoneEpoch.clear();
    mapMonsterGame = nullptr;
    mapMonsterEpoch.clear();
    mapMonsterCount = mapPlayerCount = mapGlobalCount = 0;
    mapMonsterBytes = {};
    mapPlayerBytes = {};
    mapGlobalBytes = {};
    mapPlayerAppliedArea = 0;
    mapPlayerBeforeMagicFind = mapPlayerAfterMagicFind = 0;
    mapMonsterApplied = 0;
    mapMonsterStockExisting = 0;
    mapMonsterSampleUnit = 0;
    mapMonsterSampleArea = 0;
    mapMonsterSampleBefore = 0;
    mapMonsterSampleAfter = 0;
    return true;
}
// The game's compiled affix table is the catalog; no second copy of its 232
// S13 map rows or property-functions is maintained by the roguelike.
std::array<RoguelikeAffixes::Candidate, 233> mapCatalog{};
unsigned mapCatalogCount = 0;
bool MapCatalog(std::string& error) {
    if (mapCatalogCount) return true;
    D2MagicAffixDataTbl* source = D2COMMON_10492_DATATBLS_GetMagicAffixDataTables();
    if (!source || !source->pMagicAffixTxt || !source->pMagicSuffix ||
        !source->pMagicPrefix || !source->pAutoMagic ||
        source->nMagicAffixTxtRecordCount <= 0 ||
        source->nMagicAffixTxtRecordCount > 32768 ||
        !Readable(source->pMagicAffixTxt,
            size_t(source->nMagicAffixTxtRecordCount) * sizeof(AutoMagicTxt))) {
        error = "native map-affix catalog unavailable"; return false;
    }
    unsigned prefixes = 0, suffixes = 0, count = 0;
    for (int index = 0; index < source->nMagicAffixTxtRecordCount; ++index) {
        const AutoMagicTxt* affix = source->pMagicAffixTxt + index;
        if (affix >= source->pAutoMagic || !affix->wSpawnable ||
            affix->wVersion != 1 || !affix->bFrequency) continue;
        unsigned tiers = 0;
        for (unsigned type : affix->wIType)
            if (type >= 106 && type <= 109) tiers |= 1u << (type - 106);
        for (unsigned type : affix->wEType)
            if (type >= 106 && type <= 109) tiers &= ~(1u << (type - 106));
        if (!tiers) continue;
        if (!affix->dwGroup || !affix->szName[0] || count >= 232) {
            error = "unexpected stock map-affix row"; return false;
        }
        const bool prefix = affix >= source->pMagicPrefix;
        mapCatalog[count++] = {static_cast<unsigned>(index + 1),
            affix->dwGroup, tiers, affix->bFrequency,
            affix->nRare != 0, prefix};
        if (prefix) ++prefixes;
        else ++suffixes;
    }
    if (count != 232 || prefixes != 118 || suffixes != 114) {
        error = "pinned stock map-affix catalog diverged"; return false;
    }
    // Source S13 has an area-level property (374) but no enabled map-tier
    // affix that rolls it. Make that zone-only property explicit.
    mapCatalog[count++] = {kZoneAreaAffix, 0x10000, 0xF, 4, true, true};
    mapCatalogCount = count;
    return true;
}
// S13 D2GAME unit lookup/destructor are type-generic (item case 4 is
// explicitly handled at E20A0); the portal variant uses object case 2.
UnitAny* FindRolledItem(void* game, DWORD id) {
    const BYTE* server = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2GAME.dll"));
    const BYTE* lookup = server ? server + 0xE03A0 : nullptr;
    const BYTE signature[] = {0x85,0xc9,0x75,0x1f,0x68};
    if (!game || !lookup || !Readable(lookup, sizeof(signature)) ||
        memcmp(lookup, signature, sizeof(signature))) return nullptr;
    UnitAny* item = nullptr;
    __asm {
        mov ecx, game
        mov edx, id
        mov eax, 4
        call lookup
        mov item, eax
    }
    return item && Readable(item, sizeof(UnitAny)) &&
        item->dwType == UNIT_ITEM && item->dwUnitId == id ? item : nullptr;
}
bool RemoveRolledItem(void* game, UnitAny* player, UnitAny* item) {
    const BYTE* server = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2GAME.dll"));
    const BYTE* destructor = server ? server + 0xE20A0 : nullptr;
    const BYTE* sender = server ? server + 0x8A3E0 : nullptr;
    const BYTE destructorSignature[] = {0x55,0x56,0x57,0x8b,0xf9,0x33,0xed,0x3b,0xfd};
    const BYTE senderSignature[] = {0x53,0x55,0x8b,0x6c,0x24,0x10,0x56,0x8b,0xf0};
    if (!item || !player || !Readable(item, sizeof(UnitAny)) ||
        item->dwType != UNIT_ITEM || !player->pPlayerData ||
        !Readable(player->pPlayerData, 0xA0) ||
        !Readable(player->pPlayerData->pClient, 0x1C4) ||
        *reinterpret_cast<void**>(
            reinterpret_cast<BYTE*>(player->pPlayerData->pClient) + 0x1A8) != game ||
        !destructor || !Readable(destructor, sizeof(destructorSignature)) ||
        memcmp(destructor, destructorSignature, sizeof(destructorSignature)) ||
        !sender || !Readable(sender, sizeof(senderSignature)) ||
        memcmp(sender, senderSignature, sizeof(senderSignature))) return false;
    const DWORD id = item->dwUnitId;
    if (FindRolledItem(game, id) != item || item->dwMode != ITEM_MODE_ON_GROUND ||
        !player->pPath || !Readable(player->pPath, sizeof(Path)) ||
        player->pPath->pTargetUnit == item) return false;
    using RemoveUnit = void (__fastcall*)(void*, UnitAny*);
    reinterpret_cast<RemoveUnit>(const_cast<BYTE*>(destructor))(game, item);
    if (FindRolledItem(game, id)) return false;
    BYTE packet[6] = {0x0A, UNIT_ITEM, 0, 0, 0, 0};
    memcpy(packet + 2, &id, sizeof(id));
    void* client = player->pPlayerData->pClient;
    __asm {
        push 6
        lea ecx, packet
        push ecx
        mov eax, client
        call sender
    }
    return true;
}
bool ResolveMapRoll(void* game, UnitAny* player, unsigned depth,
    std::minstd_rand& random, RoguelikeAffixes::Roll& roll, std::string& error) {
    sgptDataTable* tables = Tables();
    D2MagicAffixDataTbl* affixes = D2COMMON_10492_DATATBLS_GetMagicAffixDataTables();
    if (!game || !player || !tables || !S13ItemStatCostLayoutValid(tables) ||
        !affixes || !affixes->pMagicAffixTxt || !player->pPath ||
        !player->pPath->pRoom1 || !player->pPath->pRoom1->Coll ||
        !createItem || !rollAffix) {
        error = "native map-item roller or compiled stat metadata unavailable"; return false;
    }
    Pending exact;
    unsigned prefixes = 0, suffixes = 0;
    for (unsigned i = 0; i < roll.count; ++i) {
        const unsigned id = roll.modifiers[i].id;
        if (id == kZoneAreaAffix) continue;
        const AutoMagicTxt* affix = ZoneAffix(id);
        if (!affix || !affix->wSpawnable || !affix->bFrequency) {
            error = "selected stock map affix changed"; return false;
        }
        if (affix >= affixes->pMagicPrefix)
            exact.prefixes[prefixes++] = static_cast<int>(id);
        else exact.suffixes[suffixes++] = static_cast<int>(id);
    }
    if (prefixes + suffixes) {
        Room1* room = player->pPath->pRoom1;
        CollMap* collision = room->Coll;
        const uint64_t cells = uint64_t(collision->dwSizeGameX) * collision->dwSizeGameY;
        if (!cells || cells > 65536 ||
            !Readable(collision->pMapStart, size_t(cells) * sizeof(WORD))) {
            error = "native map-roll room collision unavailable"; return false;
        }
        int x = player->pPath->xPos, y = player->pPath->yPos;
        bool placed = false;
        for (int radius = 0; radius <= 4 && !placed; ++radius)
            for (int dy = -radius; dy <= radius && !placed; ++dy)
                for (int dx = -radius; dx <= radius && !placed; ++dx) {
                    const int px = x + dx, py = y + dy;
                    if (px < static_cast<int>(collision->dwPosGameX) ||
                        py < static_cast<int>(collision->dwPosGameY)) continue;
                    const unsigned cx = px - collision->dwPosGameX;
                    const unsigned cy = py - collision->dwPosGameY;
                    if (cx >= collision->dwSizeGameX || cy >= collision->dwSizeGameY ||
                        (collision->pMapStart[cy * collision->dwSizeGameX + cx] & 0x3E01)) continue;
                    x = px; y = py; placed = true;
                }
        if (!placed) { error = "no ground tile for native map roll"; return false; }
        const ItemsTxt* base = D2COMMON_GetItemText(737);
        if (!base || base->szCode[0] != 't' || base->szCode[1] != '1' ||
            base->szCode[2] != '1') {
            error = "pinned synthetic map item base changed"; return false;
        }
        Drop drop = {};
        drop.unit = player; drop.game = game; drop.level = (std::min)(99u, 20u + depth);
        drop.baseId = 737; drop.spawnType = ITEM_MODE_ON_GROUND;
        drop.x = x; drop.y = y; drop.room = room; drop.initFlags = 1;
        drop.format = *reinterpret_cast<WORD*>(static_cast<BYTE*>(game) + 0x78);
        drop.quality = roll.quality == RoguelikeAffixes::Quality::Rare ? 6 : 4;
        drop.seed = random(); drop.itemSeed = drop.seed;
        drop.generationFlags = 2 | 8;
        const int prefixOffset = static_cast<int>(affixes->pMagicPrefix - affixes->pMagicAffixTxt);
        const int suffixOffset = static_cast<int>(affixes->pMagicSuffix - affixes->pMagicAffixTxt);
        for (unsigned i = 0; i < prefixes; ++i)
            drop.prefixes[i] = exact.prefixes[i] - prefixOffset;
        for (unsigned i = 0; i < suffixes; ++i)
            drop.suffixes[i] = exact.suffixes[i] - suffixOffset;
        UnitAny* item = createItem(game, &drop, 1);
        if (!item || !Readable(item, sizeof(UnitAny)) || !item->pItemData) {
            error = "native synthetic map item constructor failed"; return false;
        }
        bool resolved = item->dwType == UNIT_ITEM && item->dwTxtFileNo == 737 &&
            item->dwMode == ITEM_MODE_ON_GROUND &&
            item->pItemData->dwQuality == static_cast<DWORD>(drop.quality) &&
            ExactAffixes(item, exact);
        StatList* modifiers = resolved ? D2COMMON_GetItemPropertyList(item, 0, 0x40) : nullptr;
        if (!modifiers || !Readable(modifiers, 0x2C) ||
            modifiers->wStatCount1 > roll.stats.size() ||
            (modifiers->wStatCount1 &&
             !Readable(modifiers->pStat, modifiers->wStatCount1 * sizeof(Stat))))
            resolved = false;
        if (resolved)
            for (unsigned i = 0; i < modifiers->wStatCount1; ++i) {
                const Stat& entry = modifiers->pStat[i];
                if (entry.wStatIndex < 370 || entry.wStatIndex >= tables->dwItemStatCostRecs) {
                    resolved = false; break;
                }
                roll.stats[roll.statCount++] = {entry.wStatIndex, entry.wSubIndex,
                    tables->pItemStatCostTxt[entry.wStatIndex].dwDivide,
                    static_cast<int32_t>(entry.dwStatValue)};
            }
        if (!RemoveRolledItem(game, player, item)) {
            error = "temporary native map item could not be removed safely"; return false;
        }
        if (!resolved) { error = "native map item did not materialize all affix stats"; return false; }
    }
    for (unsigned i = 0; i < roll.count; ++i) {
        if (roll.modifiers[i].id != kZoneAreaAffix) continue;
        if (roll.statCount == roll.stats.size()) {
            error = "native zone stat capacity exceeded"; return false;
        }
        roll.stats[roll.statCount++] = {374, 0, 2001,
            1 + static_cast<int>((std::min)(depth / 10, 4u))};
    }
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        if (stat.mapStat == 374) roll.areaLevel += stat.value;
        if (stat.mapStat == 372) roll.densityPercent += stat.value;
        if (stat.mapStat == 370) roll.magicFind += stat.value;
        if (stat.mapStat == 405) roll.monsterHealthPercent += stat.value;
    }
    return true;
}
struct MapContext {
    std::array<BYTE, 0x400> monster, player;
    std::array<BYTE, 0xC0> global;
    WORD monsters = 0, players = 0, globals = 0;
};
bool ComposeMapContext(const RoguelikeAffixes::Roll& roll,
    MapContext& context, std::string& error) {
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        BYTE* destination;
        WORD* count;
        size_t capacity;
        unsigned category = stat.category;
        if (category < 1000 || category == 0x400 || category == 3000) {
            destination = context.monster.data();
            count = &context.monsters; capacity = context.monster.size() / 8;
            if (category == 3000) category = stat.mapStat;
        } else if (category < 2000) {
            destination = context.player.data();
            count = &context.players; capacity = context.player.size() / 8;
            category -= 1000;
        } else if (category < 3000) {
            destination = context.global.data();
            count = &context.globals; capacity = context.global.size() / 8;
            category -= 2000;
        } else {
            error = "unsupported stock map-item stat category"; return false;
        }
        if (stat.category == 2005 &&
            (stat.mapStat != 498 || stat.value != 1)) {
            error = "stock Duplicated boss modifier changed"; return false;
        }
        if (*count == capacity || stat.mapStat > 0xFFFF || category > 0xFFFF ||
            stat.layer > 0xFFFF) {
            error = "stock map-affix context capacity or stat identity exceeded"; return false;
        }
        BYTE* record = destination + (*count)++ * 8;
        const DWORD header = (category << 16) |
            ((stat.category == 3000 || (stat.category < 1000 && stat.layer)) ?
                stat.layer : stat.mapStat);
        memcpy(record, &header, sizeof(header));
        memcpy(record + 4, &stat.value, sizeof(stat.value));
    }
    // S13 inserts into statlists sorted by target ID, then tags the entry at
    // its original ordinal with a source layer. Feed monster and player
    // records in target order to keep compound affixes attributed correctly.
    const auto sortRecords = [](BYTE* records, WORD count) {
        for (unsigned i = 1; i < count; ++i) {
            BYTE current[8];
            memcpy(current, records + i * 8, sizeof(current));
            DWORD header = 0;
            memcpy(&header, current, sizeof(header));
            unsigned index = i;
            while (index) {
                DWORD preceding = 0;
                memcpy(&preceding, records + (index - 1) * 8,
                    sizeof(preceding));
                if ((preceding >> 16) <= (header >> 16)) break;
                memcpy(records + index * 8, records + (index - 1) * 8, 8);
                --index;
            }
            memcpy(records + index * 8, current, sizeof(current));
        }
    };
    sortRecords(context.monster.data(), context.monsters);
    sortRecords(context.player.data(), context.players);
    return true;
}
// The stock map-world function updates an existing region. Keep all three
// native game arrays live for its monster and player entry consumers; only
// replace a context that this run already owns in the same game/epoch.
bool ApplyMapContext(void* game, const std::string& epoch, int area,
    const RoguelikeAffixes::Roll& roll, int density, int level,
    int baselineDensity, std::string& error) {
    if (!game || area < 13 || area > 254 || !Readable(game, 0x26f2)) {
        error = "native map-affix game context unavailable"; return false;
    }
    std::string digest;
    if (!LabProfile::ModFileIdentity(digest)) {
        error = "pinned PD2 map-affix image unavailable"; return false;
    }
    MapContext next{};
    if (!ComposeMapContext(roll, next, error)) return false;
    BYTE* bytes = static_cast<BYTE*>(game);
    BYTE* monster = bytes + 0x1df8;
    BYTE* player = bytes + 0x21fa;
    BYTE* global = bytes + 0x2630;
    WORD* monsterCount = reinterpret_cast<WORD*>(bytes + 0x21f8);
    WORD* playerCount = reinterpret_cast<WORD*>(bytes + 0x25fa);
    WORD* globalCount = reinterpret_cast<WORD*>(bytes + 0x26f0);
    const bool owned = mapMonsterGame == game && mapMonsterEpoch == epoch;
    if ((!owned && (*monsterCount || *playerCount || *globalCount)) ||
        (owned && (*monsterCount != mapMonsterCount ||
            *playerCount != mapPlayerCount || *globalCount != mapGlobalCount ||
            memcmp(monster, mapMonsterBytes.data(), size_t(mapMonsterCount) * 8) ||
            memcmp(player, mapPlayerBytes.data(), size_t(mapPlayerCount) * 8) ||
            memcmp(global, mapGlobalBytes.data(), size_t(mapGlobalCount) * 8)))) {
        error = "another PD2 map-affix context owns this game"; return false;
    }
    BYTE* region = *reinterpret_cast<BYTE**>(bytes + 0xf0 + area * 4);
    if (!Readable(region, 0x2e4) ||
        *reinterpret_cast<DWORD*>(region + 0x2c0) != static_cast<DWORD>(area) ||
        *reinterpret_cast<DWORD*>(region + 0x2b8) != static_cast<DWORD>(baselineDensity) ||
        *reinterpret_cast<int*>(region + 0x2e0) != level - roll.areaLevel) {
        error = "stock map target region baseline changed"; return false;
    }
    const BYTE* module = reinterpret_cast<const BYTE*>(GetModuleHandleA("ProjectDiablo.dll"));
    const BYTE* entry = module ? module + 0x2dc520 : nullptr;
    const BYTE signature[] = {0x55,0x8b,0xec,0x83,0xec,0x4c,0x56,0x8b,0xf2,0x57,0x8b,0xf9};
    if (!entry || !Readable(entry, sizeof(signature)) ||
        memcmp(entry, signature, sizeof(signature))) {
        error = "stock S13 map-world applicator changed"; return false;
    }
    MapContext previous;
    previous.monsters = *monsterCount;
    previous.players = *playerCount;
    previous.globals = *globalCount;
    memcpy(previous.monster.data(), monster, size_t(previous.monsters) * 8);
    memcpy(previous.player.data(), player, size_t(previous.players) * 8);
    memcpy(previous.global.data(), global, size_t(previous.globals) * 8);
    memcpy(monster, next.monster.data(), size_t(next.monsters) * 8);
    memcpy(player, next.player.data(), size_t(next.players) * 8);
    memcpy(global, next.global.data(), size_t(next.globals) * 8);
    *monsterCount = next.monsters;
    *playerCount = next.players;
    *globalCount = next.globals;
    std::array<BYTE, 0x2e4> regionBefore;
    memcpy(regionBefore.data(), region, regionBefore.size());
    const DWORD oldMultiplier = *reinterpret_cast<DWORD*>(bytes + 0x262c);
    if (next.globals) {
        // Its caller removes the third stack argument: fastcall RET 4 is
        // wrong for this pinned S13 function, even when it seems to work once.
        __asm {
            push 0
            mov edx, area
            mov ecx, game
            mov eax, entry
            call eax
            add esp, 4
        }
    } else {
        *reinterpret_cast<int*>(region + 0x2dc) = level;
        *reinterpret_cast<int*>(region + 0x2e0) = level;
    }
    *reinterpret_cast<DWORD*>(bytes + 0x262c) = oldMultiplier;
    const bool rosterValid = [&] {
        for (unsigned i = 0; i < roll.statCount; ++i) {
            const auto& stat = roll.stats[i];
            if (stat.category != 2003) continue;
            bool present = false;
            for (unsigned slot = 0; slot < region[0x12] && slot < 13; ++slot) {
                WORD monsterClass;
                memcpy(&monsterClass, region + 0x14 + slot * 0x34, sizeof(monsterClass));
                if (monsterClass == stat.value) { present = true; break; }
            }
            if (!present) return false;
        }
        return true;
    }();
    if (*reinterpret_cast<DWORD*>(region + 0x2b8) != static_cast<DWORD>(density) ||
        *reinterpret_cast<int*>(region + 0x2dc) != level ||
        *reinterpret_cast<int*>(region + 0x2e0) != level || !rosterValid) {
        memcpy(region, regionBefore.data(), regionBefore.size());
        memcpy(monster, previous.monster.data(), size_t(previous.monsters) * 8);
        memcpy(player, previous.player.data(), size_t(previous.players) * 8);
        memcpy(global, previous.global.data(), size_t(previous.globals) * 8);
        *monsterCount = previous.monsters;
        *playerCount = previous.players;
        *globalCount = previous.globals;
        error = "native map-world density, area level or added-monster roster disagrees with roll";
        return false;
    }
    mapMonsterGame = game;
    mapMonsterEpoch = epoch;
    mapMonsterCount = next.monsters;
    mapPlayerCount = next.players;
    mapGlobalCount = next.globals;
    memcpy(mapMonsterBytes.data(), monster, size_t(next.monsters) * 8);
    memcpy(mapPlayerBytes.data(), player, size_t(next.players) * 8);
    memcpy(mapGlobalBytes.data(), global, size_t(next.globals) * 8);
    return true;
}
// Stock MapPlayer uses these lazy imports for state 0xC6; verify their
// IAT-backed wrappers before reusing the same lifecycle for a resistance-only list.
bool ProjectDiabloImportThunk(const BYTE* module, DWORD functionRva, DWORD importRva) {
    if (!module) return false;
    const BYTE* thunk = module + functionRva;
    if (!Readable(thunk, 7) || !Readable(module + importRva, sizeof(DWORD)))
        return false;
    DWORD importSlot = 0;
    memcpy(&importSlot, thunk + 2, sizeof(importSlot));
    return thunk[0] == 0x83 && thunk[1] == 0x3d && thunk[6] == 0 &&
        importSlot == reinterpret_cast<DWORD>(module + importRva);
}
bool ZoneResistanceReleaseApiReady(const BYTE* module) {
    return ProjectDiabloImportThunk(module, 0x2780a0, 0x4e83d8) &&
        ProjectDiabloImportThunk(module, 0x277920, 0x4e7218) &&
        Readable(module + 0x2778f0, 8);
}
bool ZoneResistanceStateApiReady(const BYTE* module) {
    return ZoneResistanceReleaseApiReady(module) &&
        ProjectDiabloImportThunk(module, 0x278220, 0x4e8824) &&
        ProjectDiabloImportThunk(module, 0x2783a0, 0x4e7a3c) &&
        ProjectDiabloImportThunk(module, 0x27dd70, 0x4e7cc0);
}
StatList* AllocateZoneResistanceState(void* game, UnitAny* player,
    const BYTE* module, std::string& error) {
    if (!game || !player || player->dwType != UNIT_PLAYER ||
        !ZoneResistanceStateApiReady(module)) {
        error = "pinned native player resistance-state helpers unavailable";
        return nullptr;
    }
    const BYTE* gameBytes = static_cast<const BYTE*>(game);
    if (!Readable(gameBytes + 0x1c, sizeof(void*))) {
        error = "native player resistance-state pool unavailable";
        return nullptr;
    }
    // A zero FOG pool handle selects its default pool for both allocation and free.
    // Match the native flags, initial state, and player owner identity.
    void* pool = *reinterpret_cast<void* const*>(gameBytes + 0x1c);
    StatList* state = D2COMMON_AllocatePropertyList(
        pool, 0x8, 0, player->dwType, player->dwUnitId);
    if (!state) {
        error = "native player resistance-state allocation failed";
        return nullptr;
    }
    using SetStateListId = void (__stdcall*)(StatList*, DWORD);
    reinterpret_cast<SetStateListId>(
        const_cast<BYTE*>(module + 0x278220))(state, 0xc6);
    return state;
}
bool SetZoneResistancePenalty(StatList* state, int penalty,
    std::string& error) {
    if (!state || penalty >= 0 || !Readable(state, sizeof(StatList))) {
        error = "invalid native player resistance-state update";
        return false;
    }
    const int stats[] = {
        STAT_FIRERESIST, STAT_COLDRESIST,
        STAT_LIGHTNINGRESIST, STAT_POISONRESIST
    };
    int original[sizeof(stats) / sizeof(stats[0])]{};
    for (size_t i = 0; i < sizeof(stats) / sizeof(stats[0]); ++i)
        original[i] = D2COMMON_GetStatValueFromStatList(state, stats[i], 0);
    for (size_t i = 0; i < sizeof(stats) / sizeof(stats[0]); ++i) {
        const int updated = original[i] + penalty;
        if (!D2COMMON_SetPropertyStat(state, stats[i], updated, 0) ||
            D2COMMON_GetStatValueFromStatList(state, stats[i], 0) != updated) {
            for (size_t restore = 0; restore < sizeof(stats) / sizeof(stats[0]); ++restore)
                D2COMMON_SetPropertyStat(state, stats[restore], original[restore], 0);
            error = "native elemental resistance state update failed";
            return false;
        }
    }
    return true;
}
bool AttachZoneResistanceState(UnitAny* player, StatList* state,
    const BYTE* module, std::string& error) {
    if (!player || !state || !ZoneResistanceStateApiReady(module)) {
        error = "pinned native player resistance-state helpers unavailable";
        return false;
    }
    // Match the native order: register callback, attach, refresh, then activate.
    using SetStateListCallback = void (__stdcall*)(StatList*, void*);
    reinterpret_cast<SetStateListCallback>(
        const_cast<BYTE*>(module + 0x2783a0))(
            state, const_cast<BYTE*>(module + 0x27dd70));
    D2COMMON_AttachPropertyList(player, state, TRUE);
    using Refresh = void (__stdcall*)(UnitAny*);
    reinterpret_cast<Refresh>(
        const_cast<BYTE*>(module + 0x2778f0))(player);
    using SetUnitState = void (__stdcall*)(UnitAny*, DWORD, BOOL);
    reinterpret_cast<SetUnitState>(
        const_cast<BYTE*>(module + 0x277920))(player, 0xc6, TRUE);
    if (D2COMMON_GetStateStatList(player, 0xc6) != state ||
        !D2COMMON_GetUnitState(player, 0xc6)) {
        error = "native player resistance state did not activate";
        return false;
    }
    return true;
}
bool ReleaseZoneResistanceState(UnitAny* player, StatList* state,
    const BYTE* module, std::string& error) {
    if (!player || !state || !ZoneResistanceReleaseApiReady(module)) {
        error = "pinned native player-state cleanup unavailable";
        return false;
    }
    using Release = void (__stdcall*)(StatList*);
    using Refresh = void (__stdcall*)(UnitAny*);
    using SetUnitState = void (__stdcall*)(UnitAny*, DWORD, BOOL);
    reinterpret_cast<Release>(
        const_cast<BYTE*>(module + 0x2780a0))(state);
    reinterpret_cast<Refresh>(
        const_cast<BYTE*>(module + 0x2778f0))(player);
    if (D2COMMON_GetStateStatList(player, 0xc6)) {
        error = "native player zone state survived cleanup";
        return false;
    }
    if (D2COMMON_GetUnitState(player, 0xc6)) {
        reinterpret_cast<SetUnitState>(
            const_cast<BYTE*>(module + 0x277920))(player, 0xc6, FALSE);
        reinterpret_cast<Refresh>(
            const_cast<BYTE*>(module + 0x2778f0))(player);
        if (D2COMMON_GetUnitState(player, 0xc6)) {
            error = "native player zone-state flag survived cleanup";
            return false;
        }
    }
    return true;
}
void AppendZoneResistanceCleanupError(UnitAny* player, StatList* state,
    const BYTE* module, std::string& error) {
    std::string cleanupError;
    if (!ReleaseZoneResistanceState(player, state, module, cleanupError))
        error += "; resistance-state rollback failed: " + cleanupError;
}
bool PrepareZonePlayerState(void* game, UnitAny* player, bool hasMapAffixes,
    int penalty, const BYTE* module, StatList*& state,
    bool& createdResistanceState, std::string& error) {
    createdResistanceState = false;
    if (penalty && !state) {
        state = AllocateZoneResistanceState(game, player, module, error);
        if (!state) return false;
        createdResistanceState = true;
        if (!SetZoneResistancePenalty(state, penalty, error) ||
            !AttachZoneResistanceState(player, state, module, error)) {
            AppendZoneResistanceCleanupError(player, state, module, error);
            return false;
        }
    } else if (hasMapAffixes || penalty) {
        const BYTE* refresh = module + 0x2778f0;
        if (!Readable(refresh, 8)) {
            error = "stock player state refresh unavailable";
            return false;
        }
        using Refresh = void (__stdcall*)(UnitAny*);
        reinterpret_cast<Refresh>(const_cast<BYTE*>(refresh))(player);
    }
    return true;
}
bool ApplyExistingZoneResistancePenalty(UnitAny* player, StatList* state,
    int penalty, bool createdResistanceState, const BYTE* module,
    std::string& error) {
    if (!penalty || createdResistanceState) return true;
    if (!SetZoneResistancePenalty(state, penalty, error)) return false;
    using Refresh = void (__stdcall*)(UnitAny*);
    reinterpret_cast<Refresh>(
        const_cast<BYTE*>(module + 0x2778f0))(player);
    return true;
}
bool NormalizeZonePlayerAffixes(UnitAny* player, StatList* state,
    const RoguelikeAffixes::Roll& roll, std::string& error) {
    if (!state || !Readable(state, sizeof(StatList))) {
        error = "native player map-affix state is unreadable";
        return false;
    }
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        if (stat.category < 1000 || stat.category >= 2000) continue;
        const int target = static_cast<int>(stat.category - 1000);
        const int observed = D2COMMON_GetStatValueFromStatList(
            state, target, static_cast<WORD>(stat.mapStat));
        const int expected = stat.value ? stat.value : 1;
        if (observed != expected) {
            error = "stock player map-affix state mismatch map stat " +
                std::to_string(stat.mapStat) + " target " +
                std::to_string(target) + ": expected " +
                std::to_string(expected) + ", layer observed " +
                std::to_string(observed) + ", zero layer " +
                std::to_string(D2COMMON_GetStatValueFromStatList(state, target, 0)) +
                ", effective " + std::to_string(
                    D2COMMON_GetUnitStat(player, target, 0)) +
                ", state count " + std::to_string(state->wStatCount1);
            if (state->wStatCount1 <= 32 &&
                Readable(state->pStat, size_t(state->wStatCount1) * sizeof(Stat)))
                for (unsigned entry = 0; entry < state->wStatCount1; ++entry) {
                    const Stat& observedStat = state->pStat[entry];
                    error += " [" + std::to_string(observedStat.wStatIndex) + ":" +
                        std::to_string(observedStat.wSubIndex) + "=" +
                        std::to_string(static_cast<int32_t>(observedStat.dwStatValue)) + "]";
                }
            return false;
        }
    }
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        if (stat.category < 1000 || stat.category >= 2000) continue;
        const int target = static_cast<int>(stat.category - 1000);
        const int value = stat.value ? stat.value : 1;
        if (!D2COMMON_SetPropertyStat(
                state, target, 0, static_cast<WORD>(stat.mapStat)) ||
            !D2COMMON_SetPropertyStat(state, target, value, 0) ||
            D2COMMON_GetStatValueFromStatList(state, target, 0) != value ||
            D2COMMON_GetStatValueFromStatList(
                state, target, static_cast<WORD>(stat.mapStat))) {
            error = "native player statlist layer normalization failed";
            return false;
        }
    }
    return true;
}
bool RetireZonePlayer(void* game, UnitAny* player, int source, std::string& error) {
    if (!mapPlayerAppliedArea) return true;
    if (!game || !player || mapMonsterGame != game ||
        mapPlayerAppliedArea != source || !PortalArea(player, source)) {
        error = "prior zone player-affix owner changed"; return false;
    }
    StatList* state = D2COMMON_GetStateStatList(player, 0xc6);
    if (state) {
        const BYTE* module = reinterpret_cast<const BYTE*>(GetModuleHandleA("ProjectDiablo.dll"));
        if (!ReleaseZoneResistanceState(player, state, module, error)) return false;
    } else if (D2COMMON_GetUnitState(player, 0xc6)) {
        error = "zone player state flag exists without its stat list";
        return false;
    }
    mapPlayerAppliedArea = 0;
    return true;
}
bool ApplyZonePlayer(void* game, UnitAny* player, int area, std::string& error) {
    if (!game || !player || player->dwType != UNIT_PLAYER ||
        area < 13 || area > 254 || !PortalArea(player, area) ||
        !zoneSlots[area].configured || mapMonsterGame != game ||
        mapMonsterEpoch != zoneEpoch ||
        *reinterpret_cast<WORD*>(static_cast<BYTE*>(game) + 0x25fa) != mapPlayerCount ||
        memcmp(static_cast<BYTE*>(game) + 0x21fa, mapPlayerBytes.data(),
            size_t(mapPlayerCount) * 8)) {
        error = "selected player-affix context changed";
        return false;
    }
    if (mapPlayerAppliedArea == area) return true;
    if (mapPlayerAppliedArea) {
        error = "previous zone player state was not retired";
        return false;
    }
    const int penalty = RoguelikeResistance::AdditionalPenalty(
        zoneSlots[area].depth, D2CLIENT_GetDifficulty());
    if (!mapPlayerCount && !penalty) {
        mapPlayerAppliedArea = area;
        return true;
    }
    const BYTE* module =
        reinterpret_cast<const BYTE*>(GetModuleHandleA("ProjectDiablo.dll"));
    if (!module) {
        error = "pinned native player-state module unavailable";
        return false;
    }
    StatList* state = D2COMMON_GetStateStatList(player, 0xc6);
    if (mapPlayerCount) {
        mapPlayerBeforeMagicFind = D2COMMON_GetUnitStat(player, 80, 0);
        if (!state) {
            const BYTE* entry = module + 0x2dba50;
            const BYTE signature[] = {0x55,0x8b,0xec,0x6a,0xff};
            if (!Readable(entry, sizeof(signature)) ||
                memcmp(entry, signature, sizeof(signature))) {
                error = "stock map player applicator changed";
                return false;
            }
            using MapPlayer = int (__fastcall*)(void*, UnitAny*, void*, int);
            const int attached = reinterpret_cast<MapPlayer>(
                const_cast<BYTE*>(entry))(
                    game, player, static_cast<BYTE*>(game) + 0x21fa, area);
            if (!attached) {
                error = "stock player map-affix state was not attached";
                return false;
            }
            state = D2COMMON_GetStateStatList(player, 0xc6);
        }
        if (!state) {
            error = "native player state absent after map entry";
            return false;
        }
        if (!NormalizeZonePlayerAffixes(
                player, state, zoneSlots[area].affixes, error)) return false;
    }
    bool createdResistanceState = false;
    if (!PrepareZonePlayerState(game, player, mapPlayerCount != 0, penalty,
            module, state, createdResistanceState, error)) return false;
    if (mapPlayerCount) {
        mapPlayerAfterMagicFind = D2COMMON_GetUnitStat(player, 80, 0);
        if (zoneSlots[area].affixes.magicFind > 0 &&
            mapPlayerAfterMagicFind - mapPlayerBeforeMagicFind <
                zoneSlots[area].affixes.magicFind) {
            error = "native map-player magic find did not affect item-quality stat";
            return false;
        }
    }
    if (!ApplyExistingZoneResistancePenalty(player, state, penalty,
            createdResistanceState, module, error)) return false;
    mapPlayerAppliedArea = area;
    return true;
}
bool ValidOrdinaryDensity(const Pending::Zone& zone) {
    if (zone.density <= 0) return false;
    if (!zone.depth || zone.testOnly) return true;
    return zone.densityBaseline > 0 && zone.densityBaseline <= 100000 &&
        zone.density == RoguelikeAffixes::NativeDensity(
            zone.densityBaseline, zone.affixes.densityPercent);
}
json ConfigureZone(void* game, UnitAny* player, const Pending& request) {
    const Pending::Zone& zone = *request.zoneJob;
    if (!ObservationTelemetry::MatchesContext(request.expectedEpoch, game, player))
        return {{"ok", false}, {"code", "stale_epoch"}, {"request_id", request.id}};
    if (!game || !player || player->dwType != UNIT_PLAYER || player->dwUnitId != request.playerId ||
        !player->pPath || !player->pPath->pRoom1 || !player->pPath->pRoom1->pRoom2 ||
        !player->pPath->pRoom1->pRoom2->pLevel)
        return Failure("wrong source area for cave configuration");
    const DWORD source = player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
    if ((source != 1 && source != 20 &&
        !((source >= 13 && source <= 16) || (source >= 138 && source <= 254))) || source == zone.level)
        return Failure("wrong source area for cave configuration");
    if (!TargetUnused(player, zone.level))
        return TargetFailure(player, zone.level, request.id);
    if (zone.level >= 138) {
        std::string error;
        if (!InstallArenaActLookup(error))
            return Failure("scoped arena act lookup unavailable: " + error);
    }
    std::string imageError;
    if (!VerifiedPortalEntry(imageError)) return Failure(imageError);
    sgptDataTable* table = Tables();
    if (!table || !table->pLevelsTxt || table->dwLevelsRecs < 255 ||
        table->dwLevelsRecs > 1024 ||
        !Readable(table->pLevelsTxt, size_t(table->dwLevelsRecs) * sizeof(LevelsTxt)) ||
        !table->pMonStatsTxt || !table->dwMonStatsRecs || table->dwMonStatsRecs > 32768 ||
        !Readable(table->pMonStatsTxt, size_t(table->dwMonStatsRecs) * sizeof(MonStatsTxt)) ||
        static_cast<DWORD>(zone.classId) >= table->dwMonStatsRecs)
        return Failure("unrecognized loaded S13 level or monster table");
    const MonStatsTxt& monster = table->pMonStatsTxt[zone.classId];
    const bool ordinary = zone.kind == Pending::ZoneKind::Monster;
    const bool boss = BossZone(zone.kind);
    if (ordinary) {
        if (!zone.monsterClassCount ||
            zone.monsterClassCount > zone.monsterClasses.size() ||
            zone.monsterClasses[0] != zone.classId)
            return Failure("selected monster roster is empty, oversized or has a different primary class");
        for (unsigned index = 0; index < zone.monsterClassCount; ++index) {
            const int classId = zone.monsterClasses[index];
            if (classId < 0 ||
                static_cast<DWORD>(classId) >= table->dwMonStatsRecs ||
                !Readable(table->pMonStatsTxt + classId, sizeof(MonStatsTxt)))
                return Failure("selected monster roster contains an invalid class");
            const MonStatsTxt& candidate = table->pMonStatsTxt[classId];
            if (!candidate.dwFlags.bEnabled || !candidate.dwFlags.bKillable ||
                candidate.dwFlags.bNPC || candidate.dwFlags.binTown)
                return Failure("selected monster roster contains a non-hostile class");
            for (unsigned previous = 0; previous < index; ++previous)
                if (zone.monsterClasses[previous] == classId)
                    return Failure("selected monster roster contains a duplicate class");
        }
    }
    RoguelikeAffixes::Roll mapAffixes = zone.affixes;
    if (boss && !RoguelikeAffixes::EnsureWarlordBossCorruption(mapAffixes))
        return Failure("native boss-corruption zone affix is invalid");
    if (ordinary && (!monster.dwFlags.bEnabled ||
        !monster.dwFlags.bKillable || monster.dwFlags.bNPC ||
        monster.dwFlags.binTown || !ValidOrdinaryDensity(zone)))
        return Failure("selected monster is not a natural hostile class");
    if (!ordinary && (zone.density != 0 || zone.densityBaseline != 0 ||
        zone.monsterClassCount != 0 ||
        (boss && zone.bossSuperuniqueId < -1) ||
        (!boss && zone.bossSuperuniqueId != -1) ||
        ((zone.kind == Pending::ZoneKind::Merchant ||
          zone.kind == Pending::ZoneKind::Shrine ||
          zone.kind == Pending::ZoneKind::Chest) && zone.classId != 5)))
        return Failure("event zone roster or boss identity changed");
    if (boss && (!monster.dwFlags.bEnabled || !monster.dwFlags.bKillable ||
        monster.dwFlags.bNPC || monster.dwFlags.binTown ||
        (zone.bossSuperuniqueId >= 0 &&
         (table->dwSuperUniquesRecs > 32768 ||
          static_cast<DWORD>(zone.bossSuperuniqueId) >= table->dwSuperUniquesRecs))))
        return Failure("selected boss record is no longer spawnable");
    if (zoneEpoch != request.expectedEpoch) {
        std::string error;
        if (!ResetZoneRows(error)) return Failure(error);
        zoneRows = table->pLevelsTxt;
        zoneEpoch = request.expectedEpoch;
        zoneGame = game;
    }
    if (zoneGame != game || zoneRows != table->pLevelsTxt)
        return Failure("game or level table changed during cave configuration");
    ZoneSlot& slot = zoneSlots[zone.level];
    if (slot.configured) return Failure("cave already configured for this game");
    LevelsTxt& row = zoneRows[zone.level];
    if (!CaveBaseline(row, zone.level, table->dwMonStatsRecs))
        return {{"ok", false}, {"code", "malformed_level_table"},
            {"request_id", request.id}, {"to_level", zone.level},
            {"row_level", row.wLevelNo}, {"normal_level", row.wMonLvl[0]},
            {"expansion_level", row.wMonLvlEx[0]}, {"num_mon", row.dwNumMon},
            {"density", {row.dwMonDen[0], row.dwMonDen[1], row.dwMonDen[2]}}};
    // Level TXT writes do not change the already-constructed MonsterRegion.
    // S13 MonsterRegion.cpp RVA CAB30 indexes this game-owned cache by level
    // at game+0xF0; its density at +0x2B8 drives natural room population.
    const auto* regions = reinterpret_cast<void* const*>(
        reinterpret_cast<const BYTE*>(game) + 0xF0);
    if (!Readable(regions + zone.level, sizeof(void*)))
        return Failure("unrecognized server monster region index");
    auto* region = reinterpret_cast<BYTE*>(regions[zone.level]);
    if (!region || !Readable(region, 0x2E4) ||
        *reinterpret_cast<const DWORD*>(region + 0x2C0) != static_cast<DWORD>(zone.level) ||
        *reinterpret_cast<const DWORD*>(region + 0x2B8) != row.dwMonDen[0])
        return Failure("natural monster region absent or baseline density changed");
    const DWORD originalDensity = *reinterpret_cast<DWORD*>(region + 0x2B8);
    const int originalLevel = *reinterpret_cast<int*>(region + 0x2dc);
    const int originalBaseLevel = *reinterpret_cast<int*>(region + 0x2e0);
    LevelsTxt replacement = row;
    replacement.dwNumMon = ordinary && !zone.resumeCleared ?
        zone.monsterClassCount * 0x01010101 : 0;
    for (unsigned difficulty = 0; difficulty < 3; ++difficulty) {
        replacement.wMonLvl[difficulty] = static_cast<WORD>(zone.monsterLevel);
        replacement.wMonLvlEx[difficulty] = static_cast<WORD>(zone.monsterLevel);
        replacement.dwMonDen[difficulty] = zone.density;
    }
    for (unsigned index = 0; index < 25; ++index) {
        replacement.wMon[index] = 0;
        replacement.wNMon[index] = 0;
        replacement.wUMon[index] = 0;
    }
    if (ordinary && !zone.resumeCleared) {
        for (unsigned index = 0; index < zone.monsterClassCount; ++index) {
            const WORD classId = static_cast<WORD>(zone.monsterClasses[index]);
            replacement.wMon[index] = classId;
            replacement.wNMon[index] = classId;
            replacement.wUMon[index] = classId;
        }
    }
    slot.original = row;
    if (!UpdateLevelRow(row, replacement)) {
        UpdateLevelRow(row, slot.original);
        return Failure("cannot write loaded level TXT row safely");
    }
    std::string hookError;
    if (ordinary && !zone.resumeCleared &&
        !ServerMonsters::EnableNaturalZone(game, request.expectedEpoch, zone.level,
            zone.monsterClasses, zone.monsterClassCount,
            zone.monsterLevel, hookError)) {
        if (!UpdateLevelRow(row, slot.original))
            return Failure("natural hook rejected and level row restoration is uncertain: " + hookError);
        return Failure("natural constructor hook rejected: " + hookError);
    }
    // The loaded level row controls natural room creation; the already-built
    // MonsterRegion receives the native map-world density and area-level roll.
    // Automated choices apply stock density affixes to the highest eligible
    // vanilla area density. Explicit/test zones keep their absolute density.
    const int baselineDensity =
        ordinary && zone.depth && !zone.testOnly ?
            zone.densityBaseline : zone.density;
    *reinterpret_cast<DWORD*>(region + 0x2B8) = baselineDensity;
    *reinterpret_cast<int*>(region + 0x2dc) = zone.monsterLevel;
    *reinterpret_cast<int*>(region + 0x2e0) =
        zone.monsterLevel - zone.affixes.areaLevel;
    if (!ApplyMapContext(game, request.expectedEpoch, zone.level,
            mapAffixes, zone.density, zone.monsterLevel, baselineDensity, imageError)) {
        *reinterpret_cast<DWORD*>(region + 0x2B8) = originalDensity;
        *reinterpret_cast<int*>(region + 0x2dc) = originalLevel;
        *reinterpret_cast<int*>(region + 0x2e0) = originalBaseLevel;
        if (!UpdateLevelRow(row, slot.original))
            return Failure("native map context rejected and row restoration is uncertain: " + imageError);
        return Failure("native map context rejected: " + imageError);
    }
    slot.magicFind = zone.magicFind;
    slot.depth = zone.depth;
    slot.densityBaseline = zone.densityBaseline;
    slot.testOnly = zone.testOnly;
    slot.affixes = mapAffixes;
    slot.kind = zone.kind;
    slot.regionDensity = baselineDensity;
    slot.otherShrine = zone.otherShrine;
    slot.secondOtherShrine = zone.secondOtherShrine;
    slot.bossSuperuniqueId = zone.bossSuperuniqueId;
    slot.selectedClassId = zone.classId;
    slot.selectedMonsterClasses = zone.monsterClasses;
    slot.selectedMonsterClassCount = zone.monsterClassCount;
    slot.reward = zone.reward;
    slot.configuredRow = replacement;
    slot.configured = true;
    json selectedMonsterClasses = json::array();
    for (unsigned index = 0; index < zone.monsterClassCount; ++index)
        selectedMonsterClasses.push_back(zone.monsterClasses[index]);
    mapMonsterSampleUnit = 0;
    mapMonsterSampleArea = 0;
    mapMonsterSampleBefore = 0;
    mapMonsterSampleAfter = 0;
    return {{"ok", true}, {"status", "configured"}, {"request_id", request.id},
        {"source", "offline_server"}, {"to_level", zone.level},
        {"monster_class", zone.classId}, {"monster_level", zone.monsterLevel},
        {"monster_classes", std::move(selectedMonsterClasses)},
        {"density", zone.density}, {"region_density", *reinterpret_cast<DWORD*>(region + 0x2B8)},
        {"density_baseline", baselineDensity},
        {"natural_level_hook", ordinary}, {"hp_scaling", "proportional_level_ratio"},
        {"magic_find", zone.magicFind}, {"run_level", zone.depth ? json(zone.depth) : json(nullptr)},
        {"zone_kind", static_cast<int>(zone.kind)},
        {"reward_focus", static_cast<int>(zone.reward)},
        {"zone_affixes", zone.depth ? ZoneAffixReceipt(zone.affixes) : json(nullptr)}};
}
bool RunTileFree(Room1* room, const CollMap* map, int x, int y) {
    if (x < 1 || y < 1 || x >= UINT16_MAX || y >= UINT16_MAX) return false;
    // Keep live actors, objects and ground drops clear of NPC/portal tiles.
    for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
        const int64_t mx = int64_t(x) + ox - map->dwPosGameX;
        const int64_t my = int64_t(y) + oy - map->dwPosGameY;
        if (mx < 0 || my < 0 || mx >= map->dwSizeGameX || my >= map->dwSizeGameY ||
            (map->pMapStart[size_t(my) * map->dwSizeGameX + size_t(mx)] & 0x3E01))
            return false;
    }
    UnitAny* unit = room->pUnitFirst;
    unsigned visited = 0;
    for (; unit && visited < 4096; unit = unit->pRoomNext, ++visited) {
        if (!Readable(unit, sizeof(UnitAny))) return false;
        if (unit->dwType == UNIT_MONSTER &&
            (unit->dwMode == NPC_MODE_DEATH || unit->dwMode == NPC_MODE_DEAD))
            continue;
        int ux, uy;
        if (unit->dwType == UNIT_ITEM) {
            if (!Readable(unit->pItemPath, sizeof(ItemPath))) return false;
            ux = unit->pItemPath->dwPosX; uy = unit->pItemPath->dwPosY;
        } else if (unit->dwType == UNIT_OBJECT) {
            if (!Readable(unit->pObjectPath, sizeof(ObjectPath))) return false;
            ux = unit->pObjectPath->dwPosX; uy = unit->pObjectPath->dwPosY;
        } else {
            if (!Readable(unit->pPath, sizeof(Path))) return false;
            ux = unit->pPath->xPos; uy = unit->pPath->yPos;
        }
        if (std::abs(x - ux) <= 2 && std::abs(y - uy) <= 2) return false;
    }
    return visited < 4096;
}
bool FindPortalTile(Room1* room, UnitAny* player, int& x, int& y) {
    if (!Readable(room->Coll, sizeof(CollMap))) return false;
    const CollMap* map = room->Coll;
    const uint64_t cells = uint64_t(map->dwSizeGameX) * map->dwSizeGameY;
    if (!cells || cells > 65536 ||
        !Readable(map->pMapStart, static_cast<size_t>(cells) * sizeof(WORD)))
        return false;
    const int px = player->pPath->xPos, py = player->pPath->yPos;
    for (int radius = 3; radius <= 16; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (std::abs(dx) != radius && std::abs(dy) != radius) continue;
                if (RunTileFree(room, map, px + dx, py + dy)) {
                    x = px + dx; y = py + dy; return true;
                }
            }
        }
    }
    return false;
}
bool RunFloorOpen(const CollMap* map, int x, int y, const RunTile& entry) {
    const int64_t mx = int64_t(x) - map->dwPosGameX;
    const int64_t my = int64_t(y) - map->dwPosGameY;
    if (mx < 0 || my < 0 || mx >= map->dwSizeGameX ||
        my >= map->dwSizeGameY) return false;
    const WORD flags = map->pMapStart[size_t(my) * map->dwSizeGameX + size_t(mx)];
    WORD blockers = flags & 0x1C09; // D2Common COLLIDE_MASK_PLAYER_PATH
    // D2 stamps the live player's own tile NO_PATH (0x1000). Only the
    // observed entry center can ignore that stamp; never clear actual walls.
    if (x == entry.x && y == entry.y && (flags & 0x80))
        blockers &= ~WORD(0x1000);
    return !blockers;
}
int RunTileDistance(const RunTile& a, const RunTile& b) {
    return (std::max)(std::abs(a.x - b.x), std::abs(a.y - b.y));
}
bool WalkRunFloor(const CollMap* map, const RunTile& entry,
    const RunLayout* obstacles, std::vector<BYTE>& reachable,
    std::vector<size_t>& queue) {
    const auto blocked = [&](int x, int y) {
        if (!obstacles) return false;
        const RunTile tile{x, y};
        for (unsigned i = 0; i < obstacles->actorCount; ++i)
            if (RunTileDistance(tile, obstacles->actors[i]) <= 1) return true;
        for (const RunTile& portal : obstacles->portals)
            if (RunTileDistance(tile, portal) <= 1) return true;
        return false;
    };
    if (!RunFloorOpen(map, entry.x, entry.y, entry) || blocked(entry.x, entry.y))
        return false;
    const size_t width = map->dwSizeGameX;
    const size_t start = size_t(entry.y - map->dwPosGameY) * width +
        size_t(entry.x - map->dwPosGameX);
    reachable[start] = 1;
    queue.push_back(start);
    for (size_t head = 0; head < queue.size(); ++head) {
        const size_t current = queue[head];
        const int x = map->dwPosGameX + int(current % width);
        const int y = map->dwPosGameY + int(current / width);
        for (const RunTile delta : {RunTile{1, 0}, RunTile{-1, 0},
                                    RunTile{0, 1}, RunTile{0, -1}}) {
            const int nx = x + delta.x, ny = y + delta.y;
            if (!RunFloorOpen(map, nx, ny, entry) || blocked(nx, ny)) continue;
            const size_t offset = size_t(ny - map->dwPosGameY) * width +
                size_t(nx - map->dwPosGameX);
            if (reachable[offset]) continue;
            reachable[offset] = 1;
            queue.push_back(offset);
        }
    }
    return true;
}
bool RunLayoutAccessible(const CollMap* map, const RunTile& entry,
    const RunLayout& layout, std::vector<BYTE>& reachable,
    std::vector<size_t>& queue) {
    std::fill(reachable.begin(), reachable.end(), 0);
    queue.clear();
    if (!WalkRunFloor(map, entry, &layout, reachable, queue)) return false;
    const auto approach = [&](const RunTile& tile) {
        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
            const int64_t mx = int64_t(tile.x) + dx - map->dwPosGameX;
            const int64_t my = int64_t(tile.y) + dy - map->dwPosGameY;
            if (mx < 0 || my < 0 || mx >= map->dwSizeGameX ||
                my >= map->dwSizeGameY) continue;
            if (reachable[size_t(my) * map->dwSizeGameX + size_t(mx)])
                return true;
        }
        return false;
    };
    for (unsigned i = 0; i < layout.actorCount; ++i)
        if (!approach(layout.actors[i])) return false;
    for (const RunTile& portal : layout.portals)
        if (!approach(portal)) return false;
    return true;
}
bool ChestTileHasApproachSpace(Room1* room, const CollMap* map,
    const RunTile& tile) {
    unsigned openApproaches = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            if (RunTileFree(room, map, tile.x + dx, tile.y + dy) &&
                ++openApproaches >= 4)
                return true;
        }
    return false;
}
// One collision component and one plan for the whole event, rather than
// placing each sprite at the first free tile three steps from the player.
bool PlanRunLayout(Room1* room, UnitAny* player, unsigned actorCount,
    RunLayout& layout, bool safeChestActors = false) {
    if (!player || !player->pPath || !room ||
        !Readable(room->Coll, sizeof(CollMap)) || actorCount > layout.actors.size())
        return false;
    const CollMap* map = room->Coll;
    const uint64_t cells = uint64_t(map->dwSizeGameX) * map->dwSizeGameY;
    if (!cells || cells > 65536 ||
        !Readable(map->pMapStart, size_t(cells) * sizeof(WORD))) return false;
    const RunTile entry{player->pPath->xPos, player->pPath->yPos};
    std::vector<BYTE> reachable(size_t(cells), 0);
    std::vector<size_t> queue;
    queue.reserve(size_t(cells));
    if (!WalkRunFloor(map, entry, nullptr, reachable, queue)) return false;
    std::vector<RunTile> candidates;
    candidates.reserve(queue.size());
    int minX = INT_MAX, minY = INT_MAX, maxX = 0, maxY = 0;
    for (const size_t cell : queue) {
        const int x = map->dwPosGameX + int(cell % map->dwSizeGameX);
        const int y = map->dwPosGameY + int(cell / map->dwSizeGameX);
        if (!RunTileFree(room, map, x, y)) continue;
        candidates.push_back({x, y});
        minX = (std::min)(minX, x); maxX = (std::max)(maxX, x);
        minY = (std::min)(minY, y); maxY = (std::max)(maxY, y);
    }
    if (candidates.size() < actorCount + layout.portals.size() ||
        maxX - minX < 12 || maxY - minY < 10) return false;
    layout.actorCount = actorCount;
    const bool playerSouth = entry.y >= minY + (maxY - minY) / 2;
    const int actorY = minY + (maxY - minY) * (playerSouth ? 1 : 3) / 4;
    const int portalY = minY + (maxY - minY) * (playerSouth ? 3 : 1) / 4;
    const auto choose = [&](unsigned index, unsigned count, bool portal,
                            RunTile& output) {
        const int anchorX = minX + (maxX - minX) * int(index) / int(count - 1);
        const int anchorY = portal ? portalY : actorY;
        // A 17-tile-wide floor fits four interior merchant feet at four
        // tiles apart; the three choice portals retain wider spacing.
        const int actorSpacing = portal ? 7 : (actorCount == 4 ? 4 : 5);
        int best = INT_MAX;
        bool found = false;
        for (const RunTile& tile : candidates) {
            // Keep tall merchant sprites off the cliff and back wall so
            // their bodies are visibly separate and easy to mouse-target.
            if (!portal && actorCount == 4 &&
                (tile.x < minX + 2 || tile.x > maxX - 2 ||
                 tile.y < minY + 3 || tile.y > maxY - 4))
                continue;
            if (!portal && safeChestActors &&
                (!RoguelikeLoot::InsideRoomMargin(
                    tile.x, tile.y, minX, minY, maxX, maxY, 3) ||
                 !ChestTileHasApproachSpace(room, map, tile)))
                continue;
            bool clear = true;
            for (unsigned i = 0; i < layout.actorCount; ++i) {
                const RunTile& actor = layout.actors[i];
                if (actor.x && RunTileDistance(tile, actor) < actorSpacing)
                    { clear = false; break; }
            }
            if (!clear) continue;
            for (const RunTile& choice : layout.portals)
                if (choice.x && RunTileDistance(tile, choice) < 7)
                    { clear = false; break; }
            if (!clear) continue;
            const int score = 2 * std::abs(tile.x - anchorX) +
                3 * std::abs(tile.y - anchorY);
            if (score < best) { best = score; output = tile; found = true; }
        }
        return found;
    };
    // Reserve the choice side first: four merchants must not displace the
    // final vendor into the portal lane or against the front wall.
    for (unsigned i = 0; i < layout.portals.size(); ++i)
        if (!choose(i, unsigned(layout.portals.size()), true,
                    layout.portals[i])) return false;
    for (unsigned i = 0; i < actorCount; ++i)
        if (!choose(i, actorCount, false, layout.actors[i])) return false;
    return RunLayoutAccessible(map, entry, layout, reachable, queue);
}
json CreatePortal(void* game, UnitAny* player, const Pending& request) {
    const int from = request.portalJob->first;
    const int to = request.portalJob->second;
    if (!ObservationTelemetry::MatchesContext(request.expectedEpoch, game, player))
        return {{"ok", false}, {"code", "stale_epoch"}, {"request_id", request.id}};
    if (!game || !player || player->dwType != UNIT_PLAYER || player->dwUnitId != request.playerId ||
        !player->pPath || !player->pPath->pRoom1 || !player->pPath->pRoom1->pRoom2 ||
        !player->pPath->pRoom1->pRoom2->pLevel ||
        player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo != from)
        return {{"ok", false}, {"code", "wrong_source_area"}, {"request_id", request.id}};
    if (!TargetUnused(player, to))
        return TargetFailure(player, to, request.id);
    if (to != 20 && zoneSlots[to].configured &&
        (!request.zoneJob || request.zoneJob->level != to ||
        request.zoneJob->classId != zoneSlots[to].selectedClassId ||
        request.zoneJob->monsterClassCount != zoneSlots[to].selectedMonsterClassCount ||
        request.zoneJob->monsterClasses != zoneSlots[to].selectedMonsterClasses ||
        request.expectedEpoch != zoneEpoch || game != zoneGame))
        return Failure("configured arena cannot be reused for another portal");
    std::string error;
    const BYTE* entry = VerifiedPortalEntry(error);
    if (!entry) return Failure(error);
    if ((from >= 138 || to >= 138) && !InstallArenaActLookup(error))
        return Failure("scoped arena act lookup unavailable: " + error);
    Room1* room = D2COMMON_GetRoomFromUnit(player);
    int x, y;
    if (request.portalReplacementId) {
        UnitAny* selected = FindServerObject(game, request.portalReplacementId);
        if (!selected || selected->dwOwnerType == UNIT_OBJECT ||
            !Readable(selected->pObjectPath, sizeof(ObjectPath)) ||
            !Readable(selected->pObjectPath->pRoom1, sizeof(Room1)) ||
            !Readable(selected->pObjectPath->pRoom1->pRoom2, sizeof(Room2)) ||
            !Readable(selected->pObjectPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
            selected->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo != from ||
            D2COMMON_GetUnitDistance(player, selected) >= 9)
            return {{"ok", false}, {"code", "selected_trigger_moved"},
                {"request_id", request.id}};
        room = selected->pObjectPath->pRoom1;
        x = selected->pObjectPath->dwPosX;
        y = selected->pObjectPath->dwPosY;
        // Preserve the selected trigger until the stock pair has been verified.
        // Its occupied tile is the only authenticated, reachable choice site;
        // the stock constructor itself decides whether colocation is possible.
    } else {
        if (!room || !room->Coll) return Failure("source room unavailable");
        if (!FindPortalTile(room, player, x, y))
            return Failure("no safe unoccupied portal tile in source room");
    }
    if (!room || !room->Coll || !Readable(room->Coll, sizeof(CollMap)) ||
        x < 1 || y < 1 || x >= UINT16_MAX || y >= UINT16_MAX ||
        int64_t(x) < room->Coll->dwPosGameX ||
        int64_t(y) < room->Coll->dwPosGameY ||
        int64_t(x) >= int64_t(room->Coll->dwPosGameX) + room->Coll->dwSizeGameX ||
        int64_t(y) >= int64_t(room->Coll->dwPosGameY) + room->Coll->dwSizeGameY)
        return Failure("source portal tile outside verified room");
    UnitAny* portal = nullptr;
    int created = 0;
    auto constructor = reinterpret_cast<PortalConstructor>(const_cast<BYTE*>(entry));
    // MSVC x86 thiscall-like custom ABI: EAX actor, eight stack arguments.
    // The selected choice's existing tile is kept occupied until the
    // constructor returns a verified pair; no guessed room frees or moves.
    __asm {
        push 0
        push 60
        lea ecx, portal
        push ecx
        push to
        push y
        push x
        push room
        push game
        xor eax, eax
        call constructor
        mov created, eax
    }
    json result = {{"request_id", request.id}, {"source", "offline_server"},
        {"source_level", from}, {"destination_level", to},
        {"x", x}, {"y", y}, {"constructor_result", created}, {"object_id", nullptr}};
    if (portal && Readable(portal, sizeof(UnitAny)) && portal->dwType == UNIT_OBJECT &&
        portal->dwTxtFileNo == 60) result["object_id"] = portal->dwUnitId;
    result["ok"] = created == 1 && !result["object_id"].is_null();
    result["status"] = result["ok"] == true ? "created" : "uncertain";
    if (result["ok"] == true && portal->dwOwnerType == UNIT_OBJECT) {
        UnitAny* exit = FindServerObject(game, portal->dwOwnerId);
        if (exit && exit->dwOwnerType == UNIT_OBJECT && exit->dwOwnerId == portal->dwUnitId)
            result["return_object_id"] = exit->dwUnitId;
    }
    return result;
}

const wchar_t* RewardFocusName(Pending::RewardFocus reward) {
    switch (reward) {
    case Pending::RewardFocus::Equipment: return L"Equipment";
    case Pending::RewardFocus::Socketables: return L"Socketed gear + gems/runes";
    case Pending::RewardFocus::Unique: return L"Unique gear";
    case Pending::RewardFocus::Set: return L"Set gear";
    case Pending::RewardFocus::Jewelry: return L"Jewelry";
    case Pending::RewardFocus::Charms: return L"Charms";
    }
    return L"Equipment";
}
// S13 SuperUniques rows are 0x34 bytes; the leading fields identify both
// the localized display name and the base monster class.
#pragma pack(push, 1)
struct PortalSuperUniqueNameRow {
    WORD id, name;
    int classId;
    BYTE reserved[0x2C];
};
#pragma pack(pop)
static_assert(sizeof(PortalSuperUniqueNameRow) == 0x34,
    "pinned S13 portal superunique row");
const wchar_t* PortalBossName(const Pending::Zone& zone) {
    if (zone.bossSuperuniqueId < 0) return nullptr;
    sgptDataTable* table = Tables();
    if (!table || !table->dwSuperUniquesRecs ||
        table->dwSuperUniquesRecs > 1024 ||
        static_cast<DWORD>(zone.bossSuperuniqueId) >= table->dwSuperUniquesRecs ||
        !Readable(table->pSuperUniquesTxt,
            size_t(table->dwSuperUniquesRecs) * sizeof(PortalSuperUniqueNameRow)))
        return nullptr;
    const PortalSuperUniqueNameRow& row =
        reinterpret_cast<const PortalSuperUniqueNameRow*>(
            table->pSuperUniquesTxt)[zone.bossSuperuniqueId];
    if (row.id != zone.bossSuperuniqueId || row.classId != zone.classId ||
        !row.name) return nullptr;
    const wchar_t* name = D2LANG_GetLocaleText(row.name);
    if (!name || !Readable(name, sizeof(wchar_t)) || wcsnlen_s(name, 48) == 48)
        return nullptr;
    return name;
}

bool CopyZoneAffixLabel(unsigned id, wchar_t (&name)[32]) {
    const char* source = ZoneAffixName(id);
    if (!source) return false;
    const int sourceLength = static_cast<int>(strnlen_s(source, 31));
    if (sourceLength <= 0) return false;
    const int written = MultiByteToWideChar(
        CODE_PAGE, 0, source, sourceLength, name, 32);
    if (written <= 0 || written >= 32) return false;
    name[written] = L'\0';
    return true;
}
bool FormatChoiceName(wchar_t (&text)[2048], const wchar_t* monsterName,
    const Pending::Zone& zone, bool townEntry) {
    const wchar_t* displayName = monsterName;
    wchar_t prefixName[32]{}, suffixName[32]{};
    bool hasPrefix = false, hasSuffix = false;
    if (BossZone(zone.kind) && zone.bossSuperuniqueId >= 0) {
        displayName = PortalBossName(zone);
        if (!displayName) return false;
    }
    const bool showAffixName = !townEntry &&
        zone.kind == Pending::ZoneKind::Monster &&
        (zone.affixes.quality == RoguelikeAffixes::Quality::Magic ||
         zone.affixes.quality == RoguelikeAffixes::Quality::Rare);
    if (showAffixName) {
        D2MagicAffixDataTbl* tables =
            D2COMMON_10492_DATATBLS_GetMagicAffixDataTables();
        if (!tables || !Readable(tables, sizeof(*tables)) ||
            !tables->pMagicAffixTxt || !tables->pMagicSuffix ||
            !tables->pMagicPrefix || !tables->pAutoMagic ||
            tables->nMagicAffixTxtRecordCount <= 0 ||
            tables->nMagicAffixTxtRecordCount > 32768 ||
            !Readable(tables->pMagicAffixTxt,
                size_t(tables->nMagicAffixTxtRecordCount) * sizeof(AutoMagicTxt)) ||
            zone.affixes.count > zone.affixes.modifiers.size()) return false;
        // Rare zones can roll multiple modifiers per orientation; use the first
        // rolled name of each kind while listing every effect below it.
        for (unsigned i = 0; i < zone.affixes.count; ++i) {
            const unsigned id = zone.affixes.modifiers[i].id;
            const AutoMagicTxt* affix = ZoneAffix(id);
            if (id != kZoneAreaAffix &&
                (!affix || affix < tables->pMagicSuffix ||
                 affix >= tables->pAutoMagic)) return false;
            const bool prefix = id == kZoneAreaAffix ||
                affix >= tables->pMagicPrefix;
            if (prefix && !hasPrefix) {
                if (!CopyZoneAffixLabel(id, prefixName)) return false;
                hasPrefix = true;
            } else if (!prefix && !hasSuffix) {
                if (!CopyZoneAffixLabel(id, suffixName)) return false;
                hasSuffix = true;
            }
        }
    }
    RoguelikePortalLabels::Request request;
    if (townEntry) {
        request.kind = RoguelikePortalLabels::Kind::TownEntry;
    } else if (zone.kind == Pending::ZoneKind::Merchant) {
        request.kind = RoguelikePortalLabels::Kind::Merchant;
    } else if (zone.kind == Pending::ZoneKind::Shrine) {
        request.kind = RoguelikePortalLabels::Kind::Shrine;
    } else if (zone.kind == Pending::ZoneKind::Chest) {
        request.kind = RoguelikePortalLabels::Kind::Chest;
    } else if (BossZone(zone.kind)) {
        request.kind = RoguelikePortalLabels::Kind::Boss;
    } else {
        request.kind = RoguelikePortalLabels::Kind::Monster;
    }
    request.quality = zone.affixes.quality;
    request.monsterName = displayName;
    request.monsterPrefix = hasPrefix ? prefixName : nullptr;
    request.monsterSuffix = hasSuffix ? suffixName : nullptr;
    request.areaLevel = zone.monsterLevel;
    request.density = zone.density;
    request.stats = zone.affixes.stats.data();
    request.statCount = zone.affixes.statCount;
    request.reward = RewardFocusName(zone.reward);
    return RoguelikePortalLabels::Format(request, text, 2048);
}

json CreateZoneShrine(void* game, UnitAny* player, int objectClass,
    BYTE shrineType, const RunTile& tile) {
    if (objectClass != 77 && objectClass != 2 ||
        (shrineType != 1 && (shrineType < 6 || shrineType > 15)))
        return Failure("shrine type outside verified nonhazardous S13 set");
    HMODULE server = GetModuleHandleA("D2Game.dll");
    const BYTE* factory = server ? reinterpret_cast<const BYTE*>(server) + 0xE1D90 : nullptr;
    const BYTE signature[] = {0x83, 0xec, 0x08, 0x56, 0x57, 0x8b, 0xf9};
    if (!factory || !Readable(factory, sizeof(signature)) ||
        memcmp(factory, signature, sizeof(signature)))
        return Failure("unrecognized S13 shrine object constructor");
    Room1* room = D2COMMON_GetRoomFromUnit(player);
    if (!room || !room->Coll ||
        !RunTileFree(room, room->Coll, tile.x, tile.y))
        return Failure("planned shrine tile obstructed");
    const int x = tile.x, y = tile.y;
    UnitAny* object = nullptr;
    __asm {
        push 0
        push 0
        push 1
        push room
        push game
        push y
        push x
        mov ecx, 2
        mov edx, objectClass
        call factory
        mov object, eax
    }
    if (!object || !Readable(object, sizeof(UnitAny)) ||
        object->dwType != UNIT_OBJECT ||
        object->dwTxtFileNo != static_cast<DWORD>(objectClass) ||
        object->dwMode != 0 ||
        !Readable(object->pObjectPath, sizeof(ObjectPath)) ||
        object->pObjectPath->pRoom1 != room ||
        !Readable(object->pObjectData, sizeof(ObjectData)))
        return Failure("neutral shrine creation uncertain; inspect room before retry");
    object->pObjectData->Type = shrineType;
    return {{"ok", true}, {"status", "shrine_created"},
        {"object_id", object->dwUnitId}, {"class_id", objectClass},
        {"x", x}, {"y", y}, {"shrine_type", object->pObjectData->Type}};
}
json CreateZoneChest(void* game, UnitAny* player, const RunTile& tile) {
    HMODULE server = GetModuleHandleA("D2Game.dll");
    const BYTE* factory = server ? reinterpret_cast<const BYTE*>(server) + 0xE1D90 : nullptr;
    const BYTE signature[] = {0x83, 0xec, 0x08, 0x56, 0x57, 0x8b, 0xf9};
    if (!factory || !Readable(factory, sizeof(signature)) ||
        memcmp(factory, signature, sizeof(signature)))
        return Failure("unrecognized S13 chest object constructor");
    Room1* room = D2COMMON_GetRoomFromUnit(player);
    if (!room || !room->Coll ||
        !RunTileFree(room, room->Coll, tile.x, tile.y))
        return Failure("planned chest tile is no longer free and reachable");
    const int x = tile.x, y = tile.y;
    constexpr int objectClass = 397; // Pinned stock lockable sparkly chest.
    UnitAny* object = nullptr;
    __asm {
        push 0
        push 0
        push 1
        push room
        push game
        push y
        push x
        mov ecx, 2
        mov edx, objectClass
        call factory
        mov object, eax
    }
    if (!object || !Readable(object, sizeof(UnitAny)) ||
        object->dwType != UNIT_OBJECT ||
        object->dwTxtFileNo != objectClass || object->dwMode != 0 ||
        !Readable(object->pObjectPath, sizeof(ObjectPath)) ||
        object->pObjectPath->pRoom1 != room ||
        !Readable(object->pObjectData, sizeof(ObjectData)) ||
        !Readable(object->pObjectData->pTxt, sizeof(ObjectTxt)) ||
        object->pObjectData->pTxt->nOperateFn != 4)
        return Failure("native chest creation uncertain; inspect room before retry");
    object->pObjectData->ChestLocked = 1;
    return {{"ok", true}, {"status", "locked_chest_created"},
        {"object_id", object->dwUnitId}, {"class_id", objectClass},
        {"locked", object->pObjectData->ChestLocked != 0},
        {"x", x}, {"y", y}};
}
// Generic S13 D2Game unit constructor (RVA E1D90), not the paired portal constructor.
bool RecordRunOwnedObject(void* game, UnitAny* player, UnitAny* unit, int area) {
    if (!game || !player || !unit || area < 1 || area >= 255 ||
        !Readable(unit, sizeof(UnitAny)) || unit->dwType != UNIT_OBJECT ||
        unit->dwTxtFileNo != 60 || !unit->pObjectPath ||
        !Readable(unit->pObjectPath, sizeof(ObjectPath)))
        return false;
    ObjectPath* path = unit->pObjectPath;
    if (!path->pRoom1 || !Readable(path->pRoom1, sizeof(Room1))) return false;
    Room1* room = path->pRoom1;
    if (!room->pRoom2 || !Readable(room->pRoom2, sizeof(Room2))) return false;
    Room2* room2 = room->pRoom2;
    if (!room2->pLevel || !Readable(room2->pLevel, sizeof(Level))) return false;
    Level* level = room2->pLevel;
    if (level->dwLevelNo != static_cast<DWORD>(area)) return false;
    for (size_t index = 0; index < runOwnedObjectCount; ++index) {
        const RunOwnedObject& owned = runOwnedObjects[index];
        if (owned.game == game && owned.unitId == unit->dwUnitId)
            return owned.unit == unit && owned.playerId == player->dwUnitId &&
                owned.path == path && owned.room == room &&
                owned.room2 == room2 && owned.level == level &&
                owned.area == static_cast<DWORD>(area);
    }
    if (runOwnedObjectCount == kRunOwnedObjectCapacity) return false;
    runOwnedObjects[runOwnedObjectCount++] = {game, unit, path, room, room2,
        level, unit->dwUnitId, player->dwUnitId, static_cast<DWORD>(area)};
    return true;
}
bool IsRunOwnedTrigger(void* game, const Pending& request,
    const Pending::Zone& zone) {
    return request.townEntry ||
        (runGame.load(std::memory_order_acquire) == game &&
         request.expectedEpoch == runEpoch && !zone.testOnly);
}

json CreateTrigger(void* game, UnitAny* player, const Pending& request,
    const RunTile* planned = nullptr) {
    if (!ObservationTelemetry::MatchesContext(request.expectedEpoch, game, player) ||
        !player || player->dwUnitId != request.playerId ||
        !player->pPath || !player->pPath->pRoom1 || !player->pPath->pRoom1->pRoom2 ||
        !player->pPath->pRoom1->pRoom2->pLevel ||
        player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo != request.triggerSource)
        return Failure("stale game or wrong arena");
    if (!hookPairReady.load(std::memory_order_acquire) ||
        !request.triggerJob || *request.triggerJob <= 0 ||
        *request.triggerJob >= 255 || runUsedAreas[*request.triggerJob] ||
        !TargetUnused(player, *request.triggerJob))
        return Failure("choice destination was already generated or click unavailable");
    std::string error;
    if (!VerifiedPortalEntry(error)) return Failure(error);
    if ((*request.triggerJob >= 138 || request.triggerSource >= 138) &&
        !InstallArenaActLookup(error))
        return Failure("scoped arena act lookup unavailable: " + error);
    HMODULE server = GetModuleHandleA("D2Game.dll");
    const BYTE* factory = reinterpret_cast<const BYTE*>(server) + 0xE1D90;
    const BYTE signature[] = {0x83, 0xec, 0x08, 0x56, 0x57, 0x8b, 0xf9};
    if (!Readable(factory, sizeof(signature)) ||
        memcmp(factory, signature, sizeof(signature)))
        return Failure("unrecognized S13 object constructor");
    const Pending::Zone& roll = *request.zoneJob;
    sgptDataTable* table = Tables();
    if (!table || !table->pMonStatsTxt ||
        static_cast<DWORD>(roll.classId) >= table->dwMonStatsRecs ||
        !Readable(table->pMonStatsTxt + roll.classId, sizeof(MonStatsTxt)))
        return Failure("choice monster catalog unavailable");
    const wchar_t* monsterName =
        D2LANG_GetLocaleText(table->pMonStatsTxt[roll.classId].wNameStr);
    if (!monsterName || !Readable(monsterName, sizeof(wchar_t)) ||
        wcsnlen_s(monsterName, 48) == 48)
        return Failure("choice monster name unavailable");
    Room1* room = D2COMMON_GetRoomFromUnit(player);
    if (!room || !room->Coll) return Failure("source room unavailable");
    int x, y;
    if (planned) {
        if (!RunTileFree(room, room->Coll, planned->x, planned->y))
            return Failure("planned choice tile obstructed");
        x = planned->x; y = planned->y;
    } else if (!FindPortalTile(room, player, x, y))
        return Failure("no safe unoccupied trigger tile");
    ChoiceName name{request.playerId, request.triggerSource, request.expectedEpoch, {}};
    if (request.resumeRun && request.resumeSnapshot) {
        RoguelikePortalLabels::Request resumeLabel;
        resumeLabel.kind = RoguelikePortalLabels::Kind::Resume;
        resumeLabel.activeLevel = request.resumeSnapshot->value("active_level", 20);
        if (!RoguelikePortalLabels::Format(resumeLabel, name.text, 2048))
            return Failure("resume label could not be rendered");
    } else if (!FormatChoiceName(name.text, monsterName, roll,
            request.townEntry)) {
        return Failure("choice label could not be rendered");
    }
    UnitAny* trigger = nullptr;
    // ECX=unit type, EDX=object class; stack=x,y,game,room,1,1,0.
    __asm {
        push 0
        push 1
        push 1
        push room
        push game
        push y
        push x
        mov ecx, 2
        mov edx, 60
        call factory
        mov trigger, eax
    }
    const bool placed = trigger && Readable(trigger, sizeof(UnitAny)) &&
        trigger->dwType == UNIT_OBJECT && trigger->dwTxtFileNo == 60 &&
        Readable(trigger->pObjectPath, sizeof(ObjectPath)) &&
        trigger->pObjectPath->pRoom1 == room;
    const bool untouched = TargetUnused(player, *request.triggerJob);
    const bool runOwned = IsRunOwnedTrigger(game, request, roll);
    const bool tracked = !placed || !runOwned ||
        RecordRunOwnedObject(game, player, trigger, request.triggerSource);
    const DWORD triggerId = placed ? trigger->dwUnitId : 0;
    if (placed && runOwned && !tracked) {
        if (player->pPath->pTargetUnit == trigger)
            ReleaseSelectedTarget(player);
        if (!RemoveOwnedObject(game, player, triggerId, trigger))
            runError = "untracked run trigger could not be removed";
    }
    if (placed && untouched && tracked) {
        std::lock_guard<std::mutex> lock(mutex);
        choiceNames[triggerId] = std::move(name);
    }
    return {{"ok", placed && untouched && tracked},
        {"status", placed && untouched && tracked ? "created" : "uncertain"},
        {"request_id", request.id}, {"source", "offline_server"},
        {"source_level", request.triggerSource}, {"destination_level", *request.triggerJob},
        {"object_id", placed ? json(triggerId) : json(nullptr)},
        {"zone_kind", static_cast<int>(roll.kind)},
        {"reward_focus", static_cast<int>(roll.reward)},
        {"x", x}, {"y", y}, {"destinations_fresh", untouched},
        {"run_level", roll.depth ? json(roll.depth) : json(nullptr)},
        {"zone_affixes", roll.depth ? ZoneAffixReceipt(roll.affixes) : json(nullptr)},
        {"monster_level", roll.monsterLevel}, {"density", roll.density},
        {"magic_find", roll.magicFind}};
}
// The pinned S13 destructor removes the server unit but does not broadcast its
// disappearance to this offline client. Both operations are required together.
bool RemoveOwnedObject(void* game, UnitAny* player, DWORD id, UnitAny* expected,
    const char** failureReason) {
    auto fail = [&](const char* reason) {
        if (failureReason) *failureReason = reason;
        return false;
    };
    if (failureReason) *failureReason = nullptr;
    HMODULE server = GetModuleHandleA("D2Game.dll");
    if (!server) return fail("game_module_unavailable");
    if (!expected || !player || !player->pPlayerData ||
        !Readable(player->pPlayerData, 0xA0) || !player->pPlayerData->pClient ||
        !Readable(player->pPlayerData->pClient, 0x1C4))
        return fail("cleanup_context_unavailable");
    void* client = player->pPlayerData->pClient;
    if (*reinterpret_cast<void**>(reinterpret_cast<BYTE*>(client) + 0x1A8) != game)
        return fail("client_game_changed");
    if (FindServerObject(game, id) != expected)
        return fail("owned_object_changed");
    if (!player->pPath || !Readable(player->pPath, sizeof(Path)))
        return fail("player_path_unavailable");
    if (player->pPath->pTargetUnit == expected)
        return fail("selected_target_not_released");
    const BYTE* destructor = reinterpret_cast<const BYTE*>(server) + 0xE20A0;
    const BYTE signature[] = {0x55,0x56,0x57,0x8b,0xf9,0x33,0xed,0x3b,0xfd};
    const BYTE* sender = reinterpret_cast<const BYTE*>(server) + 0x8A3E0;
    const BYTE senderSignature[] = {0x53,0x55,0x8b,0x6c,0x24,0x10,0x56,0x8b,0xf0};
    if (!Readable(destructor, sizeof(signature)) ||
        memcmp(destructor, signature, sizeof(signature)))
        return fail("unit_destructor_signature_changed");
    if (!Readable(sender, sizeof(senderSignature)) ||
        memcmp(sender, senderSignature, sizeof(senderSignature)))
        return fail("client_sender_signature_changed");
    using RemoveUnit = void (__fastcall*)(void*, UnitAny*);
    reinterpret_cast<RemoveUnit>(const_cast<BYTE*>(destructor))(game, expected);
    if (FindServerObject(game, id))
        return fail("server_unit_removal_failed");
    BYTE packet[6] = {0x0A, UNIT_OBJECT, 0, 0, 0, 0};
    memcpy(packet + 2, &id, sizeof(id));
    __asm {
        push 6
        lea ecx, packet
        push ecx
        mov eax, client
        call sender
    }
    return true;
}
// The player path owns the selected target; clearing it does not mutate the
// target unit, which may already have been destroyed during death cleanup.
bool ReleaseSelectedTarget(UnitAny* player) {
    if (!player || !Readable(player->pPath, sizeof(Path))) return false;
    Path* path = player->pPath;
    if (!path->pTargetUnit) return true;
    std::string error;
    if (!LabProfile::VerifyCommon(error)) return false;
    HMODULE common = GetModuleHandleA("D2Common.dll");
    if (!common) return false;
    const BYTE* entry = reinterpret_cast<const BYTE*>(common) + 0x30E20;
    const BYTE signature[] = {0x8b,0x44,0x24,0x04,0x85,0xc0,0x75,0x07,
        0x68,0xdc,0x08,0,0};
    if (!Readable(entry, sizeof(signature)) ||
        memcmp(entry, signature, sizeof(signature)) ||
        reinterpret_cast<const BYTE*>(GetProcAddress(common, MAKEINTRESOURCEA(10420))) != entry)
        return false;
    using SetUnitTarget = void (__stdcall*)(UnitAny*, UnitAny*);
    reinterpret_cast<SetUnitTarget>(const_cast<BYTE*>(entry))(player, nullptr);
    return path->pTargetUnit == nullptr;
}
void ForgetRunOwnedObjectAt(size_t index) {
    if (index >= runOwnedObjectCount) return;
    for (size_t next = index + 1; next < runOwnedObjectCount; ++next)
        runOwnedObjects[next - 1] = runOwnedObjects[next];
    runOwnedObjects[--runOwnedObjectCount] = {};
}
void ForgetRunOwnedObject(void* game, DWORD id, UnitAny* expected) {
    for (size_t index = 0; index < runOwnedObjectCount; ++index) {
        const RunOwnedObject& owned = runOwnedObjects[index];
        if (owned.game == game && owned.unitId == id &&
            (!expected || owned.unit == expected)) {
            ForgetRunOwnedObjectAt(index);
            return;
        }
    }
}
bool RetireSelectedTownEntry(void* game, UnitAny* player, DWORD id,
    UnitAny* trigger, const char** failureReason) {
    if (!RemoveOwnedObject(game, player, id, trigger, failureReason)) return false;
    ForgetRunOwnedObject(game, id, trigger);
    std::lock_guard<std::mutex> lock(mutex);
    choiceNames.erase(id);
    return true;
}
void ForgetRunOwnedObjects() {
    runOwnedObjects.fill(RunOwnedObject{});
    runOwnedObjectCount = 0;
}
bool RunOwnedObjectMatches(void* game, const RunOwnedObject& owned,
    UnitAny* object) {
    if (owned.game != game || !object || !Readable(object, sizeof(UnitAny)) ||
        object != owned.unit || object->dwUnitId != owned.unitId ||
        object->dwType != UNIT_OBJECT || object->dwTxtFileNo != 60 ||
        object->pObjectPath != owned.path ||
        !Readable(owned.path, sizeof(ObjectPath)) ||
        owned.path->pRoom1 != owned.room ||
        !Readable(owned.room, sizeof(Room1)) ||
        owned.room->pRoom2 != owned.room2 ||
        !Readable(owned.room2, sizeof(Room2)) ||
        owned.room2->pLevel != owned.level ||
        !Readable(owned.level, sizeof(Level)))
        return false;
    return owned.level->dwLevelNo == owned.area;
}
json RemoveRunOwnedObjects(void* game, UnitAny* player) {
    if (!game || !player || !Readable(player, sizeof(UnitAny)) ||
        !player->pPath || !Readable(player->pPath, sizeof(Path)))
        return Failure("run portal cleanup context unavailable");
    unsigned attempted = 0, removed = 0, alreadyAbsent = 0, failed = 0;
    std::array<DWORD, kRunOwnedObjectCapacity> clearedIds{};
    size_t clearedCount = 0;
    json failures = json::array();
    auto failObject = [&](DWORD id, const char* reason) {
        ++failed;
        failures.push_back({{"object_id", id}, {"reason", reason}});
    };
    for (size_t index = 0; index < runOwnedObjectCount;) {
        const RunOwnedObject owned = runOwnedObjects[index];
        if (owned.game != game) {
            ++index;
            continue;
        }
        ++attempted;
        UnitAny* object = FindServerObject(game, owned.unitId);
        if (!object) {
            ++alreadyAbsent;
            clearedIds[clearedCount++] = owned.unitId;
            ForgetRunOwnedObjectAt(index);
            continue;
        }
        if (owned.playerId != player->dwUnitId) {
            failObject(owned.unitId, "owner_player_changed");
            ++index;
            continue;
        }
        if (object != owned.unit) {
            // The GUID now names a different unit; never remove that reused object.
            ++alreadyAbsent;
            clearedIds[clearedCount++] = owned.unitId;
            ForgetRunOwnedObjectAt(index);
            continue;
        }
        if (!RunOwnedObjectMatches(game, owned, object)) {
            failObject(owned.unitId, "owned_object_location_changed");
            ++index;
            continue;
        }
        if (player->pPath->pTargetUnit == object &&
            !ReleaseSelectedTarget(player)) {
            failObject(owned.unitId, "selected_target_release_failed");
            ++index;
            continue;
        }
        const char* reason = nullptr;
        if (!RemoveOwnedObject(game, player, owned.unitId, object, &reason)) {
            failObject(owned.unitId, reason ? reason : "object_removal_failed");
            ++index;
            continue;
        }
        ++removed;
        clearedIds[clearedCount++] = owned.unitId;
        ForgetRunOwnedObjectAt(index);
    }
    if (clearedCount) {
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t index = 0; index < clearedCount; ++index)
            choiceNames.erase(clearedIds[index]);
    }
    json result = {{"ok", failed == 0},
        {"status", failed ? "uncertain" : "removed"},
        {"attempted", attempted}, {"removed", removed},
        {"already_absent", alreadyAbsent}, {"failed", failed}};
    if (!failures.empty()) result["failures"] = std::move(failures);
    return result;
}
// S13 D2Common ordinal 10816 creates the destination Room1 on demand from
// the authenticated Act and level ID. D2Game E19C0 performs the normal
// player transition, but neither path creates a paired portal object.
json WarpToFreshLevel(void* game, UnitAny* player, int level) {
    if (!TargetUnused(player, level)) return Failure("warp destination is not fresh");
    std::string error;
    if (!LabProfile::VerifyCommon(error) || !LabProfile::VerifyServer(error))
        return Failure(error);
    if (level >= 138 && !InstallArenaActLookup(error))
        return Failure("scoped arena act lookup unavailable: " + error);
    auto* common = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2Common.dll"));
    auto* server = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2Game.dll"));
    if (!common || !server) return Failure("offline images unavailable");
    const BYTE* allocate = common + 0x3CDF0;
    const BYTE* transfer = server + 0xE19C0;
    const BYTE allocateSignature[] = {0x8b,0x4c,0x24,0x04,0x83,0xec,0x08};
    const BYTE transferSignature[] = {0x83,0xec,0x2c,0x53,0x55,0x8b,0x6c,0x24,0x3c};
    if (!Readable(allocate, sizeof(allocateSignature)) ||
        memcmp(allocate, allocateSignature, sizeof(allocateSignature)) ||
        reinterpret_cast<const BYTE*>(GetProcAddress(
            reinterpret_cast<HMODULE>(const_cast<BYTE*>(common)),
            MAKEINTRESOURCEA(10816))) != allocate ||
        !Readable(transfer, sizeof(transferSignature)) ||
        memcmp(transfer, transferSignature, sizeof(transferSignature)))
        return Failure("unrecognized native level constructor or transfer");
    Room1* source = D2COMMON_GetRoomFromUnit(player);
    if (!source || !Readable(source->pRoom2, sizeof(Room2)) ||
        !Readable(source->pRoom2->pLevel, sizeof(Level))) return Failure("source room unavailable");
    ActMisc* misc = source->pRoom2->pLevel->pMisc;
    if (!Readable(misc, sizeof(ActMisc)) || !Readable(misc->pAct, sizeof(Act)) ||
        misc->pAct->pMisc != misc) return Failure("source act mismatch");
    int x = 0, y = 0;
    using GenerateRoom = Room1* (__stdcall*)(Act*, int, int, int*, int*, int);
    Room1* destination = reinterpret_cast<GenerateRoom>(const_cast<BYTE*>(allocate))(
        misc->pAct, level, 11, &x, &y, 3);
    if (!destination || !Readable(destination, sizeof(Room1)) ||
        !Readable(destination->pRoom2, sizeof(Room2)) ||
        !Readable(destination->pRoom2->pLevel, sizeof(Level)) ||
        destination->pRoom2->pLevel->dwLevelNo != static_cast<DWORD>(level) ||
        x <= 0 || x > UINT16_MAX || y <= 0 || y > UINT16_MAX)
        return Failure("game could not generate a valid destination room");
    int accepted = 0;
    // E19C0 passes ECX as the low (X) word and EAX as the high (Y) word
    // to D2Common ordinal 10533; these are not conventional EAX=X, ECX=Y.
    __asm {
        push 0
        push 0
        push player
        push game
        mov edx, destination
        mov ecx, x
        mov eax, y
        call transfer
        mov accepted, eax
    }
    return {{"ok", accepted != 0}, {"status", accepted ? "transition_requested" : "transfer_rejected"},
        {"destination_level", level}, {"x", x}, {"y", y}};
}
// Called only after the one-room generator has returned; its census is not
// evidence of natural completion while the destination transition is queued.
json EnsureArenaEncounter(void* game, UnitAny* player, int level,
    const std::array<int, 4>& monsterClasses, unsigned monsterClassCount,
    int monsterLevel, unsigned requiredHostiles = RoguelikeProgression::minimumMobPackSize) {
    if (!monsterClassCount || monsterClassCount > monsterClasses.size())
        return Failure("arena encounter roster is empty or oversized");
    for (unsigned index = 0; index < monsterClassCount; ++index) {
        if (monsterClasses[index] < 0 || monsterClasses[index] > 32767)
            return Failure("arena encounter roster contains an invalid class");
        for (unsigned previous = 0; previous < index; ++previous)
            if (monsterClasses[previous] == monsterClasses[index])
                return Failure("arena encounter roster contains duplicate classes");
    }
    ServerMonsters::Request inspect;
    inspect.operation = ServerMonsters::Operation::Area;
    json census = ServerMonsters::Execute(game, player, inspect);
    if (!census.value("ok", false) || census.value("level_id", 0) != level ||
        !census.value("room_catalog_complete", false) ||
        !census.value("population_complete", false) ||
        !census.contains("enemies") || !census["enemies"].is_array())
        return Failure("arena population cannot be certified after arrival");
    const int inactiveHostiles = census.value("inactive_hostile_alive_count", -1);
    if (inactiveHostiles < 0)
        return Failure("arena inactive population count unavailable");
    unsigned livingHostiles = static_cast<unsigned>(inactiveHostiles);
    for (const auto& monster : census["enemies"])
        if (!monster.value("dead", false)) ++livingHostiles;
    const unsigned toSpawn =
        RoguelikeProgression::MissingMobPackMembers(livingHostiles, requiredHostiles);
    if (!toSpawn)
        return {{"ok", true}, {"status", "natural_population"},
            {"living_hostiles", livingHostiles}};
    ServerMonsters::Request seed;
    seed.operation = ServerMonsters::Operation::Spawn;
    seed.encounterLevel = monsterLevel;
    seed.enemies.push_back(ServerMonsters::Enemy{monsterClasses[0], -1,
        ServerMonsters::Rank::Normal, 1});
    for (unsigned i = 0; i < toSpawn; ++i) {
        seed.enemies[0].classId = monsterClasses[i % monsterClassCount];
        const json result = ServerMonsters::Execute(game, player, seed);
        if (!result.value("ok", false) ||
            result.value("spawned_count", 0) != 1 ||
            !result.value("group_observation_complete", false) ||
            !result.contains("enemies") || !result["enemies"].is_array() ||
            result["enemies"].size() != 1)
            return {{"ok", false}, {"status", "seed_failed"}, {"detail", result}};
    }
    const char* status = livingHostiles ?
        "population_augmented" : "empty_arena_seeded";
    return {{"ok", true}, {"status", status},
        {"living_hostiles_before_spawn", livingHostiles}, {"spawned_count", toSpawn}};
}
json EnsureArenaEncounter(void* game, UnitAny* player, int level, int classId,
    int monsterLevel, unsigned requiredHostiles = RoguelikeProgression::minimumMobPackSize) {
    std::array<int, 4> monsterClasses{};
    monsterClasses[0] = classId;
    return EnsureArenaEncounter(game, player, level, monsterClasses, 1,
        monsterLevel, requiredHostiles);
}
json CompleteStarterEncounter(void* game, UnitAny* player) {
    if (runCleared[20] || !runPopulationReady[20] ||
        !PortalArea(player, 20) || runEncounters[20])
        return {{"ok", true}, {"status", "awaiting_native_population"}};
    sgptDataTable* table = *p_D2COMMON_sgptDataTable;
    if (!table || !table->pLevelsTxt || table->dwLevelsRecs <= 20 ||
        table->pLevelsTxt[20].wLevelNo != 20 ||
        (table->pLevelsTxt[20].dwNumMon & 0xff) != 1 ||
        table->pLevelsTxt[20].wMonLvl[0] != 1) {
        runEncounters[20] = 3;
        runError = "authored first-zone monster pool changed";
        return Failure(runError);
    }
    // Keep the authored one-Zombie intro; generated arenas use the pack default.
    json result = EnsureArenaEncounter(game, player, 20,
        table->pLevelsTxt[20].wMon[0], table->pLevelsTxt[20].wMonLvl[0], 1);
    runEncounters[20] = result.value("ok", false) ?
        (result.value("status", "") == "natural_population" ? 1 : 2) : 3;
    if (runEncounters[20] == 3)
        runError = "first-zone population could not be certified";
    return result;
}
unsigned ExtraBossCount(const RoguelikeAffixes::Roll& roll) {
    unsigned count = 0;
    for (unsigned i = 0; i < roll.statCount; ++i)
        if (roll.stats[i].category == 2005 && roll.stats[i].mapStat == 498 &&
            roll.stats[i].value > 0)
            count += static_cast<unsigned>(roll.stats[i].value);
    return count;
}
json SpawnBossEncounter(void* game, UnitAny* player, int level,
    const ZoneSlot& zone) {
    ServerMonsters::Request inspect;
    inspect.operation = ServerMonsters::Operation::Area;
    json before = ServerMonsters::Execute(game, player, inspect);
    if (!before.value("ok", false) || before.value("level_id", 0) != level ||
        !before.value("room_catalog_complete", false) ||
        !before.value("population_complete", false) ||
        before.value("inactive_hostile_alive_count", -1) != 0 ||
        !before.contains("enemies") || !before["enemies"].empty())
        return Failure("boss arena is not an empty certified native population");
    const int boss = zone.selectedClassId;
    if (boss < 0 || boss >= 32768 || zone.bossSuperuniqueId < -1)
        return Failure("invalid procedural boss candidate");
    ServerMonsters::Request spawn;
    spawn.operation = ServerMonsters::Operation::Spawn;
    spawn.encounterLevel = zoneSlots[level].configuredRow.wMonLvl[0];
    spawn.enemies.push_back(ServerMonsters::Enemy{boss,
        zone.bossSuperuniqueId, ServerMonsters::Rank::Normal, 1});
    json result = ServerMonsters::Execute(game, player, spawn);
    if (!result.value("ok", false) ||
        !result.value("group_observation_complete", false) ||
        result.value("spawned_count", 0) != 1 ||
        !result.contains("enemies") || result["enemies"].size() != 1 ||
        result["enemies"][0].value("class_id", -1) != boss ||
        (zone.bossSuperuniqueId >= 0 &&
         result["enemies"][0].value("superunique_id", json(nullptr)) !=
            zone.bossSuperuniqueId))
        return {{"ok", false}, {"status", "boss_spawn_uncertain"}, {"detail", result}};
    sgptDataTable* table = *p_D2COMMON_sgptDataTable;
    if (!table || !table->pMonStatsTxt) return Failure("boss level table unavailable");
    std::array<BossDamageSource, 32> sources{};
    unsigned count = 0;
    for (const char* group : {"enemies", "additional_enemies"}) {
        if (!result.contains(group) || !result[group].is_array())
            return Failure("boss group census incomplete");
        for (const json& unit : result[group]) {
            const int classId = unit.contains("class_id") &&
                unit["class_id"].is_number_integer() ?
                unit["class_id"].get<int>() : -1;
            if (!unit.contains("id") || !unit["id"].is_number_unsigned() ||
                classId < 0 || static_cast<DWORD>(classId) >= table->dwMonStatsRecs ||
                !unit.contains("attributes") ||
                unit["attributes"].value("level", 0) != spawn.encounterLevel ||
                count == sources.size() || !unit["id"].get<DWORD>())
                return Failure("boss damage owner registry incomplete");
            const int nativeLevel = table->pMonStatsTxt[classId].wLevel;
            if (nativeLevel < 1) return Failure("boss native level unavailable");
            UnitAny* owner = FindServerMonster(game, unit["id"].get<DWORD>());
            if (!owner || owner->dwTxtFileNo != static_cast<DWORD>(classId))
                return Failure("boss damage owner unit could not be authenticated");
            sources[count++] = {owner->dwUnitId, owner,
                static_cast<WORD>(nativeLevel)};
    }
    }
    if (!count) return Failure("boss population has no damage owners");
    runBossDamageSources = sources;
    runBossDamageCount = count;
    runBossUnit[level] = result["enemies"][0]["id"].get<DWORD>();
    return {{"ok", true}, {"status", "boss_population"},
        {"boss", result["enemies"][0]}, {"spawn", result}};
}
json CompleteNonCombatZone(void* game, UnitAny* player, int level,
    const ZoneSlot& zone) {
    ServerMonsters::Request inspect;
    inspect.operation = ServerMonsters::Operation::Area;
    json census = ServerMonsters::Execute(game, player, inspect);
    if (!census.value("ok", false) || census.value("level_id", 0) != level ||
        !census.value("room_catalog_complete", false) ||
        !census.value("population_complete", false) ||
        census.value("inactive_hostile_alive_count", -1) != 0 ||
        !census.contains("enemies") || !census["enemies"].empty()) {
        runEncounters[level] = 3;
        runError = "noncombat area has uncertified or hostile native population";
        return Failure(runError);
    }
    RunLayout layout{};
    const unsigned actorCount =
        zone.kind == Pending::ZoneKind::Merchant ? 4u :
        (zone.kind == Pending::ZoneKind::Shrine ||
         zone.kind == Pending::ZoneKind::Chest ? 3u : 0u);
    if (actorCount && !PlanRunLayout(
            D2COMMON_GetRoomFromUnit(player), player, actorCount, layout,
            zone.kind == Pending::ZoneKind::Chest)) {
        runEncounters[level] = 3;
        runError = "no spacious, reachable event and choice layout";
        return Failure(runError);
    }
    json event;
    if (zone.kind == Pending::ZoneKind::Merchant) {
        event = {{"ok", true}, {"status", "market_spawned"},
            {"merchants", json::array()}};
        // Use stock NPC identities and their native commerce/recruiting menus.
        // Kashya is the Act I hireling vendor; Charsi retains her stock menu.
        unsigned actorIndex = 0;
        for (int classId : {148, 154, 147, 150}) {
            const RunTile& tile = layout.actors[actorIndex++];
            json merchant = ServerMonsters::SpawnEventNpc(
                game, player, classId, tile.x, tile.y);
            event["merchants"].push_back(merchant);
            if (!merchant.value("ok", false)) {
                event["ok"] = false;
                event["status"] = "partial_market_event";
                break;
            }
        }
    } else if (zone.kind == Pending::ZoneKind::Chest) {
        event = {{"ok", true}, {"status", "chests_spawned"},
            {"chests", json::array()}};
        for (unsigned i = 0; i < 3; ++i) {
            const RunTile& tile = layout.actors[i];
            json chest = CreateZoneChest(game, player, tile);
            event["chests"].push_back(chest);
            if (!chest.value("ok", false)) {
                event["ok"] = false;
                event["status"] = "partial_chest_event";
                break;
            }
        }
    } else if (zone.kind == Pending::ZoneKind::Shrine) {
        const json healing = CreateZoneShrine(game, player, 77, 1,
            layout.actors[0]);
        event = healing;
        if (healing.value("ok", false)) {
            json others = json::array();
            for (unsigned index = 0; index < 2; ++index) {
                const BYTE type = index == 0 ?
                    zone.otherShrine : zone.secondOtherShrine;
                json other = CreateZoneShrine(game, player, 2, type,
                    layout.actors[index + 1]);
                others.push_back(other);
                if (!other.value("ok", false)) {
                    event["ok"] = false;
                    event["status"] = "partial_shrine_event";
                    event["healing"] = healing;
                    event["others"] = std::move(others);
                    break;
                }
            }
            if (event.value("ok", false)) {
                event["status"] = "shrines_spawned";
                event["healing"] = healing;
                event["others"] = others;
                event["other"] = others[0];
            }
        }
    }
    if (!event.value("ok", false)) {
        runEncounters[level] = 3;
        runError = "noncombat event creation uncertain";
        return event;
    }
    if (zone.kind == Pending::ZoneKind::Shrine) {
        if (!event.contains("others") || !event["others"].is_array() ||
            event["others"].size() != 2) {
            runEncounters[level] = 3;
            runError = "three-shrine identity ledger incomplete";
            return Failure(runError);
        }
        runShrineIds[level] = {event["object_id"].get<DWORD>(),
            event["others"][0]["object_id"].get<DWORD>(),
            event["others"][1]["object_id"].get<DWORD>()};
    }
    if (zone.kind == Pending::ZoneKind::Chest) {
        if (event["chests"].size() != 3) {
            runEncounters[level] = 3;
            runError = "locked chest identity ledger incomplete";
            return Failure(runError);
        }
        for (unsigned i = 0; i < 3; ++i)
            runChestIds[level][i] = event["chests"][i]["object_id"].get<DWORD>();
    }
    runEncounters[level] = 2;
    runCleared[level] = true; // Entry progress, never a kill-certified bonus level.
    const bool choicesOpen = !zoneSlots[level].testOnly;
    if (choicesOpen)
        ServerSpawns::OfferChoices(game, level, actorCount ? &layout : nullptr);
    event["choices_open"] = choicesOpen && runError.empty();
    return event;
}
json CompleteArenaEncounter(void* game, UnitAny* player, int level) {
    if (!zoneSlots[level].configured || !PortalArea(player, level))
        return {{"ok", true}, {"status", "awaiting_native_arrival"}};
    std::string playerError;
    if (!ApplyZonePlayer(game, player, level, playerError)) {
        runError = playerError;
        return Failure(playerError);
    }
    if (!runPopulationReady[level] || runEncounters[level])
        return {{"ok", true}, {"status", "awaiting_native_population"}};
    const ZoneSlot& zone = zoneSlots[level];
    if (zone.kind == Pending::ZoneKind::Merchant ||
        zone.kind == Pending::ZoneKind::Shrine ||
        zone.kind == Pending::ZoneKind::Chest)
        return CompleteNonCombatZone(game, player, level, zone);
    if (BossZone(zone.kind)) {
        json boss = SpawnBossEncounter(game, player, level, zone);
        runEncounters[level] = boss.value("ok", false) ? 2 : 3;
        if (runEncounters[level] == 3) {
            runError = boss.value("error", "boss encounter could not be certified");
            if (boss.contains("detail")) runError += ": " + boss["detail"].dump();
        }
        return boss;
    }
    json result = EnsureArenaEncounter(game, player, level,
        zone.selectedMonsterClasses, zone.selectedMonsterClassCount,
        zone.configuredRow.wMonLvl[0]);
    if (!result.value("ok", false)) {
        runEncounters[level] = 3;
        runError = "native room finished but combat encounter could not be certified";
        return result;
    }
    const unsigned extra = ExtraBossCount(zone.affixes);
    if (extra) {
        if (extra != 1) {
            runEncounters[level] = 3;
            runError = "stock Duplicated boss magnitude changed"; return Failure(runError);
        }
        // S13's enabled 498/category-2005 map affix has no world switch
        // consumer. Reuse the verified native unique rank/spawn path once
        // after natural population, so this boss joins the same clear census.
        ServerMonsters::Request boss;
        boss.operation = ServerMonsters::Operation::Spawn;
        boss.encounterLevel = zone.configuredRow.wMonLvl[0];
        boss.enemies.push_back(ServerMonsters::Enemy{
            zone.selectedClassId, -1, ServerMonsters::Rank::Unique, 1});
        json spawned = ServerMonsters::Execute(game, player, boss);
        if (!spawned.value("ok", false) || spawned.value("spawned_count", 0) != 1 ||
            !spawned.value("group_observation_complete", false) ||
            !spawned.contains("enemies") || spawned["enemies"].size() != 1 ||
            !spawned["enemies"][0].contains("id") ||
            !spawned["enemies"][0]["id"].is_number_unsigned() || !runError.empty()) {
            runEncounters[level] = 3;
            runError = "stock Duplicated unique boss could not be certified";
            return {{"ok", false}, {"status", "extra_boss_failed"}, {"detail", spawned}};
        }
        runBossUnit[level] = spawned["enemies"][0]["id"].get<DWORD>();
        result["extra_boss"] = spawned["enemies"][0];
    }
    runEncounters[level] = result.value("status", "") == "natural_population" ? 1 : 2;
    return result;
}
json ArmRun(void* game, UnitAny* player, const Pending& request);
void CancelTownArm() {
    CombatTelemetry::StopRoguelikeKills();
    if (!ClearRunAuraSkill(runGame.load(std::memory_order_acquire), runPlayer)) {
        OutputDebugStringA("Roguelike town arm cleanup could not detach temporary aura skill\n");
        return;
    }
    runGame.store(nullptr, std::memory_order_release);
    runStarterSettled.store(false, std::memory_order_release);
    runBossKillWitness.store(false, std::memory_order_release);
    runPlayer = nullptr;
    runEpoch.clear();
    runStage.store(0, std::memory_order_release);
    runRootLevel = 0;
    runArrivalLevel = 0;
    runItemUsePackets = 0;
    runItemUseStatus = 0;
}
// S13 sends inventory item-use packet 0x20 and belt item-use packet 0x26.
// The stock inventory callback at CF730 reaches D9BF0; pSpell9 AE4D0
// rejects blank state, so the owned custom items never reach that spell.
bool OwnedRunItemLocation(UnitAny* player, UnitAny* item, bool allowBelt) {
    if (!player || !item || !player->pInventory ||
        item->pItemData->pOwnerInventory != player->pInventory) return false;
    if (item->dwMode == ITEM_MODE_INV_STASH_CUBE_STORE &&
        item->pItemData->NodePage == NODEPAGE_STORAGE &&
        item->pItemData->ItemLocation == STORAGE_INVENTORY) return true;
    return allowBelt && item->dwMode == ITEM_MODE_IN_BELT &&
        item->pItemData->NodePage == NODEPAGE_BELTSLOTS;
}
UnitAny* OwnedUseItem(void* game, UnitAny* player, DWORD id, const char code[4],
    bool allowBelt = false) {
    if (!game || !player || player->dwType != UNIT_PLAYER ||
        !Readable(player->pInventory, sizeof(Inventory)) ||
        player->pInventory->pOwner != player ||
        !Readable(player->pPath, sizeof(Path)) ||
        !ObservationTelemetry::MatchesContext(runEpoch, game, player) ||
        runGame.load(std::memory_order_acquire) != game ||
        runPlayer != player) return nullptr;
    UnitAny* item = FindRolledItem(game, id);
    if (!item || !Readable(item->pItemData, sizeof(ItemData)) ||
        !OwnedRunItemLocation(player, item, allowBelt) ||
        player->pInventory->pCursorItem ||
        player->pPath->pTargetUnit == item) return nullptr;
    const ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
    if (!Readable(text, sizeof(ItemsTxt)) ||
        memcmp(text->szCode, code, 3) ||
        (text->szCode[3] != ' ' && text->szCode[3] != '\0') || !text->buseable ||
        text->bstackable || text->dwpSpell != 9 ||
        (text->wstate != 0 && text->wstate != 0xffff)) return nullptr;
    unsigned visited = 0;
    for (UnitAny* member = D2COMMON_GetItemFromInventory(player->pInventory);
         member && visited++ < 512; member = D2COMMON_GetNextItemFromInventory(member))
        if (member == item) return item;
    return nullptr;
}
// S13 D39B0 is the stock post-use item action: ESI=item, EAX=item,
// ECX=player, stack=game, RET 4. It sends the inventory item update through
// 8C810, then D2D90 removes it from the inventory (D2Common remove at A748)
// and calls E20A0 to destroy the game unit. This is the same branch used by
// D9BF0 after a successful nonstackable pSpell. Preflight before mutation.
bool ConsumeRunItem(void* game, UnitAny* player, UnitAny* item,
    const char code[4], bool allowBelt = false) {
    if (!item || OwnedUseItem(game, player, item->dwUnitId, code, allowBelt) != item ||
        !Readable(player->pPlayerData, 0xA0) ||
        !Readable(player->pPlayerData->pClient, 0x1C4) ||
        *reinterpret_cast<void**>(
            reinterpret_cast<BYTE*>(player->pPlayerData->pClient) + 0x1A8) != game)
        return false;
    const BYTE* server = reinterpret_cast<const BYTE*>(GetModuleHandleA("D2GAME.dll"));
    if (!server) return false;
    const BYTE* consume = server + 0xD39B0;
    const BYTE* remove = server + 0xD2D90;
    const BYTE consumeSignature[] = {0x51,0x53,0x56,0x57,0x8b,0xf9};
    const BYTE removeSignature[] = {0x51,0x53,0x57,0x8b,0x7c,0x24,0x10};
    if (!Readable(consume, sizeof(consumeSignature)) ||
        memcmp(consume, consumeSignature, sizeof(consumeSignature)) ||
        !Readable(remove, sizeof(removeSignature)) ||
        memcmp(remove, removeSignature, sizeof(removeSignature)))
        return false;
    const DWORD id = item->dwUnitId;
    __asm {
        push game
        mov ecx, player
        mov eax, item
        mov esi, item
        call consume
    }
    return FindRolledItem(game, id) == nullptr;
}
bool SupportedAuraServerFunction(WORD function) {
    return RoguelikeAura::SupportedServerFunction(function);
}
const SkillsTxt* RunAuraSkill(const RoguelikeAura::Config* config) {
    sgptDataTable* tables = Tables();
    if (!config || RoguelikeAura::FindBySkillId(config->skillId) != config ||
        !tables || !tables->pSkillsTxt ||
        config->skillId < 0 || static_cast<DWORD>(config->skillId) >= tables->dwSkillsRecs ||
        config->skillLevel < 1 || config->skillLevel > 99 ||
        config->durationSeconds < 1 || config->durationSeconds > 3600 ||
        !SupportedAuraServerFunction(static_cast<WORD>(config->serverFunction)))
        return nullptr;
    const SkillsTxt* skill = tables->pSkillsTxt + config->skillId;
    if (!Readable(skill, sizeof(SkillsTxt)) || skill->wSkillId != config->skillId ||
        !skill->dwFlags.bAura || skill->wSrvDoFunc != config->serverFunction ||
        !skill->wAuraState || skill->wAuraState >= tables->dwStatesTxtRecs)
        return nullptr;
    return skill;
}
using AuraGetUsedSkill = Skill* (__stdcall*)(UnitAny*);
using AuraSetUsedSkill = void (__stdcall*)(UnitAny*, Skill*);
struct RunAuraFunctions {
    AuraGetUsedSkill getUsedSkill = nullptr;
    AuraSetUsedSkill setUsedSkill = nullptr;
};
bool RunAuraSkillAccessorsMatch(AuraGetUsedSkill getUsedSkill,
    AuraSetUsedSkill setUsedSkill) {
    if (!getUsedSkill || !setUsedSkill) return false;
    const BYTE* getter = reinterpret_cast<const BYTE*>(getUsedSkill);
    const BYTE* setter = reinterpret_cast<const BYTE*>(setUsedSkill);
    static const BYTE prefix[] = {0x8b,0x44,0x24,0x04,0x85,0xc0,0x75,0x1f};
    static const BYTE getterTail[] = {0x8b,0x40,0x10,0xc2,0x04,0x00};
    static const BYTE setterTail[] = {
        0x8b,0x4c,0x24,0x08,0x89,0x48,0x10,0xc2,0x08,0x00};
    return Readable(getter, 0x3A) && Readable(setter, 0x3B) &&
        !memcmp(getter, prefix, sizeof(prefix)) &&
        !memcmp(setter, prefix, sizeof(prefix)) &&
        !memcmp(getter + 0x34, getterTail, sizeof(getterTail)) &&
        !memcmp(setter + 0x31, setterTail, sizeof(setterTail));
}
// S13 D2Common ordinals 10511/11103 read/write pInfo+0x10.
bool BindRunAuraSkillContext(RunAuraFunctions& functions) {
    HMODULE common = GetModuleHandleA("D2Common.dll");
    if (!common) {
        runAura.error = "D2Common aura skill context unavailable";
        return false;
    }
    const auto getUsedSkill = reinterpret_cast<AuraGetUsedSkill>(
        GetProcAddress(common, MAKEINTRESOURCEA(10511)));
    const auto setUsedSkill = reinterpret_cast<AuraSetUsedSkill>(
        GetProcAddress(common, MAKEINTRESOURCEA(11103)));
    if (!RunAuraSkillAccessorsMatch(getUsedSkill, setUsedSkill)) {
        runAura.error = "S13 D2Common used-skill accessor signature mismatch";
        return false;
    }
    functions.getUsedSkill = getUsedSkill;
    functions.setUsedSkill = setUsedSkill;
    return true;
}
bool NearbyAuraStatePresent(UnitAny* player, WORD auraState) {
    if (!Readable(player, sizeof(UnitAny)) ||
        !Readable(player->pPath, sizeof(Path)) ||
        !Readable(player->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(player->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(player->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)))
        return false;
    Level* area = player->pPath->pRoom1->pRoom2->pLevel;
    unsigned rooms = 0, units = 0;
    for (Room2* metadata = area->pRoom2First; metadata;
         metadata = metadata->pRoom2Next) {
        if (++rooms > 256 || !Readable(metadata, sizeof(Room2)) ||
            metadata->pLevel != area)
            return false;
        Room1* room = metadata->pRoom1;
        if (!room) continue;
        if (!Readable(room, sizeof(Room1))) return false;
        for (UnitAny* unit = room->pUnitFirst; unit; unit = unit->pRoomNext) {
            if (++units > 8192 || !Readable(unit, sizeof(UnitAny))) return false;
            if (unit->dwType != UNIT_MONSTER ||
                unit->dwMode == NPC_MODE_DEATH || unit->dwMode == NPC_MODE_DEAD ||
                !Readable(unit->pPath, sizeof(Path)) ||
                !Readable(unit->pPath->pRoom1, sizeof(Room1)) ||
                !Readable(unit->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
                unit->pPath->pRoom1->pRoom2->pLevel != area ||
                D2COMMON_GetUnitStat(unit, STAT_ALIGNMENT, 0) != 0)
                continue;
            if (D2COMMON_GetStateStatList(unit, auraState)) return true;
        }
    }
    return false;
}
bool RunAuraStateEstablished(UnitAny* player, const SkillsTxt* skill,
    const RoguelikeAura::Config* config) {
    StatList* state = D2COMMON_GetStateStatList(player, skill->wAuraState);
    bool prayerStatVerified = false;
    if (config->skillId == 99 && state && skill->wPassiveStat1 == 74) {
        D2COMMON_SetPropertyStat(state, 74, config->skillLevel, 0);
        prayerStatVerified =
            D2COMMON_GetStatValueFromStatList(state, 74, 0) ==
                config->skillLevel &&
            D2COMMON_GetUnitStat(player, 74, 0) >=
                static_cast<DWORD>(config->skillLevel);
    }
    const bool statePresent = state != nullptr ||
        NearbyAuraStatePresent(player, skill->wAuraState);
    return RoguelikeAura::ActivationEstablished(
        config->serverFunction, config->skillId, statePresent,
        prayerStatVerified);
}
bool FindRunAuraTarget(UnitAny* player, DWORD& targetId, DWORD& targetType) {
    if (!Readable(player, sizeof(UnitAny)) ||
        !Readable(player->pPath, sizeof(Path)) ||
        !Readable(player->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(player->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(player->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)))
        return false;
    Level* area = player->pPath->pRoom1->pRoom2->pLevel;
    int bestDistance = 51;
    DWORD bestId = 0;
    unsigned rooms = 0, units = 0;
    for (Room2* metadata = area->pRoom2First; metadata;
         metadata = metadata->pRoom2Next) {
        if (++rooms > 256 || !Readable(metadata, sizeof(Room2)) ||
            metadata->pLevel != area)
            return false;
        Room1* room = metadata->pRoom1;
        if (!room) continue;
        if (!Readable(room, sizeof(Room1))) return false;
        for (UnitAny* unit = room->pUnitFirst; unit; unit = unit->pRoomNext) {
            if (++units > 8192 || !Readable(unit, sizeof(UnitAny))) return false;
            if (unit->dwType != UNIT_MONSTER ||
                unit->dwMode == NPC_MODE_DEATH || unit->dwMode == NPC_MODE_DEAD ||
                D2COMMON_GetUnitStat(unit, STAT_ALIGNMENT, 0) != 0 ||
                !Readable(unit->pPath, sizeof(Path)) ||
                !Readable(unit->pPath->pRoom1, sizeof(Room1)) ||
                !Readable(unit->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
                unit->pPath->pRoom1->pRoom2->pLevel != area)
                continue;
            const int distance = D2COMMON_GetUnitDistance(player, unit);
            if (distance < 0 || distance >= bestDistance) continue;
            bestDistance = distance;
            bestId = unit->dwUnitId;
        }
    }
    targetId = bestId ? bestId : player->dwUnitId;
    targetType = bestId ? UNIT_MONSTER : UNIT_PLAYER;
    return true;
}
void StopRunAuraPulse() {
    runAuraPending.store(false, std::memory_order_release);
    runAuraCastState.store(RunAuraCastState::Idle, std::memory_order_release);
    runAuraCastSkill.store(0, std::memory_order_release);
    runAuraCastTarget.store(0, std::memory_order_release);
    runAuraCastTargetType.store(UNIT_PLAYER, std::memory_order_release);
}
void ResetRunAuraLifecycle() {
    StopRunAuraPulse();
    runAura = {};
}
bool ClearRunAuraSkill(void* game, UnitAny* player) {
    if (!runAuraTemporarySkill.pSkillInfo) {
        ResetRunAuraLifecycle();
        return true;
    }
    if (game != runAura.game || !player ||
        !Readable(player, sizeof(UnitAny)) ||
        player->dwUnitId != runAura.playerId ||
        !Readable(player->pInfo, 0x14)) {
        StopRunAuraPulse();
        runAura.error = "temporary aura skill owner unavailable for cleanup";
        return false;
    }
    Skill** link = &player->pInfo->pFirstSkill;
    bool chainValid = true;
    for (unsigned visited = 0; *link; ++visited) {
        if (visited >= 4096 || !Readable(*link, sizeof(Skill))) {
            chainValid = false;
            break;
        }
        if (*link == &runAuraTemporarySkill) {
            *link = runAuraTemporarySkill.pNextSkill;
            break;
        }
        link = &(*link)->pNextSkill;
    }
    if (!chainValid) {
        StopRunAuraPulse();
        runAura.error = "temporary aura skill chain became unreadable";
        return false;
    }
    if (player->pInfo->pRightSkill == &runAuraTemporarySkill)
        player->pInfo->pRightSkill = runAura.previousRight;
    RunAuraFunctions functions{};
    if (!BindRunAuraSkillContext(functions)) {
        StopRunAuraPulse();
        return false;
    }
    if (functions.getUsedSkill(player) == &runAuraTemporarySkill)
        functions.setUsedSkill(player, runAura.previousUsed);
    if (player->pInfo->pRightSkill == &runAuraTemporarySkill ||
        functions.getUsedSkill(player) == &runAuraTemporarySkill) {
        StopRunAuraPulse();
        runAura.error = "temporary aura skill remained selected after cleanup";
        return false;
    }
    runAuraTemporarySkill = {};
    ResetRunAuraLifecycle();
    return true;
}
void FailRunAura(void* game, UnitAny* player, const std::string& error) {
    if (!ClearRunAuraSkill(game, player)) {
        runAura.error = error + "; temporary aura skill cleanup incomplete";
        return;
    }
    runAura.error = error;
}
void PulseRunAura(void* game, UnitAny* player) {
    const RunAuraCastState castState =
        runAuraCastState.load(std::memory_order_acquire);
    if (castState == RunAuraCastState::Idle &&
        !runAuraPending.load(std::memory_order_acquire))
        return;
    if (!game || !player || runAura.game != game ||
        runAura.playerId != player->dwUnitId || runAura.epoch != runEpoch ||
        !runAura.config || runGame.load(std::memory_order_acquire) != game ||
        runPlayer != player ||
        !ObservationTelemetry::MatchesContext(runEpoch, game, player)) {
        if (game == runAura.game && player &&
            player->dwUnitId == runAura.playerId)
            FailRunAura(game, player, "aura run context changed");
        else {
            runAuraPending.store(false, std::memory_order_release);
            runAuraCastState.store(RunAuraCastState::Idle,
                std::memory_order_release);
        }
        return;
    }
    const DWORD frame = *reinterpret_cast<const DWORD*>(
        reinterpret_cast<const BYTE*>(game) + 0xA8);
    if (castState != RunAuraCastState::Idle) {
        if (!PortalArea(player, runStage.load(std::memory_order_acquire))) {
            FailRunAura(game, player, "aura activation left the active run area");
            return;
        }
        if (castState == RunAuraCastState::Failed) {
            FailRunAura(game, player, "native aura selection or target-use packet failed");
            return;
        }
        if (castState != RunAuraCastState::Sent) return;
        const RoguelikeAura::Config* config = runAura.config;
        const SkillsTxt* skill = RunAuraSkill(config);
        if (!skill) {
            FailRunAura(game, player, "configured native Paladin aura unavailable");
            return;
        }
        if (RunAuraStateEstablished(player, skill, config)) {
            UnitAny* item = FindRolledItem(game, runAura.itemId);
            if (!item || !ConsumeRunItem(
                    game, player, item, config->itemCode, true)) {
                FailRunAura(game, player, "native aura activated but item debit failed");
                return;
            }
            runAura.endFrame =
                frame + static_cast<DWORD>(config->durationSeconds * 25);
            runAuraPending.store(true, std::memory_order_release);
            runAuraCastState.store(RunAuraCastState::Idle,
                std::memory_order_release);
            return;
        }
        if (static_cast<LONG>(frame - runAura.activationDeadlineFrame) >= 0)
            FailRunAura(game, player,
                "native skill cast did not establish the requested aura state");
        return;
    }
    if (static_cast<LONG>(frame - runAura.endFrame) >= 0)
        ClearRunAuraSkill(game, player);
}
int UseAuraItem(void* game, UnitAny* player, UnitAny* item,
    const RoguelikeAura::Config* config) {
    const SkillsTxt* skill = RunAuraSkill(config);
    if (!skill) {
        runAura.error = "configured native Paladin aura unavailable";
        return 3;
    }
    if (runAuraPending.load(std::memory_order_acquire) ||
        runAuraCastState.load(std::memory_order_acquire) !=
            RunAuraCastState::Idle)
        return 3;
    if (runAuraTemporarySkill.pSkillInfo) {
        runAura.error = "previous temporary aura skill cleanup incomplete";
        return 3;
    }
    if (!Readable(player, sizeof(UnitAny)) || !Readable(player->pInfo, 0x14) ||
        !Readable(item, sizeof(UnitAny)) ||
        !Readable(reinterpret_cast<const BYTE*>(game) + 0xA8, sizeof(DWORD))) {
        runAura.error = "S13 aura player, item, or frame unavailable";
        return 3;
    }
    RunAuraFunctions functions{};
    if (!BindRunAuraSkillContext(functions)) return 3;
    DWORD targetId = 0, targetType = UNIT_PLAYER;
    if (!FindRunAuraTarget(player, targetId, targetType)) {
        runAura.error = "no valid native aura target context";
        return 3;
    }
    const DWORD frame = *reinterpret_cast<const DWORD*>(
        reinterpret_cast<const BYTE*>(game) + 0xA8);
    runAura = {};
    runAura.game = game;
    runAura.playerId = player->dwUnitId;
    runAura.config = config;
    runAura.epoch = runEpoch;
    runAura.activationDeadlineFrame = frame + 125;
    runAura.itemId = item->dwUnitId;
    runAura.previousFirst = player->pInfo->pFirstSkill;
    runAura.previousRight = player->pInfo->pRightSkill;
    runAura.previousUsed = functions.getUsedSkill(player);
    runAura.error.clear();
    runAuraTemporarySkill = {};
    runAuraTemporarySkill.pSkillInfo = const_cast<SkillsTxt*>(skill);
    runAuraTemporarySkill.mode = 3;
    runAuraTemporarySkill.flags = 0;
    runAuraTemporarySkill.skillLevel =
        static_cast<DWORD>(config->skillLevel);
    runAuraTemporarySkill.itemGUID = -1;
    runAuraTemporarySkill.pNextSkill = runAura.previousFirst;
    player->pInfo->pFirstSkill = &runAuraTemporarySkill;
    if (player->pInfo->pFirstSkill != &runAuraTemporarySkill) {
        runAura = {};
        runAura.error = "S13 temporary aura skill chain install failed";
        return 3;
    }
    runAuraCastSkill.store(static_cast<WORD>(skill->wSkillId),
        std::memory_order_relaxed);
    runAuraCastTarget.store(targetId, std::memory_order_relaxed);
    runAuraCastTargetType.store(targetType, std::memory_order_relaxed);
    runAuraCastState.store(RunAuraCastState::Send, std::memory_order_release);
    return 0;
}
// Packet 0x20 uses the inventory callback; packet 0x26 uses the belt callback.
bool RunItemPacketLocation(const BYTE* packet, UnitAny* player, UnitAny* item,
    bool beltPacket) {
    if (!packet || !player || !item ||
        !Readable(player->pPath, sizeof(Path)) ||
        !Readable(item->pItemData, sizeof(ItemData))) return false;
    const bool itemInBelt = item->dwMode == ITEM_MODE_IN_BELT &&
        item->pItemData->NodePage == NODEPAGE_BELTSLOTS;
    if (itemInBelt != beltPacket) return false;
    DWORD x = 0, y = 0;
    memcpy(&x, packet + 5, sizeof(x));
    memcpy(&y, packet + 9, sizeof(y));
    if (beltPacket) return x == 0 && y == 0;
    return x == player->pPath->xPos && y == player->pPath->yPos;
}
int RunItemUseHook(void* game, UnitAny* player, BYTE* packet, int size,
    bool beltPacket) {
    Chat original = beltPacket ? originalUseBeltItem : originalUseItem;
    if (!original) return 3;
    if (!hookPairReady.load(std::memory_order_acquire) || !packet || size != 13 ||
        packet[0] != (beltPacket ? 0x26 : 0x20))
        return original(game, player, packet, size);
    ++runItemUsePackets;
    runItemUseStatus = 1;
    DWORD itemId;
    memcpy(&itemId, packet + 1, sizeof(itemId));
    UnitAny* item = FindRolledItem(game, itemId);
    if (!item || !Readable(item->pItemData, sizeof(ItemData))) {
        runItemUseStatus = 2;
        return original(game, player, packet, size);
    }
    const ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
    if (!Readable(text, sizeof(ItemsTxt))) {
        runItemUseStatus = 3;
        return 3;
    }
    const RoguelikeLoot::RerollEffect rerollEffect =
        RoguelikeLoot::RerollEffectForCode(text->szCode);
    const bool reroll = rerollEffect != RoguelikeLoot::RerollEffect::None;
    const RoguelikeAura::Config* aura = RoguelikeAura::FindByCode(text->szCode);
    if (!reroll && !aura) return original(game, player, packet, size);
    std::string profileError;
    if (!LabProfile::VerifyServer(profileError)) { runItemUseStatus = 4; return 3; }
    if (!OwnedUseItem(game, player, itemId,
            aura ? aura->itemCode : text->szCode, aura != nullptr)) {
        runItemUseStatus = 5; return 3;
    }
    if (!PortalArea(player, runStage.load(std::memory_order_acquire))) {
        runItemUseStatus = 6; return 3;
    }
    if (!runError.empty()) { runItemUseStatus = 7; return 3; }
    if (!RunItemPacketLocation(packet, player, item, beltPacket)) {
        runItemUseStatus = 8; return 3;
    }
    if (aura) {
        const int result = UseAuraItem(game, player, item, aura);
        runItemUseStatus = result == 0 ? 11 : 12;
        return result;
    }
    const int source = runStage.load(std::memory_order_acquire);
    if (source < 13 || source > 254 || !runCleared[source]) {
        runItemUseStatus = 9; return 3;
    }
    armedReroll = {game, player->dwUnitId, itemId, source, runEpoch, rerollEffect};
    runItemUseStatus = 10;
    return 0;
}
int __fastcall UseInventoryItemHook(void* game, UnitAny* player, BYTE* packet, int size) {
    return RunItemUseHook(game, player, packet, size, false);
}
int __fastcall UseBeltItemHook(void* game, UnitAny* player, BYTE* packet, int size) {
    return RunItemUseHook(game, player, packet, size, true);
}
json BeginRunResume(void* game, UnitAny* player, const Pending& entry,
    DWORD townEntryId, UnitAny* townEntry);
void ContinueRunResume(void* game, UnitAny* player);
// D2Game packet 0x13 callback: intercept only source objects generated by this run.
int __fastcall ClickHook(void* game, UnitAny* player, BYTE* packet, int size) {
    if (!hookPairReady.load(std::memory_order_acquire) ||
        !packet || size != 9 || packet[0] != 0x13)
        return originalClick(game, player, packet, size);
    DWORD unitType;
    memcpy(&unitType, packet + 1, sizeof(unitType));
    if (unitType != UNIT_OBJECT) return originalClick(game, player, packet, size);
    DWORD objectId;
    memcpy(&objectId, packet + 5, sizeof(objectId));
    std::shared_ptr<Pending> choice;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [id, request] : ownedJobs) {
            if (!request->triggerJob || !request->completed ||
                !player || request->playerId != player->dwUnitId ||
                !PortalArea(player, request->triggerSource) ||
                !ObservationTelemetry::MatchesContext(request->expectedEpoch, game, player))
                continue;
            auto receipt = portalReceipts.find(id);
            if (receipt != portalReceipts.end() &&
                receipt->second.second.value("object_id", json()) == objectId) {
                choice = request;
                break;
            }
        }
    }
    if (!choice) return originalClick(game, player, packet, size);
    if (!ObservationTelemetry::MatchesContext(choice->expectedEpoch, game, player) ||
        !player || player->dwUnitId != choice->playerId ||
        !PortalArea(player, choice->triggerSource) ||
        !TargetUnused(player, *choice->triggerJob))
        return 3;
    UnitAny* trigger = FindServerObject(game, objectId);
    if (!trigger || !trigger->pObjectPath ||
        !Readable(trigger->pObjectPath, sizeof(ObjectPath)) ||
        !trigger->pObjectPath->pRoom1 ||
        !Readable(trigger->pObjectPath->pRoom1, sizeof(Room1)) ||
        !trigger->pObjectPath->pRoom1->pRoom2 ||
        !Readable(trigger->pObjectPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !trigger->pObjectPath->pRoom1->pRoom2->pLevel ||
        !Readable(trigger->pObjectPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
        trigger->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo != choice->triggerSource ||
        trigger->dwOwnerType == UNIT_OBJECT) return 3;
    if (armedReroll.itemId) {
        const ArmedReroll armed = std::move(armedReroll);
        armedReroll = {};
        if (armed.game != game || armed.playerId != choice->playerId ||
            armed.epoch != choice->expectedEpoch ||
            armed.source != choice->triggerSource || choice->townEntry ||
            armed.effect == RoguelikeLoot::RerollEffect::None)
            return 3;
        return ServerSpawns::RerollOfferedChoice(game, player, choice, objectId,
            armed.itemId, armed.effect) ? 0 : 3;
    }
    const bool hadSelectedTarget = player->pPath->pTargetUnit == trigger;
    if (!ReleaseSelectedTarget(player)) {
        std::lock_guard<std::mutex> lock(mutex);
        auto& receipt = portalReceipts.at(choice->id).second;
        receipt["status"] = "transition_uncertain";
        receipt["selection"] = {{"code", "target_release_failed"},
            {"object_id", objectId}};
        return 3;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto selected = selectedTriggerEpochBySource.find(choice->triggerSource);
        if (selected != selectedTriggerEpochBySource.end() &&
            selected->second == choice->expectedEpoch) return 3;
        selectedTriggerEpochBySource[choice->triggerSource] = choice->expectedEpoch;
    } // Never replay this round's transition, even if configuration fails.
    json arm = choice->townEntry ? ArmRun(game, player, *choice) :
        json{{"ok", true}, {"status", "already_armed"}};
    if (choice->resumeRun) {
        json resumed = arm.value("ok", false) && choice->resumeSnapshot ?
            BeginRunResume(game, player, *choice, objectId, trigger) :
            Failure("saved route resume request is incomplete");
        const int resumedResult = resumed.value("ok", false) ? 0 : 3;
        std::lock_guard<std::mutex> lock(mutex);
        auto& receipt = portalReceipts.at(choice->id).second;
        receipt["selection"] = {{"arm", std::move(arm)},
            {"resume", std::move(resumed)},
            {"target_released", hadSelectedTarget}};
        receipt["status"] = resumedResult == 0 ?
            "resume_transition_requested" : "transition_uncertain";
        return resumedResult;
    }
    json roll = choice->townEntry ? json{{"ok", true}, {"status", "first_arena"}} :
        ConfigureZone(game, player, *choice);
    json transit = {{"ok", false}, {"error", "one-way route transition not attempted"}};
    int result = 3;
    json population = {{"ok", true}, {"status", "awaiting_native_population"}};
    if (arm.value("ok", false) && roll.value("ok", false)) {
        const int source = choice->triggerSource;
        const int destination = *choice->triggerJob;
        if (source < 1 || source >= 255 || destination < 1 ||
            destination >= 255 ||
            (runRouteNext[source] >= 0 && runRouteNext[source] != destination) ||
            (runRouteParent[destination] >= 0 &&
                runRouteParent[destination] != source)) {
            transit = Failure("selected route conflicts with the saved one-way path");
        } else {
            std::string error;
            if (!RetireZonePlayer(game, player, source, error)) {
                runError = error;
                transit = Failure(error);
            } else {
                bool entryRemoved = !choice->townEntry;
                if (choice->townEntry) {
                    const char* reason = nullptr;
                    entryRemoved = RetireSelectedTownEntry(game, player,
                        objectId, trigger, &reason);
                    if (!entryRemoved) {
                        transit = Failure("selected town entry could not be retired");
                        transit["cleanup_reason"] =
                            reason ? reason : "object_removal_failed";
                    }
                }
                if (entryRemoved) {
                    if (choice->townEntry)
                        runStage.store(source, std::memory_order_release);
                    runArrivalLevel = destination;
                    transit = WarpToFreshLevel(game, player, destination);
                    if (choice->townEntry)
                        transit["source_entry_removed"] = true;
                    if (transit.value("ok", false) || PortalArea(player, destination)) {
                        transit["ok"] = true;
                        transit["status"] = "transition_requested";
                        transit["source_level"] = source;
                        runRouteNext[source] = destination;
                        runRouteParent[destination] = source;
                        if (destination > 0 && destination < 255)
                            runUsedAreas[destination] = true;
                        runBossKillWitness.store(false, std::memory_order_release);
                        runStage.store(destination, std::memory_order_release);
                        runBossDamageCount = 0;
                        runDamageNativeLevel = 0;
                        runBossProjectileHits = 0;
                        runBossProjectileOriginal = runBossProjectileApplied = 0;
                        if (destination == 20) {
                            if (PortalArea(player, 20))
                                CompleteStarterEncounter(game, player);
                        } else if (zoneSlots[destination].configured &&
                            PortalArea(player, destination)) {
                            runPopulationReady[destination] = true;
                            CompleteArenaEncounter(game, player, destination);
                        }
                        result = 0;
                        population["status"] = "transition_requested";
                    }
                    runArrivalLevel = 0;
                }
            }
        }
        if (result != 0 && choice->townEntry) CancelTownArm();
    }
    runArrivalLevel = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& receipt = portalReceipts.at(choice->id).second;
        receipt["selection"] = {{"arm", std::move(arm)}, {"roll", std::move(roll)},
            {"warp", std::move(transit)},
            {"population", std::move(population)},
            {"target_released", hadSelectedTarget}};
        receipt["status"] = result == 0 ? "transition_requested" :
            "transition_uncertain";
    }
    return result;
}

bool InstallClick(std::string& error) {
    if (originalClick && originalUseItem && originalUseBeltItem) {
        if (!hookPairReady.load(std::memory_order_acquire))
            error = "item-use/click callback protections could not be restored";
        return hookPairReady.load(std::memory_order_acquire);
    }
    if (!LabProfile::VerifyServer(error)) return false;
    HMODULE server = GetModuleHandleA("D2GAME.dll");
    if (!server) { error = "offline server is not loaded"; return false; }
    auto* table = reinterpret_cast<Entry*>(reinterpret_cast<BYTE*>(server) + kPacketTableRva);
    Entry* click = table + 0x13;
    Entry* use = table + 0x20;
    Entry* beltUse = table + 0x26;
    const BYTE* clickHandler = reinterpret_cast<const BYTE*>(server) + 0xD0910;
    const BYTE* useHandler = reinterpret_cast<const BYTE*>(server) + 0xCF730;
    const BYTE* beltUseHandler = reinterpret_cast<const BYTE*>(server) + 0xCF5B0;
    const BYTE clickSignature[] = {0x83,0x7c,0x24,0x08,0x09,0x74,0x08,0xb8,0x03,0,0,0};
    const BYTE useSignature[] = {0x51,0x83,0x7c,0x24,0x0c,0x0d,0x57};
    const BYTE beltUseSignature[] = {0x83,0xec,0x08,0x83,0x7c,0x24,0x10,0x0d,0x56,0x8b,0xf2};
    if (!Readable(click, sizeof(Entry)) || !Readable(use, sizeof(Entry)) ||
        !Readable(beltUse, sizeof(Entry)) ||
        !Readable(clickHandler, sizeof(clickSignature)) ||
        memcmp(clickHandler, clickSignature, sizeof(clickSignature)) ||
        !Readable(useHandler, sizeof(useSignature)) ||
        memcmp(useHandler, useSignature, sizeof(useSignature)) ||
        !Readable(beltUseHandler, sizeof(beltUseSignature)) ||
        memcmp(beltUseHandler, beltUseSignature, sizeof(beltUseSignature)) ||
        click->flag != 1 || use->flag != 1 || beltUse->flag != 1 ||
        reinterpret_cast<const BYTE*>(click->callback) != clickHandler ||
        reinterpret_cast<const BYTE*>(use->callback) != useHandler ||
        reinterpret_cast<const BYTE*>(beltUse->callback) != beltUseHandler) {
        error = "unrecognized S13 item-use or object-click callback"; return false;
    }
    const SIZE_T hookSpan = (0x26 - 0x13 + 1) * sizeof(Entry);
    DWORD priorProtection = 0;
    if (!VirtualProtect(click, hookSpan, PAGE_READWRITE, &priorProtection)) {
        error = "item-use/click callback protection denied"; return false;
    }
    originalClick = click->callback;
    originalUseItem = use->callback;
    originalUseBeltItem = beltUse->callback;
    const PVOID expectedClick = reinterpret_cast<PVOID>(originalClick);
    const PVOID expectedUse = reinterpret_cast<PVOID>(originalUseItem);
    const PVOID expectedBeltUse = reinterpret_cast<PVOID>(originalUseBeltItem);
    const PVOID priorClick = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&click->callback),
        reinterpret_cast<PVOID>(ClickHook), expectedClick);
    PVOID priorUse = nullptr, priorBeltUse = nullptr;
    if (priorClick == expectedClick)
        priorUse = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&use->callback),
            reinterpret_cast<PVOID>(UseInventoryItemHook), expectedUse);
    if (priorUse == expectedUse)
        priorBeltUse = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&beltUse->callback),
            reinterpret_cast<PVOID>(UseBeltItemHook), expectedBeltUse);
    const bool installed = priorClick == expectedClick &&
        priorUse == expectedUse && priorBeltUse == expectedBeltUse;
    bool rollbackSucceeded = true;
    if (!installed) {
        if (priorBeltUse == expectedBeltUse)
            rollbackSucceeded &= InterlockedCompareExchangePointer(
                reinterpret_cast<PVOID volatile*>(&beltUse->callback), expectedBeltUse,
                reinterpret_cast<PVOID>(UseBeltItemHook)) ==
                reinterpret_cast<PVOID>(UseBeltItemHook);
        if (priorUse == expectedUse)
            rollbackSucceeded &= InterlockedCompareExchangePointer(
                reinterpret_cast<PVOID volatile*>(&use->callback), expectedUse,
                reinterpret_cast<PVOID>(UseInventoryItemHook)) ==
                reinterpret_cast<PVOID>(UseInventoryItemHook);
        if (priorClick == expectedClick)
            rollbackSucceeded &= InterlockedCompareExchangePointer(
                reinterpret_cast<PVOID volatile*>(&click->callback), expectedClick,
                reinterpret_cast<PVOID>(ClickHook)) ==
                reinterpret_cast<PVOID>(ClickHook);
    }
    DWORD ignored = 0;
    const bool protectionRestored =
        VirtualProtect(click, hookSpan, priorProtection, &ignored) != 0;
    if (!installed) {
        if (rollbackSucceeded) {
            originalClick = nullptr;
            originalUseItem = nullptr;
            originalUseBeltItem = nullptr;
        }
        error = rollbackSucceeded ?
            "cannot install all pinned item-use and object-click callbacks" :
            "item-use callback rollback incomplete; pass-through retained";
        return false;
    }
    if (!protectionRestored) {
        error = "item-use/click callback page protection restore failed";
        return false;
    }
    hookPairReady.store(true, std::memory_order_release);
    return true;
}


json RetireOwnedPortals(void* game, UnitAny* player, const Pending& request) {
    if (!ObservationTelemetry::MatchesContext(request.expectedEpoch, game, player) ||
        !player || player->dwUnitId != request.playerId ||
        !Readable(player->pPath, sizeof(Path)))
        return Failure("game epoch or player path changed");
    const int from = request.retireFrom;
    const size_t count = from == 1 ? 2 : 4;
    std::array<DWORD, 4> ids{};
    bool offered[255] = {};
    int selectedLevel = 0;
    size_t choices = 0;
    bool initialRetired = false;
    const std::uint64_t retirementSerial = request.runSerial ?
        request.runSerial : runActiveSerial;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (from != 1) {
            auto selected = selectedTriggerEpochBySource.find(from);
            if (selected == selectedTriggerEpochBySource.end() ||
                selected->second != request.expectedEpoch)
                return Failure("no owned choice selected from this arena in this game");
        }
        for (const auto& [id, pending] : ownedJobs) {
            if (!pending->completed || pending->expectedEpoch != request.expectedEpoch)
                continue;
            if (pending->triggerJob && retirementSerial &&
                pending->runSerial != retirementSerial)
                continue;
            auto receipt = portalReceipts.find(id);
            if (receipt == portalReceipts.end()) continue;
            const json& observed = receipt->second.second;
            if (from == 1 && pending->portalJob &&
                *pending->portalJob == std::make_pair(1, 20) &&
                observed.value("status", "") == "created") {
                if (ids[0] || !observed.contains("object_id") ||
                    !observed["object_id"].is_number_unsigned() ||
                    !observed.contains("return_object_id") ||
                    !observed["return_object_id"].is_number_unsigned())
                    return Failure("initial portal pair unverified");
                ids[0] = observed["object_id"].get<DWORD>();
                ids[1] = observed["return_object_id"].get<DWORD>();
            } else if (from == 20 && pending->portalJob &&
                *pending->portalJob == std::make_pair(1, 20) &&
                observed.value("status", "") == "retired") {
                initialRetired = true;
            } else if (from != 1 && pending->triggerJob && pending->triggerSource == from) {
                const int level = *pending->triggerJob;
                if (!((level >= 13 && level <= 16) || (level >= 138 && level <= 254)) ||
                    offered[level] || choices == 3 || !observed.contains("object_id") ||
                    !observed["object_id"].is_number_unsigned())
                    return Failure("invalid owned choice ledger");
                offered[level] = true;
                ++choices;
                if (observed.value("status", "") == "transition_requested" &&
                    observed.contains("selection") && observed["selection"].is_object() &&
                    observed["selection"].contains("portal")) {
                    if (selectedLevel || observed["selection"].value("retired_trigger_id", json()) !=
                        observed["object_id"]) return Failure("selected trigger removal unverified");
                    const json& created = observed["selection"]["portal"];
                    if (!created.value("ok", false) || !created.contains("object_id") ||
                        !created["object_id"].is_number_unsigned() ||
                        !created.contains("return_object_id") ||
                        !created["return_object_id"].is_number_unsigned())
                        return Failure("selected portal pair unverified");
                    ids[0] = created["object_id"].get<DWORD>();
                    ids[1] = created["return_object_id"].get<DWORD>();
                    selectedLevel = level;
                } else {
                    // Receipts are keyed by nonce, not by choice ordering.
                    if (!ids[2]) ids[2] = observed["object_id"].get<DWORD>();
                    else if (!ids[3]) ids[3] = observed["object_id"].get<DWORD>();
                    else return Failure("more than two unselected choices");
                }
            }
        }
    }
    if (from == 1 ? !PortalArea(player, 20) :
        (choices != 3 || !selectedLevel || !PortalArea(player, selectedLevel) ||
         !ids[2] || !ids[3] || (from == 20 && !initialRetired)))
        return Failure("player not in destination or round ledger incomplete");
    std::array<UnitAny*, 4> units{};
    for (size_t index = 0; index < count; ++index) {
        if (!ids[index]) return Failure("owned portal ledger incomplete");
        for (size_t other = 0; other < index; ++other)
            if (ids[other] == ids[index]) return Failure("owned portal identities collide");
        units[index] = FindServerObject(game, ids[index]);
        if (!units[index] || !units[index]->pObjectPath ||
            !Readable(units[index]->pObjectPath, sizeof(ObjectPath)) ||
            !units[index]->pObjectPath->pRoom1 ||
            !Readable(units[index]->pObjectPath->pRoom1, sizeof(Room1)) ||
            !units[index]->pObjectPath->pRoom1->pRoom2 ||
            !Readable(units[index]->pObjectPath->pRoom1->pRoom2, sizeof(Room2)) ||
            !units[index]->pObjectPath->pRoom1->pRoom2->pLevel ||
            !Readable(units[index]->pObjectPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
            units[index]->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo !=
                (from == 1 ? (index == 0 ? 1 : 20) :
                 index == 1 ? selectedLevel : from))
            return Failure("owned portal unit missing or in unexpected area");
    }
    if (units[0]->dwOwnerType != UNIT_OBJECT || units[0]->dwOwnerId != ids[1] ||
        units[1]->dwOwnerType != UNIT_OBJECT || units[1]->dwOwnerId != ids[0])
        return Failure("reciprocal portal owner changed");
    // Only references to owned units need releasing; movement, combat and
    // unrelated targets do not prevent retirement.
    UnitAny* ownedTarget = nullptr;
    for (size_t index = 0; index < count; ++index)
        if (player->pPath->pTargetUnit == units[index]) ownedTarget = units[index];
    if (ownedTarget &&
        (player->pPath->dwTargetType != UNIT_OBJECT ||
         player->pPath->dwTargetId != ownedTarget->dwUnitId))
        return Failure("owned portal target identity changed");
    std::string error;
    if (!VerifiedPortalEntry(error)) return Failure(error);
    // Each mutation independently rechecks the pinned removal entry and client binding.
    if (!request.retireCommit)
        return {{"ok", true}, {"status", "ready"}, {"request_id", request.id},
            {"owned_objects", std::vector<DWORD>(ids.begin(), ids.begin() + count)},
            {"player_target_id", ownedTarget ? json(ownedTarget->dwUnitId) : json(nullptr)}};
    if (ownedTarget && !ReleaseSelectedTarget(player))
        return Failure("owned portal target release failed");
    json removed = json::array();
    for (size_t index = 0; index < count; ++index) {
        if (!RemoveOwnedObject(game, player, ids[index], units[index]))
            return {{"ok", false}, {"status", "uncertain"}, {"removed", removed},
                {"error", "owned unit changed or client removal failed during cleanup"}};
        removed.push_back(ids[index]);
        std::lock_guard<std::mutex> lock(mutex);
        choiceNames.erase(ids[index]);
        ForgetRunOwnedObject(game, ids[index], units[index]);
    }
    if (from == 1) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [id, pending] : ownedJobs) {
            if (pending->completed && pending->expectedEpoch == request.expectedEpoch &&
                pending->portalJob && *pending->portalJob == std::make_pair(1, 20)) {
                auto receipt = portalReceipts.find(id);
                if (receipt != portalReceipts.end() &&
                    receipt->second.second.value("object_id", json()) == ids[0])
                    receipt->second.second["status"] = "retired";
            }
        }
    }
    return {{"ok", true}, {"status", "retired"}, {"request_id", request.id},
        {"removed", removed}, {"client_removals_queued", removed.size()}};
}

bool CaptureRunCharacterName(UnitAny* player, std::string& character);
json ArmRun(void* game, UnitAny* player, const Pending& request) {
    const int rootLevel = request.triggerJob.value_or(20);
    const bool supportedRoot = rootLevel == 20 ||
        (rootLevel >= 13 && rootLevel <= 16) ||
        (rootLevel >= 138 && rootLevel <= 254);
    if (!ObservationTelemetry::MatchesContext(request.expectedEpoch, game, player) ||
        !player || player->dwUnitId != request.playerId || !supportedRoot ||
        !(request.townEntry ? PortalArea(player, 1) : PortalArea(player, 20)) ||
        runGame.load(std::memory_order_acquire))
        return Failure("run must start in town or at its fresh root with the owning player");
    std::string error;
    std::string characterName;
    if (!CaptureRunCharacterName(player, characterName))
        return Failure("cannot capture safe character route-snapshot identity");
    if (!VerifiedPortalEntry(error) || !InstallChoiceNames(error))
        return Failure("run object or labels unavailable: " + error);
    if (!CombatTelemetry::ListenForRoguelikeKills(ServerSpawns::OnNaturalKill,
        ServerSpawns::ScaleRoguelikeDamage, error))
        return Failure("native death event unavailable: " + error);
    const std::uint64_t activeSerial = request.runSerial ?
        request.runSerial : ++runEntrySerial;
    json retired = json::array();
    if (!request.townEntry) {
        Pending retire = request;
        retire.retireJob = true;
        retire.retireFrom = 1;
        retire.retireCommit = true;
        json result = RetireOwnedPortals(game, player, retire);
        if (!result.value("ok", false)) {
            CombatTelemetry::StopRoguelikeKills();
            return Failure("initial town/Tower portal pair not safely retired");
        }
        retired = result["removed"];
    }
    runActiveSerial = activeSerial;
    runCharacterName = std::move(characterName);
    runPlayer = player;
    runDeathHandled.store(false, std::memory_order_release);
    runEpoch = request.expectedEpoch;
    runRootLevel = rootLevel;
    runStage.store(rootLevel, std::memory_order_release);
    runStarterSettled.store(false, std::memory_order_release);
    runBossKillWitness.store(false, std::memory_order_release);
    runArrivalLevel = 0;
    runKillLootLedger.fill(0);
    runKillLootLedgerLevel = 0;
    runRetiredDestinations.fill(false);
    runCleared.fill(false);
    runRouteNext.fill(-1);
    runRouteParent.fill(-1);
    runResume = {};
    {
        std::lock_guard<std::mutex> lock(mutex);
        selectedTriggerEpochBySource.clear();
    }
    runPopulationReady.fill(false);
    runEncounters.fill(0);
    runBossUnit.fill(0);
    runBossDamageCount = 0;
    runScaledHits = 0;
    runScaledMissileHits = 0;
    runBossProjectileHits = 0;
    runBossProjectileOriginal = runBossProjectileApplied = 0;
    runRejectedMissileOwners = 0;
    runObservedMissileDamage = 0;
    runOtherAttackerDamage = 0;
    runMissileDamageSample = {};
    runDamageNativeLevel = 0;
    runShrineIds.fill({});
    runClearRewardIds.fill({});
    runStarterRewardId = 0;
    naturalEquipmentCalls = naturalEquipmentForced = naturalEquipmentRejected = 0;
    naturalEquipmentQuality = naturalEquipmentBase = 0;
    observedAkaraPerms = observedAkaraPotions = 0;
    observedAkaraTrade = false;
    filteredAkaraPotions = 0;
    runChestIds.fill({});
    runChestRewardIds.fill({});
    runChestRewardCounts.fill({});
    runChestSettled.fill({});
    runChestUniqueIds.fill({});
    runChestUniqueCounts.fill(0);
    runError.clear();
    runGame.store(game, std::memory_order_release);
    return {{"ok", true}, {"status", "armed"}, {"request_id", request.id},
        {"source", "offline_server"},
        {"source_level", request.townEntry ? request.triggerSource : 20},
        {"root_level", runRootLevel}, {"run_serial", runActiveSerial},
        {"initial_portals_removed", std::move(retired)}};
}
bool ReservePortalNonce(const std::string& id, const std::string& fingerprint, json receipt);
void RememberOwned(const std::shared_ptr<Pending>& pending);
bool CaptureRunCharacterName(UnitAny* player, std::string& character) {
    if (!player || !player->pPlayerData ||
        !Readable(player->pPlayerData, 0xA0)) return false;
    const char* name = player->pPlayerData->szName;
    const size_t length = strnlen_s(name, sizeof(player->pPlayerData->szName));
    if (!length || length >= sizeof(player->pPlayerData->szName)) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    character.assign(name, length);
    return true;
}
bool RunSnapshotPath(const std::string& character, std::string& path) {
    if (character.empty()) return false;
    char executable[MAX_PATH] = {};
    const DWORD size = GetModuleFileNameA(nullptr, executable, MAX_PATH);
    if (!size || size >= MAX_PATH) return false;
    std::string directory(executable, size);
    const size_t slash = directory.find_last_of("\\/");
    if (slash == std::string::npos) return false;
    directory.resize(slash + 1);
    path = directory + "roguelike-route-" + character + ".json";
    return path.size() < MAX_PATH;
}
bool RunSnapshotPath(UnitAny* player, std::string& path) {
    std::string character;
    return CaptureRunCharacterName(player, character) &&
        RunSnapshotPath(character, path);
}
bool DeleteRunSnapshot(UnitAny* player, std::string& error) {
    std::string path;
    if (!RunSnapshotPath(player, path)) {
        error = "cannot derive a safe character route-snapshot path";
        return false;
    }
    if (!DeleteFileA(path.c_str())) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
            error = "cannot remove saved route snapshot";
            return false;
        }
    }
    error.clear();
    return true;
}
bool SnapshotInteger(const json& object, const char* key, int minimum,
    int maximum, int& value) {
    if (!object.is_object() || !object.contains(key)) return false;
    const json& field = object.at(key);
    if (!field.is_number_integer() && !field.is_number_unsigned()) return false;
    try {
        const int64_t parsed = field.get<int64_t>();
        if (parsed < minimum || parsed > maximum) return false;
        value = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}
json AffixSnapshot(const RoguelikeAffixes::Roll& roll) {
    if (roll.count > roll.modifiers.size() ||
        roll.statCount > roll.stats.size()) return nullptr;
    json modifiers = json::array();
    json stats = json::array();
    for (unsigned i = 0; i < roll.count; ++i)
        modifiers.push_back(roll.modifiers[i].id);
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        stats.push_back({{"map_stat", stat.mapStat}, {"layer", stat.layer},
            {"category", stat.category}, {"value", stat.value}});
    }
    return {{"quality", static_cast<int>(roll.quality)},
        {"modifiers", std::move(modifiers)}, {"stats", std::move(stats)},
        {"area_level", roll.areaLevel}, {"density_percent", roll.densityPercent},
        {"magic_find", roll.magicFind},
        {"monster_health_percent", roll.monsterHealthPercent}};
}
bool ReadAffixSnapshot(const json& value, RoguelikeAffixes::Roll& roll) {
    int quality, areaLevel, density, magicFind, health;
    if (!value.is_object() ||
        !SnapshotInteger(value, "quality", 0, 2, quality) ||
        !SnapshotInteger(value, "area_level", 0, 99, areaLevel) ||
        !SnapshotInteger(value, "density_percent", -100, 5000, density) ||
        !SnapshotInteger(value, "magic_find", 0, 10000, magicFind) ||
        !SnapshotInteger(value, "monster_health_percent", -100, 10000, health) ||
        !value.contains("modifiers") || !value["modifiers"].is_array() ||
        value["modifiers"].size() > roll.modifiers.size() ||
        !value.contains("stats") || !value["stats"].is_array() ||
        value["stats"].size() > roll.stats.size()) return false;
    roll.quality = static_cast<RoguelikeAffixes::Quality>(quality);
    roll.count = static_cast<unsigned>(value["modifiers"].size());
    for (unsigned i = 0; i < roll.count; ++i) {
        int id;
        if (!SnapshotInteger(
                json{{"value", value["modifiers"][i]}}, "value", 0, 65535, id))
            return false;
        roll.modifiers[i].id = static_cast<unsigned>(id);
    }
    roll.statCount = static_cast<unsigned>(value["stats"].size());
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const json& stat = value["stats"][i];
        int mapStat, layer, category, statValue;
        if (!SnapshotInteger(stat, "map_stat", 0, 65535, mapStat) ||
            !SnapshotInteger(stat, "layer", 0, 65535, layer) ||
            !SnapshotInteger(stat, "category", 0, 65535, category) ||
            !SnapshotInteger(stat, "value", INT_MIN, INT_MAX, statValue))
            return false;
        roll.stats[i] = {static_cast<unsigned>(mapStat),
            static_cast<unsigned>(layer), static_cast<unsigned>(category),
            statValue};
    }
    roll.areaLevel = areaLevel;
    roll.densityPercent = density;
    roll.magicFind = magicFind;
    roll.monsterHealthPercent = health;
    return true;
}
json ZoneSnapshot(const Pending::Zone& zone, bool cleared,
    const std::array<bool, 3>& chestSettled,
    const std::array<int, 12>& chestUniqueIds,
    unsigned chestUniqueCount) {
    json chests = json::array();
    for (bool settled : chestSettled) chests.push_back(settled);
    json uniqueIds = json::array();
    for (unsigned i = 0; i < chestUniqueCount; ++i)
        uniqueIds.push_back(chestUniqueIds[i]);
    json monsterClasses = json::array();
    for (unsigned i = 0; i < zone.monsterClassCount; ++i)
        monsterClasses.push_back(zone.monsterClasses[i]);
    return {{"level", zone.level}, {"class_id", zone.classId},
        {"monster_level", zone.monsterLevel}, {"density", zone.density},
        {"density_baseline", zone.densityBaseline},
        {"magic_find", zone.magicFind}, {"depth", zone.depth},
        {"kind", static_cast<int>(zone.kind)},
        {"reward", static_cast<int>(zone.reward)},
        {"other_shrine", zone.otherShrine},
        {"second_other_shrine", zone.secondOtherShrine},
        {"boss_superunique_id", zone.bossSuperuniqueId},
        {"monster_classes", std::move(monsterClasses)},
        {"test_only", zone.testOnly}, {"cleared", cleared},
        {"chest_settled", std::move(chests)},
        {"chest_unique_ids", std::move(uniqueIds)},
        {"affixes", AffixSnapshot(zone.affixes)}};
}
bool ReadZoneSnapshot(const json& value, int expectedLevel,
    Pending::Zone& zone, bool& cleared,
    std::array<bool, 3>& chestSettled,
    std::array<int, 12>& chestUniqueIds, unsigned& chestUniqueCount) {
    int level, classId, monsterLevel, density, densityBaseline, magicFind, depth;
    int kind, reward, otherShrine, secondOtherShrine, bossId;
    if (!value.is_object() ||
        !SnapshotInteger(value, "level", expectedLevel, expectedLevel, level) ||
        !SnapshotInteger(value, "class_id", 0, 32767, classId) ||
        !SnapshotInteger(value, "monster_level", 1, 99, monsterLevel) ||
        !SnapshotInteger(value, "density", 0, 65535, density) ||
        !SnapshotInteger(value, "magic_find", 0, 10000, magicFind) ||
        !SnapshotInteger(value, "depth", 1, 99, depth) ||
        !SnapshotInteger(value, "kind", 0, 4, kind) ||
        !SnapshotInteger(value, "reward", 0, 5, reward) ||
        !SnapshotInteger(value, "other_shrine", 0, 255, otherShrine) ||
        !SnapshotInteger(value, "second_other_shrine", 0, 255,
            secondOtherShrine) ||
        !SnapshotInteger(value, "boss_superunique_id", -1, 32767, bossId) ||
        !value.contains("cleared") || !value["cleared"].is_boolean() ||
        !value.contains("test_only") || !value["test_only"].is_boolean() ||
        !value.contains("chest_settled") ||
        !value["chest_settled"].is_array() ||
        value["chest_settled"].size() != chestSettled.size() ||
        !value.contains("affixes")) return false;
    if (value.contains("density_baseline")) {
        if (!SnapshotInteger(value, "density_baseline", 0, 100000,
                densityBaseline)) return false;
    } else {
        densityBaseline =
            kind == static_cast<int>(Pending::ZoneKind::Monster) &&
            !value["test_only"].get<bool>() ? 1600 : density;
    }
    for (unsigned i = 0; i < chestSettled.size(); ++i) {
        if (!value["chest_settled"][i].is_boolean()) return false;
        chestSettled[i] = value["chest_settled"][i].get<bool>();
    }
    chestUniqueIds.fill(0);
    chestUniqueCount = 0;
    if (value.contains("chest_unique_ids")) {
        const json& savedIds = value["chest_unique_ids"];
        if (!savedIds.is_array() || savedIds.size() > chestUniqueIds.size())
            return false;
        for (const json& savedId : savedIds) {
            int uniqueId;
            if (!SnapshotInteger(json{{"value", savedId}},
                    "value", 0, 32767, uniqueId)) return false;
            chestUniqueIds[chestUniqueCount++] = uniqueId;
        }
    }
    zone.level = level;
    zone.classId = classId;
    zone.monsterLevel = monsterLevel;
    zone.density = density;
    zone.densityBaseline = densityBaseline;
    zone.magicFind = magicFind;
    zone.depth = depth;
    zone.kind = static_cast<Pending::ZoneKind>(kind);
    zone.reward = static_cast<Pending::RewardFocus>(reward);
    zone.monsterClasses.fill(0);
    zone.monsterClassCount = 0;
    if (zone.kind == Pending::ZoneKind::Monster) {
        if (value.contains("monster_classes")) {
            const json& classes = value["monster_classes"];
            if (!classes.is_array() || classes.empty() ||
                classes.size() > zone.monsterClasses.size()) return false;
            for (const json& savedClass : classes) {
                int monsterClass;
                if (!SnapshotInteger(json{{"value", savedClass}},
                        "value", 0, 32767, monsterClass)) return false;
                if ((!zone.monsterClassCount && monsterClass != classId) ||
                    std::find(zone.monsterClasses.begin(),
                        zone.monsterClasses.begin() + zone.monsterClassCount,
                        monsterClass) != zone.monsterClasses.begin() +
                            zone.monsterClassCount) return false;
                zone.monsterClasses[zone.monsterClassCount++] = monsterClass;
            }
        } else {
            zone.monsterClasses[0] = classId;
            zone.monsterClassCount = 1;
        }
    } else if (value.contains("monster_classes") &&
        (!value["monster_classes"].is_array() ||
         !value["monster_classes"].empty())) return false;
    zone.otherShrine = static_cast<BYTE>(otherShrine);
    zone.secondOtherShrine = static_cast<BYTE>(secondOtherShrine);
    zone.bossSuperuniqueId = bossId;
    zone.testOnly = value["test_only"].get<bool>();
    cleared = value["cleared"].get<bool>();
    zone.resumeCleared = cleared;
    if (!ReadAffixSnapshot(value["affixes"], zone.affixes)) return false;
    if (zone.kind == Pending::ZoneKind::Monster &&
        !ValidOrdinaryDensity(zone)) return false;
    return true;
}
bool IsRunArenaLevel(int level) {
    return (level >= 13 && level <= 16) || (level >= 138 && level <= 254);
}
bool FreshRunArenaAvailable(UnitAny* player, int level) {
    return IsRunArenaLevel(level) && !runUsedAreas[level] &&
        !zoneSlots[level].configured && !runRetiredDestinations[level] &&
        TargetUnused(player, level);
}
bool ParseRunSnapshot(const json& snapshot, const std::string& character,
    RunResumeState& resume, std::string& error) {
    int version, active;
    if (!snapshot.is_object() ||
        !SnapshotInteger(snapshot, "version", 1, 2, version) ||
        !snapshot.contains("character") ||
        !snapshot["character"].is_string() ||
        snapshot["character"].get<std::string>() != character ||
        !SnapshotInteger(snapshot, "active_level", 1, 254, active) ||
        !snapshot.contains("edges") || !snapshot["edges"].is_array() ||
        !snapshot.contains("root_cleared") ||
        !snapshot["root_cleared"].is_boolean() ||
        !snapshot.contains("starter_settled") ||
        !snapshot["starter_settled"].is_boolean()) {
        error = "saved route header is malformed or belongs to another character";
        return false;
    }
    std::map<int, int> next;
    std::array<bool, 255> destinationSeen{};
    bool rootCleared = snapshot["root_cleared"].get<bool>();
    bool starterSettled = snapshot["starter_settled"].get<bool>();
    for (const json& edge : snapshot["edges"]) {
        int from, to;
        if (!SnapshotInteger(edge, "from", 1, 254, from) ||
            !SnapshotInteger(edge, "to", 1, 254, to) ||
            (from != 1 && from != 20 && !IsRunArenaLevel(from)) ||
            (to != 20 && !IsRunArenaLevel(to)) ||
            (version == 1 && from == 1 && to != 20) || from == to ||
            next.count(from) || destinationSeen[to]) {
            error = "saved route edges are invalid or branched";
            return false;
        }
        if (from == 1) {
            if (to == 20) {
                if (!edge.contains("cleared") ||
                    !edge["cleared"].is_boolean()) {
                    error = "saved town-to-Tower edge is malformed";
                    return false;
                }
                rootCleared = edge["cleared"].get<bool>();
                resume.rootLevel = 20;
            } else {
                Pending::Zone zone{};
                bool cleared = false;
                std::array<bool, 3> chests{};
                std::array<int, 12> uniqueIds{};
                unsigned uniqueCount = 0;
                if (!edge.contains("zone") ||
                    !ReadZoneSnapshot(edge["zone"], to, zone, cleared,
                        chests, uniqueIds, uniqueCount) ||
                    zone.kind != Pending::ZoneKind::Monster ||
                    zone.depth != 1 || zone.monsterLevel != 1 ||
                    zone.testOnly || cleared != rootCleared) {
                    error = "saved dynamic run root is invalid";
                    return false;
                }
                resume.rootLevel = to;
                resume.zones.emplace(to, zone);
                resume.cleared.emplace(to, cleared);
                resume.chestSettled.emplace(to, chests);
                resume.chestUniqueIds.emplace(to, uniqueIds);
                resume.chestUniqueCounts.emplace(to, uniqueCount);
            }
        } else {
            if (!edge.contains("zone")) {
                error = "saved arena route has no zone roll";
                return false;
            }
            Pending::Zone zone{};
            bool cleared = false;
            std::array<bool, 3> chests{};
            std::array<int, 12> uniqueIds{};
            unsigned uniqueCount = 0;
            if (!ReadZoneSnapshot(
                    edge["zone"], to, zone, cleared, chests,
                    uniqueIds, uniqueCount)) {
                error = "saved zone roll is invalid";
                return false;
            }
            resume.zones.emplace(to, zone);
            resume.cleared.emplace(to, cleared);
            resume.chestSettled.emplace(to, chests);
            resume.chestUniqueIds.emplace(to, uniqueIds);
            resume.chestUniqueCounts.emplace(to, uniqueCount);
        }
        next.emplace(from, to);
        destinationSeen[to] = true;
    }
    const auto root = next.find(1);
    if (root == next.end() || root->second != resume.rootLevel) {
        error = "saved route has no authenticated run entrance";
        return false;
    }
    resume.path = {resume.rootLevel};
    std::array<bool, 255> visited{};
    visited[resume.rootLevel] = true;
    int current = resume.rootLevel;
    while (next.count(current)) {
        const int child = next.at(current);
        if (!IsRunArenaLevel(child) || visited[child] ||
            resume.path.size() >= 99) {
            error = "saved route contains a cycle or invalid destination";
            return false;
        }
        visited[child] = true;
        resume.path.push_back(child);
        current = child;
    }
    const auto activePosition = std::find(
        resume.path.begin(), resume.path.end(), active);
    if (next.size() != resume.path.size() ||
        (active != 1 && activePosition == resume.path.end())) {
        error = "saved route is disconnected from its active level";
        return false;
    }
    if (active == 1) {
        // Legacy paired-route snapshots could be saved in town after returning.
        resume.desiredLevel = resume.rootLevel;
        resume.path.resize(1);
    } else {
        resume.desiredLevel = active;
        resume.path.resize(static_cast<size_t>(
            activePosition - resume.path.begin()) + 1);
    }
    resume.rootCleared = rootCleared;
    resume.starterSettled = starterSettled;
    error.clear();
    return true;
}
bool LoadRunSnapshot(UnitAny* player, json& snapshot, bool& found,
    std::string& path, std::string& error) {
    found = false;
    if (!RunSnapshotPath(player, path)) {
        error = "cannot derive a safe character route-snapshot path";
        return false;
    }
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            error.clear();
            return true;
        }
        error = "cannot open saved route snapshot";
        return false;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 2 ||
        length.QuadPart > 512 * 1024) {
        CloseHandle(file);
        error = "saved route snapshot size is invalid";
        return false;
    }
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
    DWORD received = 0;
    const bool read = ReadFile(file, bytes.data(),
        static_cast<DWORD>(bytes.size()), &received, nullptr) &&
        received == bytes.size();
    CloseHandle(file);
    if (!read) {
        error = "saved route snapshot read was incomplete";
        return false;
    }
    snapshot = json::parse(bytes, nullptr, false);
    RunResumeState parsed;
    const std::string character(player->pPlayerData->szName);
    if (snapshot.is_discarded() ||
        !ParseRunSnapshot(snapshot, character, parsed, error)) return false;
    found = true;
    error.clear();
    return true;
}
bool SaveRunSnapshot(void* game, UnitAny* player, std::string& error) {
    if (runResume.active) return true; // Preserve the prior complete route during reconstruction.
    if (!game || runGame.load(std::memory_order_acquire) != game ||
        player != runPlayer || runEpoch.empty()) return true;
    std::string path;
    if (runCharacterName.empty() ||
        !RunSnapshotPath(runCharacterName, path)) {
        error = "cannot derive a safe character route-snapshot path";
        return false;
    }
    json edges = json::array();
    for (int from = 1; from < 255; ++from) {
        const int to = runRouteNext[from];
        if (to < 0) continue;
        if (from == 1 && to == 20) {
            edges.push_back({{"from", from}, {"to", to},
                {"cleared", runCleared[20]}});
            continue;
        }
        if ((from != 1 && from != 20 && !IsRunArenaLevel(from)) ||
            (from == 1 &&
                (to != runRootLevel || !IsRunArenaLevel(to))) ||
            !IsRunArenaLevel(to) || !zoneSlots[to].configured ||
            runRouteParent[to] != from) {
            error = "selected route graph is incomplete at save time";
            return false;
        }
        const ZoneSlot& slot = zoneSlots[to];
        Pending::Zone zone{to, slot.selectedClassId,
            slot.configuredRow.wMonLvl[0], slot.configuredRow.dwMonDen[0],
            slot.magicFind};
        zone.monsterClasses = slot.selectedMonsterClasses;
        zone.monsterClassCount = slot.selectedMonsterClassCount;
        zone.depth = slot.depth;
        zone.densityBaseline = slot.densityBaseline;
        zone.affixes = slot.affixes;
        zone.kind = slot.kind;
        zone.otherShrine = slot.otherShrine;
        zone.reward = slot.reward;
        zone.bossSuperuniqueId = slot.bossSuperuniqueId;
        zone.secondOtherShrine = slot.secondOtherShrine;
        zone.testOnly = slot.testOnly;
        if (slot.selectedMonsterClassCount > slot.selectedMonsterClasses.size() ||
            (slot.kind == Pending::ZoneKind::Monster &&
                (!slot.selectedMonsterClassCount ||
                 slot.selectedMonsterClasses[0] != slot.selectedClassId)) ||
            (slot.kind != Pending::ZoneKind::Monster &&
                slot.selectedMonsterClassCount)) {
            error = "route monster roster cannot be serialized safely";
            return false;
        }
        if (runChestUniqueCounts[to] > runChestUniqueIds[to].size()) {
            error = "chest unique exclusion history exceeds snapshot capacity";
            return false;
        }
        json zoneData = ZoneSnapshot(zone, runCleared[to],
            runChestSettled[to], runChestUniqueIds[to],
            runChestUniqueCounts[to]);
        if (zoneData["affixes"].is_null()) {
            error = "zone affix roll cannot be serialized";
            return false;
        }
        edges.push_back({{"from", from}, {"to", to},
            {"zone", std::move(zoneData)}});
    }
    if ((runRouteNext[1] != 20 &&
            !IsRunArenaLevel(runRouteNext[1])) ||
        runRouteNext[1] != runRootLevel || edges.empty()) {
        error = "active run has no selected entrance route to save";
        return false;
    }
    const int active = runStage.load(std::memory_order_acquire);
    if (active < 1 || active >= 255) {
        error = "active run stage is outside the saved route";
        return false;
    }
    const std::string& character = runCharacterName;
    json snapshot = {{"version", 2}, {"character", character},
        {"active_level", active}, {"root_cleared", runCleared[runRootLevel]},
        {"starter_settled", runStarterSettled.load(std::memory_order_acquire)},
        {"edges", std::move(edges)}};
    const std::string bytes = snapshot.dump();
    const std::string temporary = path + ".tmp";
    if (temporary.size() >= MAX_PATH) {
        error = "route snapshot temporary path is too long";
        return false;
    }
    HANDLE file = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot create route snapshot";
        return false;
    }
    DWORD written = 0;
    const bool persisted = WriteFile(file, bytes.data(),
        static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!persisted || !MoveFileExA(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary.c_str());
        error = "atomic route snapshot replacement failed";
        return false;
    }
    error.clear();
    return true;
}
int FindFreshRunRoot(UnitAny* player, std::string& error) {
    TargetStatus tower{};
    const bool towerUnused = TargetUnused(player, 20, &tower);
    const bool towerFresh = towerUnused && !runUsedAreas[20];
    if (!tower.sourceFound || !tower.listVerified) {
        error = "server level list is not verified";
        return 0;
    }
    std::array<bool, 255> fresh{};
    for (int level = 13; level <= 254; ++level) {
        if (level == 17) level = 138;
        fresh[level] = FreshRunArenaAvailable(player, level);
    }
    const int root = RoguelikeProgression::SelectFreshRunRoot(towerFresh, fresh);
    if (!root) error = "no unused run root remains";
    else error.clear();
    return root;
}
bool BuildFreshRunRootZone(int target, unsigned seed,
    Pending::Zone& zone, std::string& error) {
    std::minstd_rand random(seed ? seed : 1);
    RoguelikeProgression::EncounterSelection selection{};
    const std::array<std::uint16_t, 2> noExcluded{};
    int densityBaseline = 0;
    if (!ServerSpawns::SelectVanillaEncounterPool(
            1, random, noExcluded, 0, selection, densityBaseline, error))
        return false;
    zone = {};
    zone.level = target;
    zone.classId = selection.classes[0];
    zone.monsterClassCount = selection.count;
    for (unsigned index = 0; index < selection.count; ++index)
        zone.monsterClasses[index] = selection.classes[index];
    zone.monsterLevel = 1;
    zone.density = RoguelikeAffixes::NativeDensity(densityBaseline, 0);
    zone.depth = 1;
    zone.densityBaseline = densityBaseline;
    zone.kind = Pending::ZoneKind::Monster;
    zone.reward = Pending::RewardFocus::Equipment;
    error.clear();
    return true;
}
json StartTownRun(void* game, UnitAny* player) {
    if (!PortalArea(player, 1) || runGame.load(std::memory_order_acquire))
        return Failure("start a fresh run in Rogue Encampment");
    std::string error;
    if (!CharacterState::RestoreLevelOneLife(game, player, error))
        return Failure("fresh run life restoration failed: " + error);
    ServerMonsters::Request observe;
    observe.operation = ServerMonsters::Operation::Observe;
    const json state = ObservationTelemetry::Capture(game, player, observe);
    if (!state.value("ok", false) || !state.value("coherent", false) ||
        !state.contains("session_epoch") || !state["session_epoch"].is_string())
        return Failure("game epoch unavailable for run entrance");
    const std::string epoch = state["session_epoch"].get<std::string>();
    if (!ObservationTelemetry::MatchesContext(epoch, game, player))
        return Failure("game context changed before run entrance");
    if (runAreaUsageGame != game || runAreaUsageEpoch != epoch) {
        runAreaUsageGame = game;
        runAreaUsageEpoch = epoch;
        runUsedAreas.fill(false);
    }
    const json portalCleanup = RemoveRunOwnedObjects(game, player);
    if (!portalCleanup.value("ok", false))
        return Failure("previous run portal cleanup uncertain: " + portalCleanup.dump());
    if (!ResetZoneRows(error))
        return Failure("prior run zone restoration failed: " + error);
    error.clear();
    const std::uint64_t entrySerial = ++runEntrySerial;
    const std::string id = "native-entry:" + epoch + ":" +
        std::to_string(entrySerial);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (ownedJobs.count(id)) return Failure("run entrance was already opened in this game");
    }
    std::string snapshotPath;
    json savedRoute;
    bool resumeAvailable = false;
    if (!LoadRunSnapshot(player, savedRoute, resumeAvailable,
            snapshotPath, error))
        return Failure("saved route snapshot unavailable: " + error);
    int rootLevel = 0;
    Pending::Zone rootZone{};
    if (resumeAvailable) {
        RunResumeState saved{};
        const std::string character(player->pPlayerData->szName);
        if (!ParseRunSnapshot(savedRoute, character, saved, error))
            return Failure("saved route root could not be parsed: " + error);
        rootLevel = saved.rootLevel;
        if (rootLevel != 20) {
            const auto root = saved.zones.find(rootLevel);
            if (root == saved.zones.end())
                return Failure("saved route has no dynamic root zone");
            rootZone = root->second;
        }
    } else {
        rootLevel = FindFreshRunRoot(player, error);
        if (!rootLevel)
            return Failure("fresh run destination unavailable: " + error);
        if (rootLevel != 20 &&
            !BuildFreshRunRootZone(rootLevel,
                static_cast<unsigned>(entrySerial), rootZone, error))
            return Failure("fresh run root encounter unavailable: " + error);
    }
    if (rootLevel != 20) {
        Pending configure{};
        configure.expectedEpoch = epoch;
        configure.playerId = player->dwUnitId;
        configure.zoneJob = rootZone;
        const json configured = ConfigureZone(game, player, configure);
        if (!configured.value("ok", false)) {
            error = configured.value("error",
                std::string("configuration failed"));
            std::string restoreError;
            if (!ResetZoneRows(restoreError))
                error += "; zone restoration uncertain: " + restoreError;
            return Failure("run root configuration failed: " + error);
        }
    }
    if (!InstallChoiceNames(error)) return Failure("run labels unavailable: " + error);
    auto entry = std::make_shared<Pending>();
    entry->id = id;
    entry->fingerprint = "native-entry";
    entry->expectedEpoch = epoch;
    entry->playerId = player->dwUnitId;
    entry->triggerJob = rootLevel;
    entry->triggerSource = 1;
    entry->townEntry = true;
    entry->runSerial = entrySerial;
    entry->resumeRun = resumeAvailable;
    if (resumeAvailable) entry->resumeSnapshot = std::move(savedRoute);
    entry->zoneJob = rootLevel == 20 ?
        Pending::Zone{20, 5, 1, 2400, 0, 1} : rootZone;
    if (!ReservePortalNonce(id, entry->fingerprint,
            {{"ok", false}, {"status", "unconfirmed"}, {"request_id", id}}))
        return Failure("run entrance receipt capacity reached");
    RememberOwned(entry);
    json result = CreateTrigger(game, player, *entry);
    result["resume_available"] = resumeAvailable;
    result["previous_portal_cleanup"] = portalCleanup;
    result["run_serial"] = entrySerial;
    result["root_level"] = rootLevel;
    if (resumeAvailable)
        result["resume_active_level"] =
            entry->resumeSnapshot->value("active_level", 20);
    {
        std::lock_guard<std::mutex> lock(mutex);
        portalReceipts.at(id).second = result;
        entry->completed = true;
    }
    return result;
}
json BeginRunResume(void* game, UnitAny* player, const Pending& entry,
    DWORD townEntryId, UnitAny* townEntry) {
    if (!entry.resumeSnapshot || !player ||
        runGame.load(std::memory_order_acquire) != game ||
        runPlayer != player || !PortalArea(player, 1))
        return Failure("saved route resume context changed");
    std::string snapshotPath;
    if (!RunSnapshotPath(player, snapshotPath))
        return Failure("saved route path cannot be derived");
    RunResumeState resume{};
    std::string error;
    if (!ParseRunSnapshot(*entry.resumeSnapshot,
            player->pPlayerData->szName, resume, error))
        return Failure("saved route is invalid: " + error);
    const int rootLevel = resume.rootLevel;
    if (rootLevel != entry.triggerJob.value_or(20) ||
        (rootLevel != 20 && !zoneSlots[rootLevel].configured)) {
        CancelTownArm();
        return Failure("saved run root was not prepared by the town entrance");
    }
    const char* cleanupReason = nullptr;
    if (!RetireSelectedTownEntry(game, player, townEntryId, townEntry,
            &cleanupReason)) {
        CancelTownArm();
        json failure = Failure("selected town entry could not be retired");
        failure["cleanup_reason"] = cleanupReason ? cleanupReason :
            "object_removal_failed";
        return failure;
    }
    resume.active = true;
    resume.snapshotPath = snapshotPath;
    runResume = std::move(resume);
    runRootLevel = rootLevel;
    runCleared[rootLevel] = runResume.rootCleared;
    runStarterSettled.store(rootLevel == 20 && runResume.starterSettled,
        std::memory_order_release);
    runPopulationReady[rootLevel] = runResume.rootCleared;
    runEncounters[rootLevel] = runResume.rootCleared ? 2 : 0;
    runStage.store(1, std::memory_order_release);
    runArrivalLevel = rootLevel;
    const json warp = WarpToFreshLevel(game, player, rootLevel);
    const bool accepted = warp.value("ok", false) ||
        PortalArea(player, rootLevel);
    runArrivalLevel = 0;
    if (!accepted) {
        runResume = {};
        CancelTownArm();
        return {{"ok", false}, {"status", "transition_rejected"},
            {"destination_level", rootLevel}, {"warp", warp}};
    }
    for (const int area : runResume.path)
        if (area > 0 && area < 255) runUsedAreas[area] = true;
    runRouteNext[1] = rootLevel;
    runRouteParent[rootLevel] = 1;
    runBossKillWitness.store(false, std::memory_order_release);
    runBossDamageCount = 0;
    runDamageNativeLevel = 0;
    runBossProjectileHits = 0;
    runBossProjectileOriginal = runBossProjectileApplied = 0;
    runStage.store(rootLevel, std::memory_order_release);
    if (PortalArea(player, rootLevel)) {
        if (rootLevel == 20) CompleteStarterEncounter(game, player);
        else CompleteArenaEncounter(game, player, rootLevel);
    }
    ContinueRunResume(game, player);
    return {{"ok", true}, {"status", "resume_transition_requested"},
        {"destination_level", rootLevel}, {"root_level", rootLevel},
        {"warp", warp}};
}
void FinishRunResume(void* game, UnitAny* player, int level) {
    if (player != runPlayer ||
        runGame.load(std::memory_order_acquire) != game) return;
    const std::string snapshotPath = runResume.snapshotPath;
    runResume.active = false;
    if (!snapshotPath.empty() && !DeleteFileA(snapshotPath.c_str()) &&
        GetLastError() != ERROR_FILE_NOT_FOUND) {
        runError = "rebuilt route but could not remove the consumed snapshot";
        OutputDebugStringA("Roguelike resume snapshot cleanup failed\n");
    }
    runResume = {};
    if (!runError.empty()) return;
    if (level >= 13 && level <= 254 && runCleared[level] &&
        runRouteNext[level] < 0 &&
        (level == 20 || zoneSlots[level].kind == Pending::ZoneKind::Monster ||
         BossZone(zoneSlots[level].kind)))
        ServerSpawns::OfferChoices(game, level);
}
void ContinueRunResume(void* game, UnitAny* player) {
    // Population and frame callbacks may reenter while this transition is built.
    std::unique_lock<std::mutex> resumeLock(
        runResumeContinuationMutex, std::try_to_lock);
    if (!resumeLock.owns_lock()) return;
    if (!runResume.active || runGame.load(std::memory_order_acquire) != game ||
        runPlayer != player || !runError.empty()) return;
    for (;;) {
        const int current = runStage.load(std::memory_order_acquire);
        if (!PortalArea(player, current) ||
            runResume.currentIndex >= runResume.path.size() ||
            runResume.path[runResume.currentIndex] != current) return;
        if (current == runResume.desiredLevel) {
            FinishRunResume(game, player, current);
            return;
        }
        if (runResume.currentIndex + 1 >= runResume.path.size()) {
            runError = "saved active level is outside the one-way route prefix";
            return;
        }
        const int destination = runResume.path[runResume.currentIndex + 1];
        if ((runRouteNext[current] >= 0 &&
                runRouteNext[current] != destination) ||
            (runRouteParent[destination] >= 0 &&
                runRouteParent[destination] != current)) {
            runError = "saved one-way route conflicts with the active route graph";
            return;
        }
        const auto zoneFound = runResume.zones.find(destination);
        const auto clearedFound = runResume.cleared.find(destination);
        const auto chestFound = runResume.chestSettled.find(destination);
        const auto uniqueIdsFound = runResume.chestUniqueIds.find(destination);
        const auto uniqueCountFound =
            runResume.chestUniqueCounts.find(destination);
        if (zoneFound == runResume.zones.end() ||
            clearedFound == runResume.cleared.end() ||
            chestFound == runResume.chestSettled.end() ||
            uniqueIdsFound == runResume.chestUniqueIds.end() ||
            uniqueCountFound == runResume.chestUniqueCounts.end() ||
            uniqueCountFound->second > uniqueIdsFound->second.size()) {
            runError = "saved route child has incomplete state";
            return;
        }
        Pending::Zone zone = zoneFound->second;
        const bool cleared = clearedFound->second;
        const bool combat = zone.kind == Pending::ZoneKind::Monster ||
            BossZone(zone.kind);
        zone.resumeCleared = combat && cleared;
        Pending configure;
        configure.expectedEpoch = runEpoch;
        configure.playerId = player->dwUnitId;
        configure.zoneJob = zone;
        const json configured = ConfigureZone(game, player, configure);
        if (!configured.value("ok", false)) {
            runError = "saved route zone could not be reconstructed: " +
                configured.value("error", std::string("configuration failed"));
            return;
        }
        runCleared[destination] = cleared;
        runPopulationReady[destination] = combat && cleared;
        runEncounters[destination] = combat && cleared ? 2 : 0;
        runChestSettled[destination] = zone.kind == Pending::ZoneKind::Chest ?
            chestFound->second : std::array<bool, 3>{};
        runChestUniqueIds[destination] = uniqueIdsFound->second;
        runChestUniqueCounts[destination] = uniqueCountFound->second;
        std::string error;
        if (!RetireZonePlayer(game, player, current, error)) {
            runError = "saved route source could not be retired: " + error;
            return;
        }
        runArrivalLevel = destination;
        const json warp = WarpToFreshLevel(game, player, destination);
        const bool accepted = warp.value("ok", false) ||
            PortalArea(player, destination);
        runArrivalLevel = 0;
        if (!accepted) {
            runError = "saved route transition was rejected: " +
                warp.value("error", std::string("native transfer failed"));
            return;
        }
        runRouteNext[current] = destination;
        runRouteParent[destination] = current;
        ++runResume.currentIndex;
        runBossKillWitness.store(false, std::memory_order_release);
        runStage.store(destination, std::memory_order_release);
        runBossDamageCount = 0;
        runDamageNativeLevel = 0;
        runBossProjectileHits = 0;
        runBossProjectileOriginal = runBossProjectileApplied = 0;
        if (!PortalArea(player, destination)) return;
        if (destination == 20) {
            CompleteStarterEncounter(game, player);
        } else if (zoneSlots[destination].configured) {
            runPopulationReady[destination] = true;
            CompleteArenaEncounter(game, player, destination);
        }
        if (!runError.empty()) return;
    }
}
void ReportStart(void* game, UnitAny* player, const json& result) {
    std::string text = !result.value("ok", false) ?
        "Run start failed: " + result.value("error", std::string("entrance unavailable")) :
        result.value("resume_available", false) ?
        "Saved run ready. Click the Resume Run portal." :
        "Roguelike entrance opened. Click the Begin descent portal.";
    if (text.size() > 120) text.resize(120);
    BYTE message[128] = {0x15, 1, 0};
    memcpy(message + 3, text.data(), text.size());
    originalChat(game, player, message, static_cast<int>(text.size() + 4));
}
void SettleChestRewards(void* game, UnitAny* player, int level) {
    if (zoneSlots[level].kind != Pending::ZoneKind::Chest ||
        !runCleared[level] || !runError.empty() ||
        !ObservationTelemetry::MatchesContext(runEpoch, game, player))
        return;
    for (unsigned index = 0; index < 3; ++index) {
        const DWORD id = runChestIds[level][index];
        if (!id || runChestSettled[level][index]) continue;
        UnitAny* chest = FindServerObject(game, id, 397);
        if (!chest || chest->dwTxtFileNo != 397 ||
            !Readable(chest->pObjectData, sizeof(ObjectData)) ||
            (chest->dwMode != OBJ_MODE_IDLE &&
             chest->dwMode != OBJ_MODE_OPERATING &&
             chest->dwMode != OBJ_MODE_OPENED)) {
            runError = "owned locked chest identity or state changed";
            return;
        }
        if (chest->dwMode != OBJ_MODE_OPENED) {
            // Initialization clears the constructor's lock; restore it only
            // while idle, never during the stock operating/key-debit phase.
            if (chest->dwMode == OBJ_MODE_IDLE)
                chest->pObjectData->ChestLocked = 1;
            continue;
        }
        // Stock OperateFn=4 has already checked and debited the key (unless
        // the player is an Assassin) before changing the object mode.
        runChestSettled[level][index] = true; // Never replay an uncertain drop.
        const DWORD entropy = GetTickCount() ^ id ^ (level * 2654435761u);
        const unsigned rewardCount = RoguelikeLoot::ChestRewardCount(entropy);
        const int itemLevel = RoguelikeLoot::ChestUniqueItemLevel(
            RoguelikeLoot::RewardItemLevel(
                zoneSlots[level].configuredRow.wMonLvl[0],
                zoneSlots[level].depth, zoneSlots[level].testOnly));
        const int playerLevel = MarketCharacterLevel();
        if (playerLevel < 1 || playerLevel > 99) {
            runError = "chest unique reward has no valid character level";
            return;
        }
        if (runChestUniqueCounts[level] > runChestUniqueIds[level].size()) {
            runError = "chest unique exclusion history is invalid";
            return;
        }
        unsigned& uniqueCount = runChestUniqueCounts[level];
        auto& uniqueIds = runChestUniqueIds[level];
        if (uniqueCount + rewardCount > uniqueIds.size()) {
            runError = "chest unique exclusion history is full";
            return;
        }
        for (unsigned rewardIndex = 0; rewardIndex < rewardCount; ++rewardIndex) {
            const DWORD rewardSeed = entropy ^
                (0x9e3779b9u * (rewardIndex + 1));
            const json reward = DropRunReward(game, player, itemLevel,
                playerLevel, 7, false, rewardSeed, 0, nullptr,
                RoguelikeLoot::BaseKind::Equipment, 0, false,
                RoguelikeLoot::kChestUniqueSearchBonus,
                uniqueIds.data(), uniqueCount);
            if (!reward.value("ok", false) ||
                !reward.contains("item_id") ||
                !reward["item_id"].is_number_unsigned() ||
                !reward.contains("unique_id") ||
                !reward["unique_id"].is_number_integer() ||
                reward.value("corrupted", true) ||
                reward.value("quality", 0) != 7 ||
                reward.value("item_level", 0) != itemLevel) {
                runError = "owned chest uncorrupted unique creation uncertain";
                return;
            }
            const int uniqueId = reward["unique_id"].get<int>();
            if (!RoguelikeLoot::WssUniqueIdExcluded(
                    uniqueId, uniqueIds.data(), uniqueCount))
                uniqueIds[uniqueCount++] = uniqueId;
            runChestRewardIds[level][index][rewardIndex] =
                reward["item_id"].get<DWORD>();
            runChestRewardCounts[level][index] =
                static_cast<BYTE>(rewardIndex + 1);
        }
    }
}
void SettleRunAfterArrival(void* game, UnitAny* player) {
    const int level = runStage.load(std::memory_order_acquire);
    if (runGame.load(std::memory_order_acquire) != game ||
        runPlayer != player || !PortalArea(player, level)) return;
    if (level != 20) {
        if (level >= 13 && level <= 254 && zoneSlots[level].configured &&
            zoneSlots[level].kind == Pending::ZoneKind::Chest)
            SettleChestRewards(game, player, level);
        if (level >= 13 && level <= 254 &&
            runBossKillWitness.load(std::memory_order_acquire) &&
            !runCleared[level] && zoneSlots[level].configured &&
            BossZone(zoneSlots[level].kind) &&
            ObservationTelemetry::MatchesContext(runEpoch, game, player))
            ServerSpawns::TryCompleteClear(game, level);
        if (runResume.active) ContinueRunResume(game, player);
        return;
    }
    if (!runEncounters[20] && !runCleared[20]) {
        ServerMonsters::Request request;
        request.operation = ServerMonsters::Operation::Area;
        json census = ServerMonsters::Execute(game, player, request);
        if (!census.value("ok", false) || census.value("level_id", 0) != 20 ||
            !census.value("room_catalog_complete", false) ||
            !census.value("population_complete", false) ||
            census.value("inactive_hostile_alive_count", -1) != 0) return;
        runPopulationReady[20] = true;
        CompleteStarterEncounter(game, player);
    }
    if (runEncounters[20] || runCleared[20])
        runStarterSettled.store(true, std::memory_order_release);
    if (runResume.active) ContinueRunResume(game, player);
}
int __fastcall ChatHook(void* game, UnitAny* player, BYTE* packet, int size) {
    const std::string_view command = ChatMessageText(packet, size);
    if (command == "run aura") {
        PulseRunAura(game, player);
        return 0;
    }
    if (command == "run settle") {
        SettleRunAfterArrival(game, player);
        return 0;
    }
    if (command == "run start") {
        ReportStart(game, player, StartTownRun(game, player));
        return 0;
    }
    // Consume only owned nonce RPCs; forward all other chat.
    constexpr size_t textSize = 5 + 32; // .lab:<32 hex characters>
    if (packet && size == static_cast<int>(textSize + 6) && packet[0] == 0x15 &&
        packet[1] == 1 && packet[2] == 0 && !memcmp(packet + 3, ".lab:", 5) &&
        packet[3 + textSize] == 0 && packet[4 + textSize] == 0 && packet[5 + textSize] == 0) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& value : requests) {
                if (!value->completed && !value->running && value->id.size() == 32 &&
                    !memcmp(packet + 8, value->id.data(), 32) &&
                    (player && player->dwUnitId == value->playerId ||
                     !value->expectedEpoch.empty() && !value->portalJob &&
                     !value->triggerJob && !value->zoneJob && !value->levelInventory &&
                     !value->monsterJob && !value->traceJob)) {
                    pending = value; value->running = true; break;
                }
            }
        }
        if (pending) {
            json result;
            if (pending->runStart) {
                result = StartTownRun(game, player);
                result["entry_request_id"] = result.value("request_id", std::string());
                result["request_id"] = pending->id;
            } else if (pending->runArm) {
                result = ArmRun(game, player, *pending);
            } else if (pending->retireJob) {
                result = RetireOwnedPortals(game, player, *pending);
            } else if (pending->triggerJob) {
                result = CreateTrigger(game, player, *pending);
            } else if (pending->portalJob) {
                result = CreatePortal(game, player, *pending);
            } else if (pending->zoneJob) {
                result = ConfigureZone(game, player, *pending);
            } else if (pending->levelInventory) {
                result = InspectLevels(game, player, *pending);
            } else if (pending->traceJob) {
                result = CombatTelemetry::Start(game, player, *pending->traceJob);
                result["request_id"] = pending->id;
            } else if (pending->monsterJob) {
                result = pending->monsterJob->operation == ServerMonsters::Operation::Observe ?
                    ObservationTelemetry::Capture(game, player, *pending->monsterJob) :
                    ServerMonsters::Execute(game, player, *pending->monsterJob);
                result["request_id"] = pending->id;
            } else result = Generate(game, player, *pending);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (pending->portalJob || pending->triggerJob || pending->zoneJob || pending->retireJob) {
                    auto stored = portalReceipts.find(pending->id);
                    if (stored != portalReceipts.end() &&
                        stored->second.first == pending->fingerprint)
                        stored->second.second = std::move(result);
                    else pending->result = Failure("mutation nonce receipt missing");
                } else pending->result = std::move(result);
                pending->completed = true;
            }
            completedRequest.notify_all();
            return 0;
        }
    }
    return originalChat(game, player, packet, size);
}
bool Install(std::string& error) {
    HMODULE server = GetModuleHandleA("D2GAME.dll");
    if (!server) { error = "offline server is not loaded"; return false; }
    auto* table = reinterpret_cast<Entry*>(reinterpret_cast<BYTE*>(server) + kPacketTableRva);
    if (!Readable(table + 0x15, sizeof(Entry))) { error = "server callback table unavailable"; return false; }
    Entry* entry = table + 0x15;
    if (entry->callback == ChatHook) return true;
    if (!LabProfile::VerifyServer(error)) return false;
    auto* constructor = reinterpret_cast<BYTE*>(server) + kCreateRva;
    const BYTE prefix[] = {0x55,0x8B,0x6C,0x24,0x0C,0x57,0x8B,0x7C,0x24,0x0C,0x8B,0x47,0x70};
    auto* addGroundItem = reinterpret_cast<BYTE*>(server) + 0xE0F40;
    static const BYTE addPrefix[] = {
        0x53,0x55,0x8B,0x6C,0x24,0x0C,0x56,0x8B,0xF0,0x8B,0x44,0x24,0x14,0x85,0xC0,0x57};
    static const BYTE addCaller[] = {
        0x8B,0x44,0x24,0x1C,0x8B,0x4C,0x24,0x20,0x6A,0x01,0x53,0x50,
        0x8B,0x44,0x24,0x34,0x8B,0xD5,0xE8,0x42,0xEF,0xFF,0xFF};
    auto* addCallerSite = reinterpret_cast<BYTE*>(server) + 0xE1FE7;
    bool addMatches = Readable(addGroundItem, sizeof(addPrefix)) &&
        !memcmp(addGroundItem, addPrefix, sizeof(addPrefix)) &&
        Readable(addCallerSite, sizeof(addCaller)) &&
        !memcmp(addCallerSite, addCaller, sizeof(addCaller));
    bool codeMatches = Readable(constructor, sizeof(prefix)) && !memcmp(constructor, prefix, sizeof(prefix));
    if (!codeMatches && constructor[0] == 0xE9) {
        const BYTE* detour = constructor + 5 + *reinterpret_cast<int32_t*>(constructor + 1);
        codeMatches = ExecutableOwned(detour, server);
    }
    auto* roller = reinterpret_cast<BYTE*>(server) + kRollAffixRva;
    const BYTE rollPrefix[] = {0xB8,0x24,0x10,0,0};
    bool rollMatches = Readable(roller, sizeof(rollPrefix)) && !memcmp(roller, rollPrefix, sizeof(rollPrefix));
    if (!rollMatches && roller[0] == 0xE9)
        rollMatches = ExecutableOwned(roller + 5 + *reinterpret_cast<int32_t*>(roller + 1), server);
    if (!codeMatches || !rollMatches || !addMatches || entry->flag != 0 ||
        !ExecutableOwned(reinterpret_cast<void*>(entry->callback), server)) {
        error = "unexpected constructor, affix roller, ground-item registration or chat callback binding"; return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(Entry), PAGE_READWRITE, &old)) { error = "server callback protection denied"; return false; }
    Chat expected = entry->callback;
    originalChat = expected;
    createItem = reinterpret_cast<Create>(constructor);
    rollAffix = reinterpret_cast<RollAffix>(roller);
    auto prior = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&entry->callback),
        reinterpret_cast<PVOID>(ChatHook), reinterpret_cast<PVOID>(expected));
    DWORD ignored = 0;
    bool restored = VirtualProtect(entry, sizeof(Entry), old, &ignored) != 0;
    if (prior != reinterpret_cast<PVOID>(expected) || !restored) { error = "server callback changed or protection restore failed"; return false; }
    return true;
}


json Cached(const std::string& id, const std::string& fingerprint) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& value : requests) if (value->id == id) {
        if (value->fingerprint != fingerprint)
            return Failure("request_id reused for a different server specification or operation");
        if (!value->completed)
            return {{"ok", true}, {"status", "pending"}, {"request_id", id}};
        if (value->portalJob || value->triggerJob || value->retireJob) {
            auto stored = portalReceipts.find(id);
            if (stored != portalReceipts.end()) return stored->second.second;
        }
        return value->result;
    }
    return nullptr;
}

json Submit(const std::shared_ptr<Pending>& pending) {
    std::string error;
    if (!Install(error)) return Failure(error);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (requests.size() >= 64) {
            auto completed = requests.begin();
            while (completed != requests.end() && !(*completed)->completed) ++completed;
            if (completed == requests.end()) return Failure("too many unresolved server requests");
            requests.erase(completed);
        }
        requests.push_back(pending);
    }
    // S13 frame: message NUL, recipient NUL and zero-length extension.
    BYTE packet[43] = {0x15, 1, 0};
    memcpy(packet + 3, ".lab:", 5);
    memcpy(packet + 8, pending->id.data(), 32);
    if (!D2NET_SendPacket(sizeof(packet), 0, packet)) {
        std::lock_guard<std::mutex> lock(mutex);
        pending->result = Failure("native packet transport rejected the server trigger");
        pending->completed = true;
        return pending->result;
    }
    return {{"ok", true}, {"status", "requested"}, {"request_id", pending->id}};
}

json EnemyRequest(const json& args, ServerMonsters::Operation operation) {
    if (!args.is_object() || !args.contains("request_id") || !args["request_id"].is_string())
        return Failure("request_id is required");
    std::string id = args["request_id"].get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        return Failure("request_id must be 32 lowercase hex characters");
    const char* kind = operation == ServerMonsters::Operation::Spawn ? "enemies:" :
        operation == ServerMonsters::Operation::Inspect ? "enemy-state:" :
        operation == ServerMonsters::Operation::Observe ? "observation:" : "area-state:";
    std::string fingerprint = kind + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    ServerMonsters::Request request;
    std::string error;
    if (!ServerMonsters::Parse(args, operation, request, error)) return Failure(error);
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || player->dwType != UNIT_PLAYER) return Failure("player unavailable");
    auto pending = std::make_shared<Pending>();
    pending->id = std::move(id);
    pending->fingerprint = std::move(fingerprint);
    pending->playerId = player->dwUnitId;
    pending->monsterJob = std::move(request);
    return Submit(pending);
}
json TraceRequest(const json& args) {
    if (!args.is_object() || !args.contains("request_id") || !args["request_id"].is_string())
        return Failure("request_id is required");
    std::string id = args["request_id"].get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        return Failure("request_id must be 32 lowercase hex characters");
    std::string fingerprint = "trace:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || player->dwType != UNIT_PLAYER) return Failure("player unavailable");
    auto pending = std::make_shared<Pending>();
    pending->id = std::move(id);
    pending->fingerprint = std::move(fingerprint);
    pending->playerId = player->dwUnitId;
    pending->traceJob = args;
    return Submit(pending);
}
bool PortalNonce(const json& args, std::string& id, std::string& epoch) {
    if (!args.is_object() || !args.contains("request_id") || !args["request_id"].is_string() ||
        !args.contains("expected_epoch") || !args["expected_epoch"].is_string()) return false;
    id = args["request_id"].get<std::string>();
    epoch = args["expected_epoch"].get<std::string>();
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos &&
        !epoch.empty() && ObservationTelemetry::MatchesEpoch(epoch);
}
bool PortalArea(UnitAny* player, int expected) {
    return player && player->dwType == UNIT_PLAYER && player->pPath &&
        player->pPath->pRoom1 && player->pPath->pRoom1->pRoom2 &&
        player->pPath->pRoom1->pRoom2->pLevel &&
        player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo == expected;
}
json PortalCache(const std::string& id, const std::string& fingerprint) {
    std::lock_guard<std::mutex> lock(mutex);
    auto existing = portalReceipts.find(id);
    if (existing == portalReceipts.end()) return nullptr;
    if (existing->second.first != fingerprint) return Failure("portal request_id reused");
    return existing->second.second;
}
bool ReservePortalNonce(const std::string& id, const std::string& fingerprint, json receipt) {
    std::lock_guard<std::mutex> lock(mutex);
    if (portalReceipts.size() >= 4096 || portalReceipts.count(id)) return false;
    portalReceipts.emplace(id, std::make_pair(fingerprint, std::move(receipt)));
    return true;
}
void RememberOwned(const std::shared_ptr<Pending>& pending) {
    std::lock_guard<std::mutex> lock(mutex);
    ownedJobs.emplace(pending->id, pending);
}
// An object 60 alone does not identify its destination. Only a portal whose
// constructor receipt belongs to this game epoch can be selected.
int CreatedDestination(DWORD objectId, int source, const std::string& epoch) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& [id, request] : ownedJobs) {
        if (!request->portalJob || !request->completed ||
            request->expectedEpoch != epoch || request->portalJob->first != source)
            continue;
        auto receipt = portalReceipts.find(id);
        if (receipt == portalReceipts.end()) continue;
        const json& result = receipt->second.second;
        if (result.value("ok", false) && result.value("status", "") == "created" &&
            result.value("object_id", json()) == objectId)
            return request->portalJob->second;
    }
    return 0;
}
int CreatedTriggerDestination(DWORD objectId, int source, const std::string& epoch) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& [id, request] : ownedJobs) {
        if (!request->triggerJob || !request->completed ||
            request->expectedEpoch != epoch || request->triggerSource != source ||
            request->townEntry != (source == 1))
            continue;
        auto receipt = portalReceipts.find(id);
        if (receipt != portalReceipts.end() &&
            receipt->second.second.value("status", "") == "created" &&
            receipt->second.second.value("object_id", json()) == objectId)
            return *request->triggerJob;
    }
    return 0;
}
}

namespace ServerSpawns {
bool PreserveNativeMonsterClass(void* game, int area, int monsterClass) {
    if (!game || game != mapMonsterGame || mapMonsterEpoch != zoneEpoch ||
        area < 13 || area > 254 || monsterClass < 0 ||
        !zoneSlots[area].configured) return false;
    const auto& roll = zoneSlots[area].affixes;
    for (unsigned i = 0; i < roll.statCount; ++i)
        if (roll.stats[i].category == 2003 && roll.stats[i].value == monsterClass)
            return true;
    return false;
}
int CombatRunDepth(void* game, int area) {
    if (!game || game != zoneGame || area < 13 || area > 254) return 0;
    const ZoneSlot& zone = zoneSlots[area];
    if (!zone.configured ||
        (zone.kind != Pending::ZoneKind::Monster && !BossZone(zone.kind)) ||
        zone.depth < 1 || zone.depth > 99) return 0;
    return zone.depth;
}
void ApplyStockMapMonster(void* game, UnitAny* monster, int area) {
    // Stock map areas may already have state 0xC6 from PD2's spawn hook.
    // Its own applicator rejects duplicate statlists, so use the same guarded
    // path for our extended IDs and tolerate a prior stock application.
    if (!game || !monster || area < 13 || area > 254 ||
        mapMonsterGame != game || mapMonsterEpoch != zoneEpoch ||
        !zoneSlots[area].configured || !mapMonsterCount ||
        *reinterpret_cast<const WORD*>(static_cast<const BYTE*>(game) + 0x21f8) !=
            mapMonsterCount ||
        memcmp(static_cast<const BYTE*>(game) + 0x1df8, mapMonsterBytes.data(),
            8u * mapMonsterCount))
        return;
    const HMODULE mod = GetModuleHandleA("ProjectDiablo.dll");
    const BYTE* entry = mod ? reinterpret_cast<const BYTE*>(mod) + 0x2dba50 : nullptr;
    const BYTE signature[] = {0x55,0x8b,0xec,0x6a,0xff};
    if (!entry || !Readable(entry, sizeof(signature)) ||
        memcmp(entry, signature, sizeof(signature))) {
        runError = "pinned native map-monster applicator unavailable";
        return;
    }
    using MonsterApplicator = int (__fastcall*)(void*, UnitAny*, void*, int);
    auto apply = reinterpret_cast<MonsterApplicator>(const_cast<BYTE*>(entry));
    const int before = D2COMMON_GetUnitStat(monster, 7, 0);
    const int attached = apply(game, monster, static_cast<BYTE*>(game) + 0x1df8, area);
    const int after = D2COMMON_GetUnitStat(monster, 7, 0);
    StatList* state = D2COMMON_GetStateStatList(monster, 0xc6);
    if (!state || (!attached && !(area >= 137 && area <= 201))) {
        runError = "native map-monster statlist was not attached"; return;
    }
    const auto& roll = zoneSlots[area].affixes;
    if (roll.monsterHealthPercent &&
        ((!attached && D2COMMON_GetStatValueFromStatList(state, 0x1b0, 0) != 1) ||
         (attached && after <= before))) {
        runError = "native map-monster max-life effect is absent"; return;
    }
    // Stock's 3000-3999 item stats become special 0x1b0..0x1b4 records.
    // Their native monster applicator converts physical damage into real
    // elemental statlist entries instead of retaining the source marker.
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        if (stat.category != 3000 || stat.value <= 0) continue;
        const int target = stat.mapStat == 432 ? 50 :
            stat.mapStat == 433 ? 54 : stat.mapStat == 434 ? 48 :
            stat.mapStat == 435 ? 57 : stat.mapStat == 436 ? 52 : 0;
        if (!target || D2COMMON_GetStatValueFromStatList(state, target, 0) <= 0) {
            runError = "native physical-as-extra map stat " +
                std::to_string(stat.mapStat) + " did not yield elemental damage";
            return;
        }
    }
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        if (stat.category >= 1000 || stat.category == 76) continue;
        const WORD layer = static_cast<WORD>(stat.layer ? stat.layer : stat.mapStat);
        const int expected = stat.value ? stat.value : 1;
        if (D2COMMON_GetStatValueFromStatList(
                state, static_cast<int>(stat.category), layer) != expected &&
            D2COMMON_GetStatValueFromStatList(
                state, static_cast<int>(stat.category), 0) != expected) {
            runError = "native monster map stat " + std::to_string(stat.mapStat) +
                " did not attach target " + std::to_string(stat.category) +
                " with source layer " + std::to_string(layer) +
                "; source=" + std::to_string(D2COMMON_GetStatValueFromStatList(
                    state, static_cast<int>(stat.category), layer)) +
                "; effective=" + std::to_string(D2COMMON_GetStatValueFromStatList(
                    state, static_cast<int>(stat.category), 0));
            return;
        }
    }
    bool changed = false;
    for (unsigned i = 0; i < roll.statCount; ++i) {
        const auto& stat = roll.stats[i];
        if (stat.category >= 1000 || stat.category == 76 || stat.layer) continue;
        const int target = static_cast<int>(stat.category);
        const int value = stat.value ? stat.value : 1;
        // Physical-as-extra inserts a lower-ID elemental stat; the stock
        // ordinal layer stamp can then land on this already-effective entry.
        if (D2COMMON_GetStatValueFromStatList(state, target, 0) == value) continue;
        D2COMMON_SetPropertyStat(state, target, 0, static_cast<WORD>(stat.mapStat));
        D2COMMON_SetPropertyStat(state, target, value, 0);
        if (D2COMMON_GetStatValueFromStatList(state, target, 0) != value) {
            runError = "native monster statlist layer normalization failed"; return;
        }
        changed = true;
    }
    if (changed) {
        const BYTE* refresh = reinterpret_cast<const BYTE*>(mod) + 0x2778f0;
        if (!Readable(refresh, 8)) {
            runError = "native monster state refresh unavailable"; return;
        }
        using Refresh = void (__stdcall*)(UnitAny*);
        reinterpret_cast<Refresh>(const_cast<BYTE*>(refresh))(monster);
    }
    if (!attached) ++mapMonsterStockExisting;
    else ++mapMonsterApplied;
    if (!mapMonsterSampleUnit) {
        mapMonsterSampleUnit = monster->dwUnitId;
        mapMonsterSampleArea = area;
        mapMonsterSampleBefore = before;
        mapMonsterSampleAfter = after;
    }
}
void OnWorldPopulationCompleted() {
    const int arrival = runArrivalLevel;
    if (arrival == 20 ||
        (arrival >= 13 && arrival <= 254 && zoneSlots[arrival].configured))
        OnPopulationCompleted(arrival);
    const int level = runStage.load(std::memory_order_acquire);
    if (level >= 13 && level <= 254 && PortalArea(runPlayer, level))
        OnPopulationCompleted(level);
    if (runResume.active && runPlayer && PortalArea(runPlayer,
            runStage.load(std::memory_order_acquire)))
        ContinueRunResume(runGame.load(std::memory_order_acquire), runPlayer);
}
void OnPopulationCompleted(int level) {
    if (level < 13 || level > 254 ||
        (level != 20 && !zoneSlots[level].configured)) return;
    void* game = runGame.load(std::memory_order_acquire);
    if (!game || (level != 20 && zoneGame != game) || !runPlayer ||
        !ObservationTelemetry::MatchesContext(runEpoch, game, runPlayer)) return;
    runPopulationReady[level] = true;
    if (runStage.load(std::memory_order_acquire) == level &&
        PortalArea(runPlayer, level)) {
        if (level == 20) CompleteStarterEncounter(game, runPlayer);
        else CompleteArenaEncounter(game, runPlayer, level);
    }
}
struct BossCandidate { int classId = -1, superuniqueId = -1; };
template <typename Visit>
bool ForEachBossCatalog(const char* table, Visit visit, std::string& error) {
    int total = -1;
    for (int offset = 0;;) {
        const json page = ServerMonsters::Catalog(
            {{"table", table}, {"offset", offset}, {"limit", 64}});
        if (!page.value("ok", false)) {
            error = page.value("error", std::string("native boss catalog unavailable"));
            return false;
        }
        if (!page.contains("records") || !page["records"].is_array()) {
            error = "native boss catalog page is malformed";
            return false;
        }
        const int pageTotal = page.value("total", -1);
        if (pageTotal < 0 || pageTotal > 32768 ||
            page.value("offset", -1) != offset ||
            (total >= 0 && pageTotal != total) ||
            offset > pageTotal ||
            offset + static_cast<int>(page["records"].size()) > pageTotal) {
            error = "native boss catalog page boundaries changed";
            return false;
        }
        total = pageTotal;
        for (const json& row : page["records"])
            if (!visit(row, error)) return false;
        const int next = offset + static_cast<int>(page["records"].size());
        if (next == total) return true;
        if (next == offset) {
            error = "native boss catalog page was empty before completion";
            return false;
        }
        offset = next;
    }
}
struct BossSelection {
    void* game = nullptr;
    const sgptDataTable* tables = nullptr;
    std::minstd_rand* random = nullptr;
    unsigned depth = 0;
    unsigned candidates = 0;
    std::array<std::uint64_t, 512> superuniqueClasses{};
    BossCandidate selected{};
};
void ChooseBossCandidate(BossSelection& selection, BossCandidate candidate) {
    if ((*selection.random)() % ++selection.candidates == 0)
        selection.selected = candidate;
}
bool BossCandidateEligible(const BossSelection& selection,
    const json& row, int classId) {
    const auto name = row.find("name");
    if (!selection.tables || !selection.tables->pMonStatsTxt ||
        classId < 0 ||
        static_cast<DWORD>(classId) >= selection.tables->dwMonStatsRecs ||
        name == row.end() || !name->is_string())
        return false;
    const std::string& displayName = name->get_ref<const std::string&>();
    const int nativeLevel = selection.tables->pMonStatsTxt[classId].wLevel;
    return RoguelikeBosses::EligibleCandidateAtDepth(
        nativeLevel, selection.depth, classId, displayName);
}
bool ConsiderSuperuniqueBoss(const json& row, BossSelection& selection,
    std::string& detail) {
    if (!row.value("spawnable", false)) return true;
    const int id = row.value("superunique_id", -1);
    const int classId = row.value("class_id", -1);
    const int minGroup = row.value("min_group", -1);
    const int maxGroup = row.value("max_group", -1);
    if (id < 0 || id >= 32768 || classId < 0 || classId >= 32768 ||
        minGroup < 0 || maxGroup < minGroup) {
        detail = "native superunique candidate is malformed";
        return false;
    }
    selection.superuniqueClasses[classId / 64] |=
        std::uint64_t{1} << (classId & 63);
    if (maxGroup > 16 || !BossCandidateEligible(selection, row, classId))
        return true;
    if (!row.value("stacks", false)) {
        const size_t byte = 0x1D30 + static_cast<unsigned>(id) / 8;
        if (!Readable(selection.game, byte + 1)) {
            detail = "native superunique encounter bits unavailable";
            return false;
        }
        if (*(static_cast<const BYTE*>(selection.game) + byte) &
            (1u << (static_cast<unsigned>(id) & 7)))
            return true;
    }
    ChooseBossCandidate(selection, {classId, id});
    return true;
}
bool ConsiderBossMonster(const json& row, BossSelection& selection,
    std::string& detail) {
    if (!row.value("spawnable", false) ||
        !row.value("enabled", false) ||
        !row.value("killable", false) ||
        row.value("npc", true) || row.value("town", true) ||
        (!row.value("set_boss", false) &&
         !row.value("prime_evil", false)))
        return true;
    const int classId = row.value("class_id", -1);
    if (classId < 0 || classId >= 32768) {
        detail = "native boss monster class is malformed";
        return false;
    }
    if ((selection.superuniqueClasses[classId / 64] &
        (std::uint64_t{1} << (classId & 63))) ||
        !BossCandidateEligible(selection, row, classId))
        return true;
    ChooseBossCandidate(selection, {classId, -1});
    return true;
}
bool SelectBossCandidate(void* game, unsigned depth,
    std::minstd_rand& random, BossCandidate& selected, std::string& error) {
    sgptDataTable* tables = Tables();
    if (!game || depth == 0 || depth > 99 || !tables ||
        !tables->pMonStatsTxt || !tables->dwMonStatsRecs) {
        error = "boss candidate context or native level table unavailable";
        return false;
    }
    BossSelection selection{};
    selection.game = game;
    selection.tables = tables;
    selection.random = &random;
    selection.depth = depth;
    if (!ForEachBossCatalog("superuniques",
            [&](const json& row, std::string& detail) {
                return ConsiderSuperuniqueBoss(row, selection, detail);
            }, error))
        return false;
    if (!ForEachBossCatalog("monsters",
            [&](const json& row, std::string& detail) {
                return ConsiderBossMonster(row, selection, detail);
            }, error))
        return false;
    if (!selection.candidates) {
        error = "no compact spawnable boss candidate is available";
        return false;
    }
    selected = selection.selected;
    error.clear();
    return true;
}
bool SelectVanillaEncounterPool(unsigned depth, std::minstd_rand& random,
    const std::array<std::uint16_t, 2>& excludedPrimaries,
    unsigned excludedCount,
    RoguelikeProgression::EncounterSelection& selection,
    int& densityBaseline, std::string& error) {
    selection = {};
    densityBaseline = 0;
    sgptDataTable* table = Tables();
    if (!table || !table->pLevelsTxt || table->dwLevelsRecs < 138 ||
        table->dwLevelsRecs > 1024 ||
        !Readable(table->pLevelsTxt,
            size_t(table->dwLevelsRecs) * sizeof(LevelsTxt)) ||
        !table->pMonStatsTxt || !table->dwMonStatsRecs ||
        table->dwMonStatsRecs > 32768 ||
        !Readable(table->pMonStatsTxt,
            size_t(table->dwMonStatsRecs) * sizeof(MonStatsTxt))) {
        error = "unrecognized loaded vanilla level or monster table";
        return false;
    }
    std::array<RoguelikeProgression::VanillaArea, 137> areas{};
    for (unsigned id = 1; id <= 137; ++id) {
        const LevelsTxt* row = zoneSlots[id].configured ?
            &zoneSlots[id].original : &table->pLevelsTxt[id];
        if (!Readable(row, sizeof(LevelsTxt))) continue;
        auto& area = areas[id - 1];
        area.id = id;
        area.selections = row->dwNumMon & 0xFF;
        for (unsigned difficulty = 0; difficulty < 3; ++difficulty) {
            area.levels[difficulty] = row->wMonLvlEx[difficulty];
            area.densities[difficulty] = row->dwMonDen[difficulty];
        }
        area.normalRoster = row->wMon;
        area.nightmareRoster = row->wNMon;
    }
    const auto spawnable = [table](std::uint16_t classId) {
        if (classId >= table->dwMonStatsRecs ||
            !Readable(table->pMonStatsTxt + classId, sizeof(MonStatsTxt)))
            return false;
        const MonStatsTxt& monster = table->pMonStatsTxt[classId];
        return monster.dwFlags.bEnabled && monster.dwFlags.bKillable &&
            !monster.dwFlags.bNPC && !monster.dwFlags.binTown;
    };
    if (!RoguelikeProgression::SelectVanillaEncounters(
            areas.data(), areas.size(), depth, table->dwMonStatsRecs,
            spawnable, random, selection, excludedPrimaries, excludedCount)) {
        error = "vanilla progression has no spawnable classes or density";
        return false;
    }
    densityBaseline = static_cast<int>(selection.density);
    error.clear();
    return true;
}
Pending::ZoneKind ZoneKindForOffer(RoguelikeProgression::OfferKind kind) {
    switch (kind) {
    case RoguelikeProgression::OfferKind::Monster:
        return Pending::ZoneKind::Monster;
    case RoguelikeProgression::OfferKind::Market:
        return Pending::ZoneKind::Merchant;
    case RoguelikeProgression::OfferKind::Sanctuary:
        return Pending::ZoneKind::Shrine;
    case RoguelikeProgression::OfferKind::Chest:
        return Pending::ZoneKind::Chest;
    case RoguelikeProgression::OfferKind::Boss:
        return Pending::ZoneKind::Boss;
    }
    return Pending::ZoneKind::Monster;
}
bool RewardFocusAvailable(const Pending::Zone& zone,
    Pending::RewardFocus focus, std::minstd_rand& random);
// Both initial offers and a scroll reroll use precisely this event/affix
// generator. Its native map-item roller resolves real compiled stock stats;
// a synthetic roll is not counted as a natural monster drop.
bool RollChoiceZone(void* game, int target, unsigned progressed,
    const RoguelikeProgression::EncounterSelection& naturalEncounter,
    int naturalDensityBaseline, std::minstd_rand& random,
    Pending::Zone& zone, std::string& error, bool allowMarket = true,
    std::optional<Pending::ZoneKind> forcedKind = std::nullopt) {
    if (forcedKind && *forcedKind == Pending::ZoneKind::Merchant &&
        !RoguelikeProgression::CanOfferKind(
            RoguelikeProgression::OfferKind::Market, allowMarket)) {
        error = "a Market is already offered in this choice set";
        return false;
    }
    const unsigned eventRoll = random() % RoguelikeProgression::offerBasisPoints;
    const Pending::ZoneKind rolledKind = ZoneKindForOffer(
        RoguelikeProgression::ChooseOfferKind(progressed, eventRoll, allowMarket));
    Pending::ZoneKind kind = forcedKind.value_or(rolledKind);
    BossCandidate boss{};
    if (kind == Pending::ZoneKind::Boss &&
        !SelectBossCandidate(game, progressed, random, boss, error)) {
        if (forcedKind ||
            error != "no compact spawnable boss candidate is available")
            return false;
        kind = Pending::ZoneKind::Monster;
        error.clear();
    }
    RoguelikeAffixes::Roll affixes{};
    if (kind == Pending::ZoneKind::Monster) {
        bool resolved = false;
        for (unsigned attempt = 0; attempt < 12; ++attempt) {
            affixes = RoguelikeAffixes::Generate(
                progressed, mapCatalog.data(), mapCatalogCount, random);
            if (affixes.quality != RoguelikeAffixes::Quality::Normal && !affixes.count)
                continue;
            error.clear();
            if (ResolveMapRoll(game, runPlayer, progressed, random, affixes, error)) {
                resolved = true;
                break;
            }
            if (error != "native map item did not materialize all affix stats")
                break;
        }
        if (!resolved) return false;
    }
    const bool natural = kind == Pending::ZoneKind::Monster;
    if (natural && (!naturalEncounter.count ||
        naturalEncounter.count > naturalEncounter.classes.size())) {
        error = "vanilla monster roster is empty or oversized";
        return false;
    }
    const int monsterClass = natural ? naturalEncounter.classes[0] :
        BossZone(kind) ? boss.classId : 5;
    const int offeredLevel = RoguelikeAffixes::ZoneLevel(progressed, affixes.areaLevel);
    Pending::RewardFocus reward = Pending::RewardFocus::Equipment;
    if (kind == Pending::ZoneKind::Monster) {
        Pending::Zone availabilityZone{};
        availabilityZone.monsterLevel = offeredLevel;
        availabilityZone.depth = static_cast<int>(progressed);
        constexpr std::array<Pending::RewardFocus, 6> focusOptions = {
            Pending::RewardFocus::Equipment,
            Pending::RewardFocus::Socketables,
            Pending::RewardFocus::Unique,
            Pending::RewardFocus::Set,
            Pending::RewardFocus::Jewelry,
            Pending::RewardFocus::Charms
        };
        std::array<Pending::RewardFocus, 6> available{};
        unsigned availableCount = 0;
        for (const Pending::RewardFocus focus : focusOptions)
            if (RewardFocusAvailable(availabilityZone, focus, random))
                available[availableCount++] = focus;
        std::uniform_int_distribution<unsigned> roll(
            0, RoguelikeLoot::kBasisPoints - 1);
        reward = RoguelikeLoot::RewardFocusForRoll(
            available.data(), availableCount, roll(random));
    }
    BYTE otherShrine = 0, secondOtherShrine = 0;
    if (kind == Pending::ZoneKind::Shrine) {
        otherShrine = static_cast<BYTE>(6 + random() % 10);
        secondOtherShrine = static_cast<BYTE>(6 + random() % 9);
        if (secondOtherShrine >= otherShrine) ++secondOtherShrine;
    }
    zone = Pending::Zone{target, monsterClass, offeredLevel,
        natural ? RoguelikeAffixes::NativeDensity(
            naturalDensityBaseline, affixes.densityPercent) : 0,
        natural ? affixes.magicFind : 0, static_cast<int>(progressed), affixes,
        kind, otherShrine, reward, boss.superuniqueId, secondOtherShrine};
    if (natural) {
        zone.monsterClassCount = naturalEncounter.count;
        for (unsigned index = 0; index < naturalEncounter.count; ++index)
            zone.monsterClasses[index] = naturalEncounter.classes[index];
    }
    zone.densityBaseline = natural ? naturalDensityBaseline : 0;
    return true;
}
void OfferChoices(void* game, int level, const RunLayout* supplied) {
    if (!runPlayer || !runPlayer->pPath) {
        runError = "choice layout has no current run player"; return;
    }
    if (level < 0 || level >= 255 || runRouteNext[level] >= 0) return;
    RunLayout planned{};
    if (!supplied) {
        if (!PlanRunLayout(D2COMMON_GetRoomFromUnit(runPlayer), runPlayer,
                           0, planned)) {
            runError = "no spacious, reachable choice layout"; return;
        }
        supplied = &planned;
    }
    std::array<int, 121> fresh{};
    unsigned freshCount = 0;
    for (int target = 13; target <= 254; ++target) {
        if (target == 17) target = 138;
        if (FreshRunArenaAvailable(runPlayer, target))
            fresh[freshCount++] = target;
    }
    if (freshCount < 3) return;
    std::string catalogError;
    if (!MapCatalog(catalogError)) {
        runError = "stock map-affix catalog rejected: " + catalogError; return;
    }
    const DWORD frame = *reinterpret_cast<const DWORD*>(
        reinterpret_cast<const BYTE*>(game) + 0xA8);
    std::minstd_rand random(GetTickCount() ^ frame ^ (level * 2654435761u));
    std::shuffle(fresh.begin(), fresh.begin() + freshCount, random);
    unsigned progressed = runRootLevel == 20 ? 2 : 1;
    for (int arena = 13; arena <= 254; ++arena) {
        if (arena == 17) arena = 138;
        if (zoneSlots[arena].configured) ++progressed;
    }
    std::array<RoguelikeProgression::EncounterSelection, 3> encounters{};
    std::array<std::uint16_t, 2> primaryClasses{};
    std::array<int, 3> densityBaselines{};
    std::string encounterError;
    for (unsigned index = 0; index < encounters.size(); ++index) {
        if (!SelectVanillaEncounterPool(progressed, random, primaryClasses,
                index, encounters[index], densityBaselines[index], encounterError)) {
            runError = "vanilla encounter progression rejected: " + encounterError;
            return;
        }
        if (index < primaryClasses.size())
            primaryClasses[index] = encounters[index].classes[0];
    }
    std::array<DWORD, 3> created{};
    unsigned createdCount = 0;
    const auto rollback = [&] {
        for (unsigned i = 0; i < createdCount; ++i) {
            const DWORD id = created[i];
            UnitAny* unit = FindServerObject(game, id);
            if (unit && RemoveOwnedObject(game, runPlayer, id, unit)) {
                std::lock_guard<std::mutex> lock(mutex);
                choiceNames.erase(id);
                ForgetRunOwnedObject(game, id, unit);
            }
        }
    };
    bool marketAllowed = true;
    for (unsigned index = 0; index < 3; ++index) {
        auto choice = std::make_shared<Pending>();
        choice->id = "native-choice:" + runEpoch + ":" +
            std::to_string(++runChoiceSerial);
        choice->expectedEpoch = runEpoch;
        choice->runSerial = runActiveSerial;
        choice->playerId = runPlayer->dwUnitId;
        choice->triggerSource = level;
        choice->triggerJob = fresh[index];
        Pending::Zone zone{};
        std::string rollError;
        if (!RollChoiceZone(game, fresh[index], progressed, encounters[index],
                densityBaselines[index], random, zone, rollError, marketAllowed)) {
            runError = "stock map-affix roll failed: " + rollError;
            rollback();
            return;
        }
        choice->zoneJob = zone;
        json result = CreateTrigger(game, runPlayer, *choice,
            &supplied->portals[index]);
        if (!result.value("ok", false) || !result.contains("object_id") ||
            !result["object_id"].is_number_unsigned()) {
            runError = "three-choice creation failed on a certified clear";
            rollback();
            return;
        }
        choice->completed = true;
        choice->result = result;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ownedJobs.emplace(choice->id, choice);
            portalReceipts.emplace(choice->id, std::make_pair("native-clear", result));
        }
        created[createdCount++] = result["object_id"].get<DWORD>();
        marketAllowed = marketAllowed &&
            zone.kind != Pending::ZoneKind::Merchant;
    }
    runError.clear();
}
// Effect-specific rerolls keep the selected destination; the original Talisman
// still replaces it with a newly rolled destination.
bool RerollZoneKind(RoguelikeLoot::RerollEffect effect,
    Pending::ZoneKind& kind) {
    switch (effect) {
    case RoguelikeLoot::RerollEffect::BossZone:
        kind = Pending::ZoneKind::Boss; return true;
    case RoguelikeLoot::RerollEffect::MerchantZone:
        kind = Pending::ZoneKind::Merchant; return true;
    case RoguelikeLoot::RerollEffect::ShrineZone:
        kind = Pending::ZoneKind::Shrine; return true;
    case RoguelikeLoot::RerollEffect::ChestZone:
        kind = Pending::ZoneKind::Chest; return true;
    default:
        return false;
    }
}
bool RerollRewardFocus(RoguelikeLoot::RerollEffect effect,
    Pending::RewardFocus& focus) {
    switch (effect) {
    case RoguelikeLoot::RerollEffect::EquipmentReward:
        focus = Pending::RewardFocus::Equipment; return true;
    case RoguelikeLoot::RerollEffect::SocketablesReward:
        focus = Pending::RewardFocus::Socketables; return true;
    case RoguelikeLoot::RerollEffect::UniqueReward:
        focus = Pending::RewardFocus::Unique; return true;
    case RoguelikeLoot::RerollEffect::SetReward:
        focus = Pending::RewardFocus::Set; return true;
    case RoguelikeLoot::RerollEffect::JewelryReward:
        focus = Pending::RewardFocus::Jewelry; return true;
    case RoguelikeLoot::RerollEffect::CharmsReward:
        focus = Pending::RewardFocus::Charms; return true;
    default:
        return false;
    }
}
const char* RerollEffectName(RoguelikeLoot::RerollEffect effect) {
    switch (effect) {
    case RoguelikeLoot::RerollEffect::Random: return "random";
    case RoguelikeLoot::RerollEffect::BossZone: return "boss_zone";
    case RoguelikeLoot::RerollEffect::MerchantZone: return "merchant_zone";
    case RoguelikeLoot::RerollEffect::ShrineZone: return "shrine_zone";
    case RoguelikeLoot::RerollEffect::ChestZone: return "chest_zone";
    case RoguelikeLoot::RerollEffect::EquipmentReward: return "equipment";
    case RoguelikeLoot::RerollEffect::SocketablesReward: return "socketables";
    case RoguelikeLoot::RerollEffect::UniqueReward: return "unique";
    case RoguelikeLoot::RerollEffect::SetReward: return "set";
    case RoguelikeLoot::RerollEffect::JewelryReward: return "jewelry";
    case RoguelikeLoot::RerollEffect::CharmsReward: return "charms";
    default: return "invalid";
    }
}
bool RewardFocusAvailable(const Pending::Zone& zone,
    Pending::RewardFocus focus, std::minstd_rand& random) {
    if (focus == Pending::RewardFocus::Equipment) return true;
    const int itemLevel = RoguelikeLoot::RewardItemLevel(
        zone.monsterLevel, zone.depth, zone.testOnly);
    const int playerLevel = MarketCharacterLevel();
    Pending candidate;
    candidate.quality = 2;
    int sockets = 0;
    RoguelikeLoot::BaseKind kind = RoguelikeLoot::BaseKind::Equipment;
    switch (focus) {
    case Pending::RewardFocus::Socketables:
        sockets = 1;
        break;
    case Pending::RewardFocus::Unique:
        candidate.quality = 7;
        break;
    case Pending::RewardFocus::Set:
        candidate.quality = 5;
        break;
    case Pending::RewardFocus::Jewelry:
        kind = RoguelikeLoot::BaseKind::Jewelry;
        break;
    case Pending::RewardFocus::Charms:
        kind = RoguelikeLoot::BaseKind::Charm;
        break;
    case Pending::RewardFocus::Equipment:
        return true;
    }
    return SelectReward(candidate, itemLevel, playerLevel, false,
        static_cast<DWORD>(random()), sockets, kind);
}
bool IsSiblingMarketOffer(const std::string& id,
    const std::shared_ptr<Pending>& candidate, const Pending& choice) {
    return candidate && id != choice.id && candidate->zoneJob &&
        candidate->runSerial == choice.runSerial &&
        !candidate->zoneJob->testOnly &&
        candidate->zoneJob->kind == Pending::ZoneKind::Merchant;
}
bool RerollOfferedChoice(void* game, UnitAny* player,
    const std::shared_ptr<Pending>& choice, DWORD objectId, DWORD itemId,
    RoguelikeLoot::RerollEffect effect) {
    UnitAny* item = FindRolledItem(game, itemId);
    const ItemsTxt* itemText = item ?
        D2COMMON_GetItemText(item->dwTxtFileNo) : nullptr;
    UnitAny* object = FindServerObject(game, objectId);
    if (effect == RoguelikeLoot::RerollEffect::None || !player || !choice ||
        !item || !Readable(item->pItemData, sizeof(ItemData)) ||
        !Readable(itemText, sizeof(ItemsTxt)) ||
        RoguelikeLoot::RerollEffectForCode(itemText->szCode) != effect ||
        !OwnedUseItem(game, player, itemId, itemText->szCode) ||
        !object || !choice->zoneJob || !choice->triggerJob ||
        choice->triggerSource < 0 || choice->triggerSource >= 255 ||
        !Readable(object->pObjectPath, sizeof(ObjectPath)) ||
        !Readable(object->pObjectPath->pRoom1, sizeof(Room1)) ||
        !Readable(object->pObjectPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(object->pObjectPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
        object->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo !=
            static_cast<DWORD>(choice->triggerSource) ||
        D2COMMON_GetUnitDistance(player, object) >= 9 ||
        !ObservationTelemetry::MatchesContext(choice->expectedEpoch, game, player) ||
        choice->expectedEpoch != runEpoch ||
        runStage.load(std::memory_order_acquire) != choice->triggerSource ||
        !runCleared[choice->triggerSource] || !runError.empty() ||
        !TargetUnused(player, *choice->triggerJob)) return false;
    bool marketAlreadyOffered = false;
    std::array<bool, 255> offered{};
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto selected = selectedTriggerEpochBySource.find(choice->triggerSource);
        const auto name = choiceNames.find(objectId);
        const auto receipt = portalReceipts.find(choice->id);
        if ((selected != selectedTriggerEpochBySource.end() &&
             selected->second == choice->expectedEpoch) ||
            name == choiceNames.end() || receipt == portalReceipts.end() ||
            name->second.playerId != player->dwUnitId ||
            name->second.epoch != choice->expectedEpoch ||
            receipt->second.second.value("object_id", json()) != objectId)
            return false;
        for (const auto& [id, job] : ownedJobs) {
            if (!job->completed || !job->triggerJob ||
                job->expectedEpoch != choice->expectedEpoch ||
                job->runSerial != choice->runSerial ||
                job->triggerSource != choice->triggerSource ||
                *job->triggerJob < 0 || *job->triggerJob >= 255)
                continue;
            marketAlreadyOffered |= IsSiblingMarketOffer(id, job, *choice);
            offered[*job->triggerJob] = true;
        }
    }
    const DWORD frame = *reinterpret_cast<const DWORD*>(
        reinterpret_cast<const BYTE*>(game) + 0xA8);
    std::minstd_rand random(GetTickCount() ^ frame ^ (itemId * 2654435761u));
    std::array<std::uint16_t, 2> noExcluded{};
    RoguelikeProgression::EncounterSelection encounter{};
    int densityBaseline = 0;
    const int oldTarget = *choice->triggerJob;
    int newTarget = oldTarget;
    Pending::Zone newZone{};
    if (effect == RoguelikeLoot::RerollEffect::Random) {
        std::array<int, 121> fresh{};
        unsigned freshCount = 0;
        for (int target = 13; target <= 254; ++target) {
            if (target == 17) target = 138;
            if (offered[target] || !FreshRunArenaAvailable(player, target))
                continue;
            fresh[freshCount++] = target;
        }
        if (!freshCount) return false;
        std::shuffle(fresh.begin(), fresh.begin() + freshCount, random);
        std::string error;
        if (!SelectVanillaEncounterPool(choice->zoneJob->depth, random,
                noExcluded, 0, encounter, densityBaseline, error)) return false;
        if (!MapCatalog(error) ||
            !RollChoiceZone(game, fresh[0], choice->zoneJob->depth,
                encounter, densityBaseline, random, newZone, error,
                !marketAlreadyOffered)) return false;
        newTarget = fresh[0];
    } else {
        Pending::ZoneKind zoneKind{};
        Pending::RewardFocus rewardFocus{};
        if (RerollZoneKind(effect, zoneKind)) {
            std::string error;
            RoguelikeProgression::EncounterSelection encounter{};
            const std::array<std::uint16_t, 2> noExcluded{};
            int densityBaseline = 0;
            if (zoneKind == Pending::ZoneKind::Monster &&
                !SelectVanillaEncounterPool(choice->zoneJob->depth, random,
                    noExcluded, 0, encounter, densityBaseline, error))
                return false;
            if (!RollChoiceZone(game, oldTarget, choice->zoneJob->depth,
                    encounter, densityBaseline, random, newZone, error,
                    !marketAlreadyOffered, zoneKind)) return false;
        } else if (RerollRewardFocus(effect, rewardFocus)) {
            if (choice->zoneJob->kind != Pending::ZoneKind::Monster &&
                !BossZone(choice->zoneJob->kind)) return false;
            newZone = *choice->zoneJob;
            newZone.reward = rewardFocus;
            if (!RewardFocusAvailable(newZone, rewardFocus, random))
                return false;
        } else {
            return false;
        }
    }
    sgptDataTable* tables = Tables();
    if (!tables || !tables->pMonStatsTxt ||
        static_cast<DWORD>(newZone.classId) >= tables->dwMonStatsRecs ||
        !Readable(tables->pMonStatsTxt + newZone.classId, sizeof(MonStatsTxt)))
        return false;
    const wchar_t* monsterName =
        D2LANG_GetLocaleText(tables->pMonStatsTxt[newZone.classId].wNameStr);
    if (!monsterName || !Readable(monsterName, sizeof(wchar_t)) ||
        wcsnlen_s(monsterName, 48) == 48) return false;
    ChoiceName replacement{choice->playerId, choice->triggerSource,
        choice->expectedEpoch, {}};
    if (!FormatChoiceName(replacement.text, monsterName, newZone, false))
        return false;
    if (!ConsumeRunItem(game, player, item, itemText->szCode)) return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (effect == RoguelikeLoot::RerollEffect::Random) {
            choice->triggerJob = newTarget;
            runRetiredDestinations[oldTarget] = true;
        }
        choice->zoneJob = newZone;
        choiceNames[objectId] = std::move(replacement);
        auto& receipt = portalReceipts.at(choice->id).second;
        receipt["destination_level"] = newTarget;
        receipt["zone_kind"] = static_cast<int>(newZone.kind);
        receipt["reward_focus"] = static_cast<int>(newZone.reward);
        receipt["zone_affixes"] = ZoneAffixReceipt(newZone.affixes);
        receipt["monster_level"] = newZone.monsterLevel;
        receipt["density"] = newZone.density;
        receipt["magic_find"] = newZone.magicFind;
        receipt["reroll"] = {{"effect", RerollEffectName(effect)},
            {"destination_changed", newTarget != oldTarget}, {"item_id", itemId},
            {"status", "native_item_consumed"}};
        choice->result = receipt;
    }
    return true;
}
void ScaleRoguelikeDamage(void* game, UnitAny* attacker, UnitAny* defender,
    int bMissile, void* damage) {
    const int level = runStage.load(std::memory_order_acquire);
    if (runGame.load(std::memory_order_acquire) != game || level < 13 ||
        level > 254 || !zoneSlots[level].configured ||
        !BossZone(zoneSlots[level].kind) ||
        !runBossDamageCount || !attacker || !defender ||
        !Readable(attacker, sizeof(UnitAny)) || !Readable(defender, sizeof(UnitAny)) ||
        !Readable(defender->pPath, sizeof(Path)) ||
        !Readable(defender->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(defender->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(defender->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
        defender->pPath->pRoom1->pRoom2->pLevel->dwLevelNo != static_cast<DWORD>(level) ||
        !Readable(damage, 0x2c)) return;
    const bool projectile = bMissile != 0;
    const bool missileUnit = attacker->dwType == UNIT_MISSILE;
    if (projectile) {
        ++runObservedMissileDamage;
        if (!runMissileDamageSample.id)
            runMissileDamageSample = {attacker->dwUnitId, attacker->dwTxtFileNo,
                attacker->dwType, attacker->dwOwnerType, attacker->dwOwnerId};
    } else if (attacker->dwType != UNIT_MONSTER) {
        ++runOtherAttackerDamage;
    }
    if (attacker->dwType != UNIT_MONSTER &&
        !(missileUnit && attacker->dwOwnerType == UNIT_MONSTER)) return;
    const DWORD sourceId = missileUnit ? attacker->dwOwnerId : attacker->dwUnitId;
    WORD nativeLevel = 0;
    for (unsigned i = 0; i < runBossDamageCount; ++i) {
        const BossDamageSource& source = runBossDamageSources[i];
        if (source.id != sourceId) continue;
        // Missile owner fields only nominate a monster. Validate its live
        // server identity, and never scale a recycled GUID or another unit.
        UnitAny* owner = FindServerMonster(game, sourceId);
        if (!owner || owner != source.unit || (!missileUnit && attacker != owner)) {
            if (projectile) ++runRejectedMissileOwners;
            return;
        }
        nativeLevel = source.nativeLevel;
        break;
    }
    if (!nativeLevel) return;
    constexpr size_t components[] = {8, 0x10, 0x1c, 0x20, 0x24, 0x28};
    int scaled[6];
    auto* bytes = static_cast<BYTE*>(damage);
    for (unsigned i = 0; i < 6; ++i) {
        const int original = *reinterpret_cast<int*>(bytes + components[i]);
        if (original < 0) return;
        const int64_t value = (int64_t(original) * zoneSlots[level].configuredRow.wMonLvl[0] +
            nativeLevel / 2) / nativeLevel;
        if (value > INT_MAX) return;
        scaled[i] = static_cast<int>(value);
    }
    if (!runDamageNativeLevel || (projectile && !runDamageMissile))
        for (unsigned i = 0; i < 6; ++i) {
            const int original = *reinterpret_cast<int*>(bytes + components[i]);
            if (original > 0 && original != scaled[i]) {
                runDamageOriginal = original;
                runDamageApplied = scaled[i];
                runDamageComponent = static_cast<unsigned>(components[i]);
                runDamageNativeLevel = nativeLevel;
                runDamageMissile = projectile;
                break;
            }
        }
    if (projectile && sourceId == runBossDamageSources[0].id) {
        ++runBossProjectileHits;
        if (!runBossProjectileOriginal)
            for (unsigned i = 0; i < 6; ++i) {
                const int original = *reinterpret_cast<int*>(bytes + components[i]);
                if (original > 0 && original != scaled[i]) {
                    runBossProjectileOriginal = original;
                    runBossProjectileApplied = scaled[i];
                    runBossProjectileComponent = static_cast<unsigned>(components[i]);
                    break;
                }
            }
    }
    for (unsigned i = 0; i < 6; ++i)
        *reinterpret_cast<int*>(bytes + components[i]) = scaled[i];
    ++runScaledHits;
    if (projectile) ++runScaledMissileHits;
}
enum class KillLootClaim { Added, Duplicate, Full };
KillLootClaim ClaimKillLoot(DWORD unitId) {
    constexpr size_t capacity = 2048;
    constexpr size_t mask = capacity - 1;
    const std::uint64_t key = static_cast<std::uint64_t>(unitId) + 1;
    size_t slot = (static_cast<std::uint64_t>(unitId) * 2654435761u) & mask;
    for (size_t probe = 0; probe < capacity; ++probe) {
        std::uint64_t& entry = runKillLootLedger[slot];
        if (entry == key) return KillLootClaim::Duplicate;
        if (!entry) { entry = key; return KillLootClaim::Added; }
        slot = (slot + 1) & mask;
    }
    return KillLootClaim::Full;
}
bool DropBossCorruptedUniques(void* game, UnitAny* player, UnitAny* boss,
    int itemLevel, int depth, std::string& error) {
    const int playerLevel = MarketCharacterLevel();
    if (playerLevel < 1 || playerLevel > 99) {
        error = "boss unique rewards have no valid character level";
        return false;
    }
    std::array<int, RoguelikeLoot::BossCorruptedUniqueCount()> uniqueIds{};
    for (unsigned index = 0; index < uniqueIds.size(); ++index) {
        const DWORD rewardSeed = GetTickCount() ^ boss->dwUnitId ^
            (depth * 2654435761u) ^ (0x9e3779b9u * (index + 1));
        const json reward = DropBossCorruptedUnique(game, player,
            itemLevel, playerLevel, rewardSeed, boss,
            uniqueIds.data(), index);
        if (!reward.value("ok", false)) {
            error = "boss corrupted unique " + std::to_string(index + 1) +
                ": " + reward.value("error", "reward generation failed");
            return false;
        }
        if (!reward.contains("item_id") ||
            !reward["item_id"].is_number_unsigned() ||
            !reward.contains("unique_id") ||
            !reward["unique_id"].is_number_integer() ||
            !reward.value("corrupted", false) ||
            reward.value("quality", 0) != 7 ||
            reward.value("item_level", 0) != itemLevel ||
            !reward.contains("corruption") ||
            !reward["corruption"].is_object()) {
            error = "boss corrupted unique " + std::to_string(index + 1) +
                ": native reward result failed verification";
            return false;
        }
        uniqueIds[index] = reward["unique_id"].get<int>();
    }
    return true;
}
void OnNaturalKill(void* game, UnitAny* defender) {
    if (runGame.load(std::memory_order_acquire) != game || !game || !runPlayer ||
        !defender || defender->dwType != UNIT_MONSTER ||
        !Readable(defender->pPath, sizeof(Path)) ||
        !Readable(defender->pPath->pRoom1, sizeof(Room1)) ||
        !Readable(defender->pPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(defender->pPath->pRoom1->pRoom2->pLevel, sizeof(Level)))
        return;
    const int level = runStage.load(std::memory_order_acquire);
    if (level < 13 || level > 254 || runCleared[level] ||
        defender->pPath->pRoom1->pRoom2->pLevel->dwLevelNo != static_cast<DWORD>(level) ||
        !PortalArea(runPlayer, level) ||
        !ObservationTelemetry::MatchesContext(runEpoch, game, runPlayer))
        return;
    if (runEncounters[level] == 3 ||
        (ExtraBossCount(zoneSlots[level].affixes) && !runBossUnit[level]) ||
        !runError.empty())
        return;
    if (BossZone(zoneSlots[level].kind))
        runBossKillWitness.store(true, std::memory_order_release);
    const int monsterLevel = level == 20 ? 1 :
        zoneSlots[level].configured ?
            zoneSlots[level].configuredRow.wMonLvl[0] : 0;
    const int itemLevel = RoguelikeLoot::RewardItemLevel(monsterLevel,
        zoneSlots[level].depth, zoneSlots[level].testOnly);
    if (itemLevel < 1 || itemLevel > 99) {
        runError = "monster loot has no valid zone item level";
        return;
    }
    if (runKillLootLedgerLevel != level) {
        runKillLootLedger.fill(0);
        runKillLootLedgerLevel = level;
    }
    const KillLootClaim claim = ClaimKillLoot(defender->dwUnitId);
    if (claim == KillLootClaim::Full) {
        runError = "per-zone monster loot deduplication ledger is full";
        return;
    }
    if (claim == KillLootClaim::Duplicate) {
        TryCompleteClear(game, level);
        return;
    }
    if (BossZone(zoneSlots[level].kind) && !runBossUnit[level]) {
        runError = "boss zone primary identity is unavailable";
        return;
    }
    if (!DropMonsterLoot(game, runPlayer, defender, itemLevel,
            zoneSlots[level].depth, zoneSlots[level].reward))
        ++runDropFailureCount;
    if (BossZone(zoneSlots[level].kind) &&
        defender->dwUnitId == runBossUnit[level]) {
        std::string error;
        if (!DropBossCorruptedUniques(game, runPlayer, defender,
                itemLevel, level, error)) {
            runError = error;
            return;
        }
    }
    TryCompleteClear(game, level);
}
json DropFocusedClearPrimary(void* game, UnitAny* player, int itemLevel,
    int playerLevel, const ZoneSlot& zone, Pending::RewardFocus focus,
    DWORD seed) {
    if (focus == Pending::RewardFocus::Jewelry)
        return DropRunCategory(game, player, nullptr, itemLevel, playerLevel,
            RoguelikeLoot::BaseKind::Jewelry, seed);
    if (focus == Pending::RewardFocus::Charms)
        return DropRunCategory(game, player, nullptr, itemLevel, playerLevel,
            RoguelikeLoot::BaseKind::Charm, seed);
    const bool sockets = focus == Pending::RewardFocus::Socketables;
    const int quality = focus == Pending::RewardFocus::Unique ? 7 :
        focus == Pending::RewardFocus::Set ? 5 :
        sockets ? 2 :
        zone.affixes.quality == RoguelikeAffixes::Quality::Magic ? 4 :
        zone.affixes.quality == RoguelikeAffixes::Quality::Rare ? 6 : 2;
    return DropRunReward(game, player, itemLevel, playerLevel,
        quality, false, seed, sockets ? 1 : 0);
}
bool AwardFocusedClearLoot(void* game, UnitAny* player, int level, std::string& error) {
    const ZoneSlot& zone = zoneSlots[level];
    if (!zone.configured || runClearRewardIds[level][0]) return false;
    const int playerLevel = MarketCharacterLevel();
    const int itemLevel = RoguelikeLoot::RewardItemLevel(
        zone.configuredRow.wMonLvl[0], zone.depth, zone.testOnly);
    const DWORD seed = GetTickCount() ^ (level * 2654435761u);
    const auto focus = zone.reward;
    const RoguelikeLoot::MonsterDropPlan plan =
        RoguelikeLoot::PlanMonsterDrops(focus, 3);
    const json first = DropFocusedClearPrimary(game, player, itemLevel,
        playerLevel, zone, focus, seed);
    if (!MonsterDropMatches(plan.primary.kind, first)) {
        error = "primary reward did not match focus: " +
            first.value("error", first.dump());
        return false;
    }
    runClearRewardIds[level][0] = first["item_id"].get<DWORD>();
    if (!plan.secondary.count) return true;
    const json second = DropRunSocketable(game, player, nullptr, itemLevel,
        playerLevel, static_cast<unsigned>(seed % 3u), seed ^ 0x9e3779b9u);
    if (!MonsterDropMatches(RoguelikeLoot::DropKind::Socketable, second)) {
        error = "socket material reward did not match focus: " +
            second.value("error", second.dump());
        return false;
    }
    runClearRewardIds[level][1] = second["item_id"].get<DWORD>();
    return true;
}
void TryCompleteClear(void* game, int level) {
    if (!runError.empty() || runCleared[level]) return;
    ServerMonsters::Request request;
    request.operation = ServerMonsters::Operation::Area;
    json census = ServerMonsters::Execute(game, runPlayer, request);
    if (!census.value("ok", false) || !census.value("room_catalog_complete", false) ||
        !census.value("population_complete", false) ||
        census.value("level_id", 0) != level || !census.contains("enemies") ||
        !census["enemies"].is_array()) {
        runError = "natural clear census incomplete after witnessed kill";
        runBossKillWitness.store(false, std::memory_order_release);
        return;
    }
    if (census.value("inactive_hostile_alive_count", -1) != 0) return;
    for (const auto& monster : census["enemies"])
        if (!monster.value("dead", false)) return;
    // Witnessed nonzero death and complete active/inactive zero, not an
    // empty-on-entry area or an inferred last-kill count.
    if (census["enemies"].empty()) {
        runError = "empty area cannot award a witnessed clear";
        return;
    }
    if (level == 20 && !runStarterRewardId) {
        const json reward = DropRunReward(game, runPlayer, 1,
            MarketCharacterLevel(), 2, true,
            GetTickCount() ^ census["enemies"][0].value("id", 0u));
        if (!reward.value("ok", false) || reward.value("quality", 0) != 2 ||
            !reward.contains("item_id") || !reward["item_id"].is_number_unsigned()) {
            runError = "guaranteed starter white gear did not materialize";
            return;
        }
        runStarterRewardId = reward["item_id"].get<DWORD>();
    }
    if (level != 20) {
        std::string rewardError;
        if (!AwardFocusedClearLoot(game, runPlayer, level, rewardError)) {
            runError = "native clear reward did not materialize as offered: " + rewardError;
            return;
        }
    }
    runCleared[level] = true;
    runBossKillWitness.store(false, std::memory_order_release);
    if (!CharacterState::AwardClearLevel(game, runPlayer)) {
        runError = "native clear level award failed";
        return;
    }
    if (!zoneSlots[level].testOnly)
        OfferChoices(game, level);
}
void RequestRunAuraPulse() {
    if (!runGame.load(std::memory_order_acquire)) return;
    const RunAuraCastState castState =
        runAuraCastState.load(std::memory_order_acquire);
    if (castState == RunAuraCastState::Idle &&
        !runAuraPending.load(std::memory_order_acquire))
        return;
    if (castState == RunAuraCastState::Send) {
        const WORD skill = runAuraCastSkill.load(std::memory_order_relaxed);
        const DWORD targetId = runAuraCastTarget.load(std::memory_order_relaxed);
        const DWORD targetType =
            runAuraCastTargetType.load(std::memory_order_relaxed);
        BYTE select[9] = {0x3C, static_cast<BYTE>(skill),
            static_cast<BYTE>(skill >> 8), 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
        BYTE use[9] = {0x0D};
        memcpy(use + 1, &targetType, sizeof(targetType));
        memcpy(use + 5, &targetId, sizeof(targetId));
        const bool sent = D2NET_SendPacket(sizeof(select), 0, select) != 0 &&
            D2NET_SendPacket(sizeof(use), 0, use) != 0;
        runAuraCastState.store(sent ? RunAuraCastState::Sent :
            RunAuraCastState::Failed, std::memory_order_release);
    }
    static DWORD nextAttempt = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - nextAttempt) < 0) return;
    nextAttempt = now + 100;
    BYTE packet[16] = {0x15, 1, 0};
    memcpy(packet + 3, "run aura", sizeof("run aura") - 1);
    D2NET_SendPacket(sizeof(packet), 0, packet);
}
void SettleRunFrame() {
    HandleRunDeath();
    RequestRunAuraPulse();
    const int level = runStage.load(std::memory_order_acquire);
    if (runResume.active) {
        void* game = runGame.load(std::memory_order_acquire);
        UnitAny* player = runPlayer;
        if (game && PortalArea(player, level))
            ContinueRunResume(game, player);
        return;
    }
    void* game = runGame.load(std::memory_order_acquire);
    UnitAny* player = runPlayer;
    if (game && player && level >= 13 && level <= 254 && level != 20 &&
        zoneSlots[level].configured && !runPopulationReady[level] &&
        runError.empty() && PortalArea(player, level)) {
        static DWORD nextPopulationAttempt = 0;
        const DWORD now = GetTickCount();
        if (static_cast<LONG>(now - nextPopulationAttempt) >= 0) {
            nextPopulationAttempt = now + 500;
            ServerMonsters::Request request;
            request.operation = ServerMonsters::Operation::Area;
            const json census = ServerMonsters::Execute(game, player, request);
            if (census.value("ok", false) &&
                census.value("level_id", 0) == level &&
                census.value("room_catalog_complete", false) &&
                census.value("population_complete", false))
                OnPopulationCompleted(level);
        }
    }
    const bool chestPending = level >= 13 && level <= 254 &&
        zoneSlots[level].configured &&
        zoneSlots[level].kind == Pending::ZoneKind::Chest &&
        runError.empty() &&
        (!runChestSettled[level][0] || !runChestSettled[level][1] ||
         !runChestSettled[level][2]);
    if (!runGame.load(std::memory_order_acquire) ||
        (level == 20 ? runStarterSettled.load(std::memory_order_acquire) :
            level < 13 || level > 254 ||
            (!runBossKillWitness.load(std::memory_order_acquire) && !chestPending))) return;
    if (!PortalArea(player, level)) return;
    // An empty starter room may skip the population callback; a boss's
    // final death can outlive the last kill-hook census. Recheck on the server.
    static DWORD nextAttempt = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(now - nextAttempt) < 0) return;
    nextAttempt = now + 500;
    BYTE packet[16] = {0x15, 1, 0};
    memcpy(packet + 3, "run settle", 10);
    D2NET_SendPacket(sizeof(packet), 0, packet);
}
void RestoreRoguelikeZones(bool saveSnapshot) {
    void* previousGame = runGame.load(std::memory_order_acquire);
    UnitAny* previousPlayer = runPlayer;
    if (!ClearRunAuraSkill(previousGame, previousPlayer)) {
        OutputDebugStringA("Roguelike game teardown could not verify temporary aura skill cleanup\n");
        // Keep the static Skill storage quarantined; UseAuraItem rejects reuse while it may still be linked to the old player.
    }
    json snapshotStatus = {{"attempted", false},
        {"root_destination", runRouteNext[1]},
        {"active_level", runStage.load(std::memory_order_acquire)},
        {"route_edge_count", 0}};
    snapshotStatus["run_serial"] = runActiveSerial;
    int routeEdgeCount = 0;
    for (int from = 1; from < 255; ++from)
        if (runRouteNext[from] >= 0) ++routeEdgeCount;
    snapshotStatus["route_edge_count"] = routeEdgeCount;
    snapshotStatus["natural_drop_failures"] = runDropFailureCount;
    snapshotStatus["last_natural_drop_error"] = runDropError;
    if (!saveSnapshot) {
        snapshotStatus["reason"] = "softcore_death_reset";
    } else if (runResume.active) {
        snapshotStatus["reason"] = "resume_in_progress; existing snapshot preserved";
    } else if (!previousGame || !previousPlayer || runEpoch.empty()) {
        snapshotStatus["reason"] = "no_active_run_context";
    } else if (runRouteNext[1] != runRootLevel ||
        (runRootLevel != 20 && !IsRunArenaLevel(runRootLevel))) {
        snapshotStatus["reason"] = "town-to-run-root route missing";
    } else {
        snapshotStatus["attempted"] = true;
        std::string saveError;
        const bool saved = SaveRunSnapshot(previousGame, previousPlayer, saveError);
        snapshotStatus["saved"] = saved;
        if (!saved) snapshotStatus["error"] = saveError;
        if (!saved)
            OutputDebugStringA(("Roguelike route snapshot not saved: " +
                saveError + "\n").c_str());
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        lastRunSnapshotStatus = std::move(snapshotStatus);
    }
    runGame.store(nullptr, std::memory_order_release);
    runDeathHandled.store(false, std::memory_order_release);
    runStarterSettled.store(false, std::memory_order_release);
    runBossKillWitness.store(false, std::memory_order_release);
    CombatTelemetry::StopRoguelikeKills();
    runStage.store(0, std::memory_order_release);
    runRootLevel = 0;
    // A new run gets fresh area IDs; resume restores the saved path below.
    runAreaUsageGame = nullptr;
    runAreaUsageEpoch.clear();
    runUsedAreas.fill(false);
    runBossDamageCount = 0;
    runObservedMissileDamage = 0;
    runOtherAttackerDamage = 0;
    runMissileDamageSample = {};
    runArrivalLevel = 0;
    runPlayer = nullptr;
    armedReroll = {};
    runEpoch.clear();
    runCharacterName.clear();
    runActiveSerial = 0;
    runDropError.clear();
    runDropFailureCount = 0;
    runError.clear();
    runPopulationReady.fill(false);
    runRetiredDestinations.fill(false);
    runEncounters.fill(0);
    runBossUnit.fill(0);
    runShrineIds.fill({});
    runClearRewardIds.fill({});
    runStarterRewardId = 0;
    naturalEquipmentCalls = naturalEquipmentForced = naturalEquipmentRejected = 0;
    naturalEquipmentQuality = naturalEquipmentBase = 0;
    observedAkaraPerms = observedAkaraPotions = 0;
    observedAkaraTrade = false;
    filteredAkaraPotions = 0;
    runChestIds.fill({});
    runChestRewardIds.fill({});
    runChestRewardCounts.fill({});
    runChestUniqueIds.fill({});
    runChestUniqueCounts.fill(0);
    runRouteNext.fill(-1);
    runRouteParent.fill(-1);
    runCleared.fill(false);
    runResume = {};
    {
        std::lock_guard<std::mutex> lock(mutex);
        choiceNames.clear();
        selectedTriggerEpochBySource.clear();
    }
    ServerMonsters::Clear();
    std::string error;
    if (!ResetZoneRows(error))
        OutputDebugStringA(("Roguelike cave TXT restoration rejected: " + error + "\n").c_str());
    if (saveSnapshot) ForgetRunOwnedObjects();
}
void RestoreRoguelikeZones() {
    RestoreRoguelikeZones(true);
}
void HandleRunDeath() {
    void* game = runGame.load(std::memory_order_acquire);
    UnitAny* player = runPlayer;
    if (!game || !Readable(player, sizeof(UnitAny))) return;
    const bool dead = player->dwMode == PLAYER_MODE_DEATH ||
        player->dwMode == PLAYER_MODE_DEAD;
    if (!dead) {
        runDeathHandled.store(false, std::memory_order_release);
        return;
    }
    if (runDeathHandled.exchange(true, std::memory_order_acq_rel)) return;
    const auto mode = CharacterState::GetDeathMode(player);
    if (mode == CharacterState::DeathMode::Hardcore) return;
    if (mode != CharacterState::DeathMode::Softcore) {
        OutputDebugStringA("Roguelike death reset skipped: character mode unavailable\n");
        return;
    }
    std::string error;
    if (!DeleteRunSnapshot(player, error)) {
        runError = "death reset could not remove run snapshot: " + error;
        OutputDebugStringA(("Roguelike " + runError + "\n").c_str());
        return;
    }
    const bool reset = CharacterState::ResetToLevelOne(game, player, error);
    if (!reset)
        OutputDebugStringA(("Roguelike Softcore death reset failed: " +
            error + "\n").c_str());
    const json portalCleanup = RemoveRunOwnedObjects(game, player);
    if (!portalCleanup.value("ok", false))
        OutputDebugStringA(("Roguelike death portal cleanup uncertain: " +
            portalCleanup.dump() + "\n").c_str());
    RestoreRoguelikeZones(false);
    {
        std::lock_guard<std::mutex> lock(mutex);
        lastRunSnapshotStatus["death_reset"] = reset;
        lastRunSnapshotStatus["portal_cleanup"] = portalCleanup;
        if (!reset) lastRunSnapshotStatus["death_reset_error"] = error;
    }
}
json RunSnapshotStatus() {
    std::lock_guard<std::mutex> lock(mutex);
    return lastRunSnapshotStatus;
}
void ShutdownMarketPriceHook() {
    RestoreMarketPriceIatsOrLog();
}
void EnsureInstalled() {
    if (unitNameInstalled &&
        (!clientMarketPriceIat.installed || !serverMarketPriceIat.installed)) {
        std::string priceError;
        if (!EnsureMarketPriceIats(priceError))
            OutputDebugStringA(("Roguelike market-price hook not installed: " +
                priceError + "\n").c_str());
    }
    if (originalChat && hookPairReady.load(std::memory_order_acquire)) return;
    std::string error;
    if (!originalChat && !Install(error))
        OutputDebugStringA(("Native chat hook not installed: " + error + "\n").c_str());
    else if (!InstallClick(error))
        OutputDebugStringA(("Native item-use/click hooks not installed: " + error + "\n").c_str());
}
json Request(const json& args) {
    if (!args.is_object() || !args.contains("request_id") || !args["request_id"].is_string()) return Failure("request_id is required");
    std::string id = args["request_id"].get<std::string>();
    if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return Failure("request_id must be 32 lowercase hex characters");
    std::string fingerprint = "item:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    if (!IsGameReady()) return Failure("game is not ready");
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || player->dwType != UNIT_PLAYER) return Failure("player unavailable");
    auto pending = std::make_shared<Pending>(); pending->id = id; pending->fingerprint = std::move(fingerprint); pending->playerId = player->dwUnitId;
    if (args.contains("expected_epoch")) {
        if (!args["expected_epoch"].is_string() || args["expected_epoch"].get_ref<const std::string&>().empty())
            return Failure("invalid expected_epoch");
        pending->expectedEpoch = args["expected_epoch"].get_ref<const std::string&>();
    }
    if (!args.contains("quality") || !args["quality"].is_string()) return Failure("quality is required");
    std::string quality = args["quality"].get<std::string>();
    if (quality == "normal") pending->quality = 2;
    else if (quality == "magic") pending->quality = 4;
    else if (quality == "rare") pending->quality = 6;
    else if (quality == "unique") pending->quality = 7;
    else if (quality == "set") pending->quality = 5;
    else return Failure("quality must be normal, magic, rare, unique or set");
    int64_t number;
    if (args.contains("item_level")) { if (!Integer(args["item_level"], 1, 99, number)) return Failure("item_level must be 1..99"); pending->level = static_cast<int>(number); }
    if (args.contains("seed")) { if (!Integer(args["seed"], 0, UINT32_MAX, number)) return Failure("seed must be uint32"); pending->seed = static_cast<DWORD>(number); }
    sgptDataTable* all = Tables();
    if (!all) return Failure("loaded item tables unavailable");
    std::string code;
    if (args.contains("code")) { if (!args["code"].is_string()) return Failure("code must be an exact base code"); code = args["code"].get<std::string>(); }
    if (pending->quality == 7) {
        if (!all->dwUniqItemsRecs || all->dwUniqItemsRecs > 32768 ||
            !Readable(all->pUniqueItemsTxt, all->dwUniqItemsRecs * sizeof(UniqueItemsTxt))) return Failure("unique table unavailable");
        if (args.contains("unique_id") == args.contains("unique_name")) return Failure("exactly one unique_id or unique_name is required");
        if (args.contains("unique_id")) {
            if (!Integer(args["unique_id"], 0, all->dwUniqItemsRecs - 1, number)) return Failure("unique_id is outside loaded table");
            pending->uniqueId = static_cast<int>(number);
        } else {
            if (!args["unique_name"].is_string()) return Failure("unique_name must be an exact table key");
            std::string name = args["unique_name"].get<std::string>();
            for (DWORD i = 0; i < all->dwUniqItemsRecs; ++i) {
                const auto& record = all->pUniqueItemsTxt[i];
                if (name == std::string(record.szName, strnlen(record.szName, sizeof(record.szName)))) {
                    if (pending->uniqueId >= 0) return Failure("unique name is ambiguous; use unique_id");
                    pending->uniqueId = static_cast<int>(i);
                }
            }
            if (pending->uniqueId < 0) return Failure("unique_name not found in loaded table");
        }
        const auto& unique = all->pUniqueItemsTxt[pending->uniqueId];
        std::string expected = GetItemCode(unique.szCode);
        if (!code.empty() && code != expected) return Failure("unique base code does not match");
        if (!(unique.dwUniqueItemFlags & 1) || unique.wLvl > pending->level) return Failure("unique is disabled or above requested item_level");
        code = std::move(expected);
    } else if (args.contains("unique_id") || args.contains("unique_name")) return Failure("unique identity requires unique quality");
    if (pending->quality == 5) {
        if (args.contains("set_item_id") == args.contains("set_item_name"))
            return Failure("exactly one set_item_id or set_item_name is required");
        if (!all->dwSetItemsRecs || all->dwSetItemsRecs > 32768 ||
            !Readable(all->pSetItemsTxt, size_t(all->dwSetItemsRecs) * sizeof(SetItemsTxt)) ||
            !all->dwSetsRecs || all->dwSetsRecs > 32768 ||
            !Readable(all->pSetsTxt, size_t(all->dwSetsRecs) * sizeof(SetsTxt)))
            return Failure("set tables unavailable");
        if (args.contains("set_item_id")) {
            if (!Integer(args["set_item_id"], 0, all->dwSetItemsRecs - 1, number))
                return Failure("set_item_id is outside loaded table");
            pending->setId = static_cast<int>(number);
        } else {
            if (!args["set_item_name"].is_string()) return Failure("set_item_name must be an exact table key");
            const std::string name = args["set_item_name"].get<std::string>();
            if (name.empty()) return Failure("set_item_name must be an exact table key");
            for (DWORD i = 0; i < all->dwSetItemsRecs; ++i) {
                const auto& record = all->pSetItemsTxt[i];
                if (name == std::string(record.szName, strnlen(record.szName, sizeof(record.szName)))) {
                    if (pending->setId >= 0) return Failure("set item name is ambiguous; use set_item_id");
                    pending->setId = static_cast<int>(i);
                }
            }
            if (pending->setId < 0) return Failure("set_item_name not found in loaded table");
        }
        const auto& piece = all->pSetItemsTxt[pending->setId];
        if (piece.nSetId >= all->dwSetsRecs ||
            piece.wLvl > pending->level) return Failure("set item unavailable at requested item_level");
        const std::string expected = GetItemCode(piece.szCode);
        if (!code.empty() && code != expected) return Failure("set item base code does not match");
        code = expected;
    } else if (args.contains("set_item_id") || args.contains("set_item_name"))
        return Failure("set item identity requires set quality");
    D2ItemDataTbl* bases = D2COMMON_10535_DATATBLS_GetItemDataTables();
    if (!bases || bases->nItemsTxtRecordCount <= 0 || bases->nItemsTxtRecordCount > 32768 ||
        !Readable(bases->pItemsTxt, bases->nItemsTxtRecordCount * sizeof(ItemsTxt))) return Failure("base table unavailable");
    for (int i = 0; i < bases->nItemsTxtRecordCount; ++i) if (Code(bases->pItemsTxt[i]) == code) {
        if (pending->baseId >= 0) return Failure("base code is ambiguous"); pending->baseId = i;
    }
    if (pending->baseId < 0 || !bases->pItemsTxt[pending->baseId].bspawnable) return Failure("base code not found or not spawnable");
    auto affixes = [&](const char* key, std::array<int,3>& output, bool prefix) -> bool {
        if (!args.contains(key)) return true;
        if (!args[key].is_array() || args[key].size() > (pending->quality == 4 ? 1u : 3u) ||
            (pending->quality != 4 && pending->quality != 6)) return false;
        D2MagicAffixDataTbl* table = D2COMMON_10492_DATATBLS_GetMagicAffixDataTables();
        if (!table || table->nMagicAffixTxtRecordCount <= 0 || table->nMagicAffixTxtRecordCount > 32768) return false;
        for (size_t i = 0; i < args[key].size(); ++i) {
            int64_t value;
            if (!Integer(args[key][i], 1, table->nMagicAffixTxtRecordCount, value)) return false;
            const AutoMagicTxt* record = table->pMagicAffixTxt + value - 1;
            if (!Readable(record,sizeof(*record)) || !record->wSpawnable ||
                (pending->quality == 6 && !record->nRare) ||
                (prefix ? (record < table->pMagicPrefix || record >= table->pAutoMagic) : (record < table->pMagicSuffix || record >= table->pMagicPrefix))) return false;
            output[i] = static_cast<int>(value);
        }
        return true;
    };
    if (!affixes("prefixes", pending->prefixes, true) || !affixes("suffixes", pending->suffixes, false)) return Failure("affixes must be compatible loaded prefix/suffix spawn IDs of the requested quality");
    pending->exactAffixes = args.contains("prefixes") || args.contains("suffixes");
    if (args.contains("stats")) {
        if (!args["stats"].is_array() || args["stats"].size() > 32 || !all->dwItemStatCostRecs || all->dwItemStatCostRecs > 4096 ||
            !Readable(all->pItemStatCostTxt, all->dwItemStatCostRecs * sizeof(ItemStatCostTxt))) return Failure("invalid or unavailable property specification");
        for (const auto& value : args["stats"]) {
            int64_t stat, layer = 0, amount;
            if (!value.is_object() || !value.contains("stat_id") || !value.contains("value") ||
                !Integer(value["stat_id"], 0, all->dwItemStatCostRecs - 1, stat) ||
                !Integer(value["value"], INT32_MIN, INT32_MAX, amount) ||
                (value.contains("layer") && !Integer(value["layer"], 0, UINT16_MAX, layer))) return Failure("invalid property stat, layer or value");
            const auto& cost = all->pItemStatCostTxt[stat];
            if (!cost.bSaveBits || cost.bSaveBits > 32 || cost.bEncode || cost.bValShift ||
                cost.dwSaveParamBits > 16 || (!cost.dwSaveParamBits && layer) ||
                (cost.dwSaveParamBits && uint64_t(layer) >= (uint64_t(1) << cost.dwSaveParamBits))) return Failure("property needs an unsupported encoded/scaled representation");
            int64_t encoded = amount + cost.dwSaveAdd;
            if (encoded < 0 || uint64_t(encoded) >= (uint64_t(1) << cost.bSaveBits)) return Failure("property value is not representable by the loaded item save format");
            for (const auto& prior : pending->properties) if (prior.id == stat && prior.layer == layer) return Failure("duplicate property stat/layer");
            pending->properties.push_back({static_cast<int>(stat), static_cast<int>(amount), static_cast<WORD>(layer)});
        }
    }
    return Submit(pending);
}
json StartRoguelikeRun(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 2 || !PortalNonce(args, id, epoch))
        return Failure("request_id and current expected_epoch required");
    const std::string fingerprint = "roguelike-start:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!PortalArea(player, 1)) return Failure("start requires Rogue Encampment");
    auto pending = std::make_shared<Pending>();
    pending->id = id;
    pending->fingerprint = fingerprint;
    pending->expectedEpoch = epoch;
    pending->playerId = player->dwUnitId;
    pending->runStart = true;
    if (!ReservePortalNonce(id, fingerprint, {{"ok", false}, {"status", "unconfirmed"},
        {"request_id", id}})) return Failure("start nonce capacity reached");
    json receipt = Submit(pending);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending->completed && !pending->result.is_null())
            portalReceipts.at(id).second = pending->result;
        else portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json ArmRoguelikeRun(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 2 || !PortalNonce(args, id, epoch))
        return Failure("request_id and current expected_epoch required");
    const std::string fingerprint = "roguelike-arm:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!PortalArea(player, 20)) return Failure("arm requires the Tower arena");
    auto pending = std::make_shared<Pending>();
    pending->id = id;
    pending->fingerprint = fingerprint;
    pending->expectedEpoch = epoch;
    pending->playerId = player->dwUnitId;
    pending->runArm = true;
    if (!ReservePortalNonce(id, fingerprint, {{"ok", false}, {"status", "unconfirmed"},
        {"request_id", id}})) return Failure("arm nonce capacity reached");
    json receipt = Submit(pending);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending->completed && !pending->result.is_null())
            portalReceipts.at(id).second = pending->result;
        else portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
Pending::Zone ExplicitZone(int area, int monsterClass, int monsterLevel,
    int density, int magicFind) {
    Pending::Zone zone{area, monsterClass, monsterLevel, density, magicFind};
    zone.monsterClasses[0] = monsterClass;
    zone.monsterClassCount = 1;
    if (magicFind) {
        zone.affixes.magicFind = magicFind;
        zone.affixes.stats[zone.affixes.statCount++] = {370, 0, 1080, magicFind};
    }
    return zone;
}
bool TestArenaLevel(int level) {
    return (level >= 13 && level <= 16) || (level >= 138 && level <= 254);
}
bool TestZoneInteger(const json& args, const char* key, int64_t minimum,
    int64_t maximum, int64_t& value) {
    return args.contains(key) && Integer(args.at(key), minimum, maximum, value);
}
bool ParseTestZoneFields(const json& args, int64_t& monsterClass,
    int64_t& monsterLevel, int64_t& density, int64_t& magicFind,
    int64_t& runLevel, std::string& error) {
    if (!TestZoneInteger(args, "monster_class", 0, 32767, monsterClass)) {
        error = "invalid test-zone monster class";
        return false;
    }
    if (!TestZoneInteger(args, "monster_level", 1, 99, monsterLevel)) {
        error = "invalid test-zone monster level";
        return false;
    }
    if (!TestZoneInteger(args, "density", 0, 4480, density)) {
        error = "invalid test-zone density";
        return false;
    }
    if (!TestZoneInteger(args, "magic_find", 0, 300, magicFind)) {
        error = "invalid test-zone magic find";
        return false;
    }
    runLevel = 0;
    if (args.contains("run_level") &&
        !TestZoneInteger(args, "run_level", 1, 99, runLevel)) {
        error = "invalid test-zone run level";
        return false;
    }
    return true;
}
bool ParseTestZoneKind(const json& args, Pending::ZoneKind& kind,
    std::string& error) {
    std::string type = "monster";
    if (args.contains("zone_type")) {
        if (!args["zone_type"].is_string()) {
            error = "zone_type must be a supported string";
            return false;
        }
        type = args["zone_type"].get<std::string>();
    }
    if (type == "monster") kind = Pending::ZoneKind::Monster;
    else if (type == "boss") kind = Pending::ZoneKind::Boss;
    else if (type == "merchant") kind = Pending::ZoneKind::Merchant;
    else if (type == "shrine") kind = Pending::ZoneKind::Shrine;
    else if (type == "chest") kind = Pending::ZoneKind::Chest;
    else {
        error = "zone_type must be monster, boss, merchant, shrine or chest";
        return false;
    }
    return true;
}
bool ParseTestZoneRewardFocus(const json& args, Pending::ZoneKind kind,
    Pending::RewardFocus& reward, std::string& error) {
    reward = Pending::RewardFocus::Equipment;
    if (!args.contains("reward_focus")) return true;
    const json& value = args["reward_focus"];
    if ((kind != Pending::ZoneKind::Monster &&
         !BossZone(kind)) || !value.is_string()) {
        error = "reward_focus is supported only for combat test zones";
        return false;
    }
    if (value == "equipment") reward = Pending::RewardFocus::Equipment;
    else if (value == "socketables") reward = Pending::RewardFocus::Socketables;
    else if (value == "unique") reward = Pending::RewardFocus::Unique;
    else if (value == "set") reward = Pending::RewardFocus::Set;
    else if (value == "jewelry") reward = Pending::RewardFocus::Jewelry;
    else if (value == "charms") reward = Pending::RewardFocus::Charms;
    else {
        error = "unsupported test-zone reward_focus";
        return false;
    }
    return true;
}
bool ParseTestZoneSuperunique(const json& args, Pending::ZoneKind kind,
    int64_t& superuniqueId, std::string& error) {
    superuniqueId = -1;
    if (!args.contains("boss_superunique_id")) return true;
    if (kind != Pending::ZoneKind::Boss ||
        !TestZoneInteger(args, "boss_superunique_id", 0, 32767, superuniqueId)) {
        error = "boss_superunique_id requires a boss zone and valid catalog ID";
        return false;
    }
    return true;
}
bool ValidateTestZoneContents(Pending::ZoneKind kind, int64_t monsterClass,
    int64_t density, int64_t magicFind, std::string& error) {
    if (kind == Pending::ZoneKind::Monster && density == 0) {
        error = "monster test zones require positive density";
        return false;
    }
    if (kind != Pending::ZoneKind::Monster && density != 0) {
        error = "non-monster test zones require zero density";
        return false;
    }
    const bool noncombat = kind == Pending::ZoneKind::Merchant ||
        kind == Pending::ZoneKind::Shrine || kind == Pending::ZoneKind::Chest;
    if (noncombat && (monsterClass != 5 || magicFind != 0)) {
        error = "noncombat test zones require the native noncombat roster";
        return false;
    }
    return true;
}
bool ParseTestZone(const json& args, int level,
    Pending::Zone& zone, std::string& error) {
    int64_t monsterClass, monsterLevel, density, magicFind, superuniqueId, runLevel;
    if (!ParseTestZoneFields(args, monsterClass, monsterLevel, density,
                             magicFind, runLevel, error)) return false;
    Pending::ZoneKind kind;
    Pending::RewardFocus reward;
    if (!ParseTestZoneKind(args, kind, error)) return false;
    if (!ParseTestZoneRewardFocus(args, kind, reward, error)) return false;
    if (!ParseTestZoneSuperunique(args, kind, superuniqueId, error)) return false;
    if (!ValidateTestZoneContents(kind, monsterClass, density, magicFind, error))
        return false;
    zone = ExplicitZone(level, static_cast<int>(monsterClass),
        static_cast<int>(monsterLevel), static_cast<int>(density),
        static_cast<int>(magicFind));
    if (kind != Pending::ZoneKind::Monster) {
        if (args.contains("monster_classes")) {
            error = "monster_classes is valid only for normal monster zones";
            return false;
        }
        zone.monsterClasses.fill(0);
        zone.monsterClassCount = 0;
    } else if (args.contains("monster_classes")) {
        const json& classes = args["monster_classes"];
        if (!classes.is_array() || classes.empty() ||
            classes.size() > zone.monsterClasses.size()) {
            error = "monster_classes must contain one to four class IDs";
            return false;
        }
        zone.monsterClasses.fill(0);
        zone.monsterClassCount = 0;
        for (const json& value : classes) {
            int64_t classId;
            if (!Integer(value, 0, 32767, classId) ||
                (!zone.monsterClassCount && classId != monsterClass)) {
                error = "monster_classes must start with monster_class and contain valid IDs";
                return false;
            }
            for (unsigned previous = 0; previous < zone.monsterClassCount; ++previous)
                if (zone.monsterClasses[previous] == classId) {
                    error = "monster_classes cannot contain duplicates";
                    return false;
                }
            zone.monsterClasses[zone.monsterClassCount++] =
                static_cast<int>(classId);
        }
    }
    zone.depth = static_cast<int>(runLevel);
    zone.testOnly = true;
    zone.kind = kind;
    zone.reward = reward;
    zone.bossSuperuniqueId = static_cast<int>(superuniqueId);
    if (kind == Pending::ZoneKind::Shrine) {
        zone.otherShrine = 6;
        zone.secondOtherShrine = 7;
    }
    return true;
}
bool ParseTestZoneEndpoints(const json& args, int& from, int& to,
    std::string& error) {
    int64_t source, destination;
    if (!TestZoneInteger(args, "from_level", 13, 254, source) ||
        !TestZoneInteger(args, "to_level", 13, 254, destination)) {
        error = "invalid test-zone source or destination level";
        return false;
    }
    if ((source != 20 && !TestArenaLevel(static_cast<int>(source))) ||
        !TestArenaLevel(static_cast<int>(destination)) || destination == source) {
        error = "only current arena to unused distinct cave choices are permitted";
        return false;
    }
    from = static_cast<int>(source);
    to = static_cast<int>(destination);
    return true;
}
bool ParseTestZoneTrigger(const json& args, std::string& id,
    std::string& epoch, int& from, int& to, Pending::Zone& zone,
    std::string& error) {
    if (!args.is_object() || !PortalNonce(args, id, epoch)) {
        error = "request_id and expected_epoch required";
        return false;
    }
    const size_t expected = 8 + args.contains("zone_type") +
        args.contains("boss_superunique_id") + args.contains("run_level") +
        args.contains("reward_focus") + args.contains("monster_classes");
    if (args.size() != expected) {
        error = "unexpected test-zone arguments";
        return false;
    }
    if (!ParseTestZoneEndpoints(args, from, to, error)) return false;
    return ParseTestZone(args, to, zone, error);
}
json ConfigureRoguelikeZone(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 7 || !PortalNonce(args, id, epoch))
        return Failure("request_id, expected_epoch, cave roll and magic_find required");
    int64_t level, monsterClass, monsterLevel, density, magicFind;
    if (!args.contains("to_level") || !args.contains("monster_class") ||
        !args.contains("monster_level") || !args.contains("density") ||
        !args.contains("magic_find") ||
        !Integer(args["to_level"], 13, 254, level) ||
        !Integer(args["monster_class"], 0, 32767, monsterClass) ||
        !Integer(args["monster_level"], 1, 99, monsterLevel) ||
        !Integer(args["density"], 1, 4480, density) ||
        !Integer(args["magic_find"], 0, 300, magicFind))
        return Failure("invalid arena level, monster class, monster level, density or magic_find");
    const std::string fingerprint = "roguelike-zone:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || player->dwType != UNIT_PLAYER) return Failure("player unavailable");
    auto pending = std::make_shared<Pending>();
    pending->id = id;
    pending->fingerprint = fingerprint;
    pending->expectedEpoch = epoch;
    pending->playerId = player->dwUnitId;
    pending->zoneJob = ExplicitZone(static_cast<int>(level), static_cast<int>(monsterClass),
        static_cast<int>(monsterLevel), static_cast<int>(density), static_cast<int>(magicFind));
    if (!ReservePortalNonce(id, fingerprint, {{"ok", false}, {"status", "unconfirmed"},
            {"request_id", id}})) return Failure("mutation nonce capacity reached");
    json receipt = Submit(pending);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending->completed && !pending->result.is_null())
            portalReceipts.at(id).second = std::move(pending->result);
        else if (!pending->completed) portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json GetRoguelikeLevels(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 2 || !PortalNonce(args, id, epoch))
        return Failure("request_id and current expected_epoch required");
    const std::string fingerprint = "roguelike-levels:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || player->dwType != UNIT_PLAYER) return Failure("player unavailable");
    auto pending = std::make_shared<Pending>();
    pending->id = id;
    pending->fingerprint = fingerprint;
    pending->expectedEpoch = epoch;
    pending->playerId = player->dwUnitId;
    pending->levelInventory = true;
    return Submit(pending);
}
json CreateRoguelikePortal(const json& args) {
    std::string id, epoch;
    if (args.size() != 4 || !PortalNonce(args, id, epoch))
        return Failure("request_id, current expected_epoch and two levels required");
    int64_t from, to;
    if (!args.contains("from_level") || !args.contains("to_level") ||
        !Integer(args["from_level"], 1, 254, from) ||
        !Integer(args["to_level"], 13, 254, to) ||
        !((from == 1 && to == 20) ||
          ((from == 20 || (from >= 13 && from <= 16) ||
            (from >= 138 && from <= 254)) &&
           ((to >= 13 && to <= 16) || (to >= 138 && to <= 254)) && to != from)))
        return Failure("only town to tower or tower/arena to distinct unused arena permitted");
    const std::string fingerprint = "roguelike-portal:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!PortalArea(player, static_cast<int>(from))) return Failure("wrong source area");
    std::string imageError;
    if (!VerifiedPortalEntry(imageError)) return Failure(imageError);
    auto pending = std::make_shared<Pending>();
    pending->id = id;
    pending->fingerprint = fingerprint;
    pending->expectedEpoch = epoch;
    pending->playerId = player->dwUnitId;
    pending->portalJob = std::make_pair(static_cast<int>(from), static_cast<int>(to));
    if (!ReservePortalNonce(id, fingerprint, {{"ok", false}, {"status", "unconfirmed"},
            {"request_id", id}})) return Failure("portal nonce capacity reached");
    RememberOwned(pending);
    json receipt = Submit(pending);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending->completed) {
            if (!pending->result.is_null())
                portalReceipts.at(id).second = std::move(pending->result);
        } else portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json CreateRoguelikeTrigger(const json& args) {
    std::string id, epoch, zoneError;
    int from, to;
    Pending::Zone zone{};
    if (!ParseTestZoneTrigger(args, id, epoch, from, to, zone, zoneError))
        return Failure(zoneError);
    const std::string fingerprint = "roguelike-trigger:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!PortalArea(player, from)) return Failure("wrong source area");
    std::string imageError;
    if (!VerifiedPortalEntry(imageError)) return Failure(imageError);
    if (!InstallChoiceNames(imageError))
        return Failure("client choice labels unavailable: " + imageError);
    auto pending = std::make_shared<Pending>();
    pending->id = id;
    pending->fingerprint = fingerprint;
    pending->expectedEpoch = epoch;
    pending->playerId = player->dwUnitId;
    pending->triggerJob = to;
    pending->triggerSource = from;
    pending->zoneJob = zone;
    if (!ReservePortalNonce(id, fingerprint, {{"ok", false}, {"status", "unconfirmed"},
            {"request_id", id}})) return Failure("trigger nonce capacity reached");
    RememberOwned(pending);
    json receipt = Submit(pending);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending->completed) {
            if (!pending->result.is_null())
                portalReceipts.at(id).second = std::move(pending->result);
        } else portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json InteractRoguelikePortal(const json& args) {
    std::string id, epoch;
    if (args.size() != 4 || !PortalNonce(args, id, epoch))
        return Failure("request_id, current expected_epoch, from_level and object_id required");
    int64_t from, objectId;
    if (!args.contains("from_level") || !args.contains("object_id") ||
        !Integer(args["from_level"], 1, 254, from) ||
        (from != 1 && from != 20 &&
         !((from >= 13 && from <= 16) || (from >= 138 && from <= 254))) ||
        !Integer(args["object_id"], 0, UINT32_MAX, objectId))
        return Failure("invalid source level or portal object ID");
    const std::string fingerprint = "roguelike-interact:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!PortalArea(player, static_cast<int>(from))) return Failure("wrong source area");
    UnitAny* object = D2CLIENT_FindServerSideUnit(static_cast<DWORD>(objectId), UNIT_OBJECT);
    if (!object || object->dwType != UNIT_OBJECT || object->dwUnitId != objectId ||
        object->dwTxtFileNo != 60 || !object->pObjectPath ||
        !object->pObjectPath->pRoom1 || !object->pObjectPath->pRoom1->pRoom2 ||
        !object->pObjectPath->pRoom1->pRoom2->pLevel ||
        object->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo != from)
        return Failure("portal object 60 is not present in the source area");
    if (D2COMMON_GetUnitDistance(player, object) >= 9)
        return Failure("portal is not within interaction range");
    const int destination = CreatedDestination(
        static_cast<DWORD>(objectId), static_cast<int>(from), epoch);
    if (!destination || !TargetUnused(player, destination))
        return Failure("portal destination is not a fresh owned world");
    json receipt = {{"ok", false}, {"status", "unconfirmed"}, {"request_id", id},
        {"object_id", objectId}};
    if (!ReservePortalNonce(id, fingerprint, receipt))
        return Failure("portal interaction nonce capacity reached");
    // Let the normal server packet hook perform the selected transition on
    // the game thread; dispatching originalClick here advances run state
    // before the player has actually changed areas.
    BYTE packet[9] = {0x13, UNIT_OBJECT, 0, 0, 0};
    const DWORD objectNumber = static_cast<DWORD>(objectId);
    memcpy(packet + 5, &objectNumber, sizeof(objectNumber));
    if (D2NET_SendPacket(sizeof(packet), 1, packet))
        receipt = {{"ok", true}, {"status", "requested"}, {"request_id", id},
            {"object_id", objectId}, {"from_level", from},
            {"destination_level", destination}};
    {
        std::lock_guard<std::mutex> lock(mutex);
        portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
// Submit one owned-item packet; the server hook authenticates and consumes it.
json UseRunItem(const json& args, const char code[4], bool allowBelt,
    const char* operation, const char* label) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 4 || !PortalNonce(args, id, epoch))
        return Failure("request_id, expected_epoch, from_level and item_id required");
    int64_t from, itemId;
    if (!args.contains("from_level") || !args.contains("item_id") ||
        !Integer(args["from_level"], 13, 254, from) ||
        !Integer(args["item_id"], 1, UINT32_MAX, itemId))
        return Failure("invalid run area or item ID");
    const std::string fingerprint = std::string("roguelike-use-") +
        operation + ":" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!PortalArea(player, static_cast<int>(from)) ||
        !Readable(player->pInventory, sizeof(Inventory)) ||
        !Readable(player->pPath, sizeof(Path)) ||
        player->pInventory->pOwner != player)
        return Failure("run player or inventory unavailable in source area");
    UnitAny* item = D2CLIENT_FindServerSideUnit(static_cast<DWORD>(itemId), UNIT_ITEM);
    if (!item || item->dwType != UNIT_ITEM || item->dwUnitId != itemId ||
        !Readable(item->pItemData, sizeof(ItemData)) ||
        !OwnedRunItemLocation(player, item, allowBelt) ||
        player->pInventory->pCursorItem)
        return Failure(allowBelt ?
            std::string(label) + " is not owned in inventory or belt" :
            std::string(label) + " is not owned in inventory");
    const bool beltItem = item->dwMode == ITEM_MODE_IN_BELT &&
        item->pItemData->NodePage == NODEPAGE_BELTSLOTS;
    const ItemsTxt* text = D2COMMON_GetItemText(item->dwTxtFileNo);
    const bool codeMatches = Readable(text, sizeof(ItemsTxt)) &&
        (code ? (!memcmp(text->szCode, code, 3) &&
                 (text->szCode[3] == ' ' || text->szCode[3] == '\0')) :
                (allowBelt ?
                    RoguelikeAura::FindByCode(text->szCode) != nullptr :
                    RoguelikeLoot::RerollEffectForCode(text->szCode) !=
                        RoguelikeLoot::RerollEffect::None));
    if (!codeMatches) return Failure(std::string("item is not a ") + label);
    json receipt = {{"ok", false}, {"status", "unconfirmed"},
        {"request_id", id}, {"item_id", itemId}};
    if (!ReservePortalNonce(id, fingerprint, receipt))
        return Failure("item-use nonce capacity reached");
    BYTE packet[13] = {};
    packet[0] = beltItem ? 0x26 : 0x20;
    const DWORD ownedId = static_cast<DWORD>(itemId);
    memcpy(packet + 1, &ownedId, sizeof(ownedId));
    if (!beltItem) {
        const DWORD x = player->pPath->xPos, y = player->pPath->yPos;
        memcpy(packet + 5, &x, sizeof(x));
        memcpy(packet + 9, &y, sizeof(y));
    }
    if (D2NET_SendPacket(sizeof(packet), 1, packet))
        receipt = {{"ok", true}, {"status", "requested"},
            {"request_id", id}, {"item_id", itemId}, {"from_level", from}};
    {
        std::lock_guard<std::mutex> lock(mutex);
        portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json UseRoguelikeScroll(const json& args) {
    return UseRunItem(args, nullptr, false, "scroll", "Portal Reroll consumable");
}
json UseRoguelikeAura(const json& args) {
    return UseRunItem(args, nullptr, true, "aura", "Aura Elixir");
}
json InteractRoguelikeChest(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 4 || !PortalNonce(args, id, epoch))
        return Failure("request_id, expected_epoch, from_level and object_id required");
    int64_t from, objectId;
    if (!args.contains("from_level") || !args.contains("object_id") ||
        !Integer(args["from_level"], 13, 254, from) ||
        !Integer(args["object_id"], 1, UINT32_MAX, objectId))
        return Failure("invalid run area or chest ID");
    const std::string fingerprint = "roguelike-open-chest:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!PortalArea(player, static_cast<int>(from)))
        return Failure("chest is not in the current run area");
    // This RPC runs on the client thread; MatchesContext requires the server
    // callback's player pointer. Bind to its active run by ID and epoch here.
    if (!runGame.load(std::memory_order_acquire) ||
        !Readable(runPlayer, sizeof(UnitAny)) ||
        runPlayer->dwUnitId != player->dwUnitId || runEpoch != epoch ||
        runStage.load(std::memory_order_acquire) != from ||
        zoneSlots[from].kind != Pending::ZoneKind::Chest ||
        !runCleared[from] || !runError.empty() ||
        std::find(runChestIds[from].begin(), runChestIds[from].end(),
            static_cast<DWORD>(objectId)) == runChestIds[from].end())
        return Failure("chest does not belong to this active run");
    UnitAny* object = D2CLIENT_FindServerSideUnit(static_cast<DWORD>(objectId), UNIT_OBJECT);
    if (!object || object->dwType != UNIT_OBJECT || object->dwUnitId != objectId ||
        object->dwTxtFileNo != 397 || object->dwMode != 0 ||
        !Readable(object->pObjectData, sizeof(ObjectData)) ||
        !object->pObjectData->ChestLocked ||
        !Readable(object->pObjectPath, sizeof(ObjectPath)) ||
        !Readable(object->pObjectPath->pRoom1, sizeof(Room1)) ||
        !Readable(object->pObjectPath->pRoom1->pRoom2, sizeof(Room2)) ||
        !Readable(object->pObjectPath->pRoom1->pRoom2->pLevel, sizeof(Level)) ||
        object->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo != from ||
        D2COMMON_GetUnitDistance(player, object) >= 9)
        return Failure("stock locked chest is absent or out of reach");
    json receipt = {{"ok", false}, {"status", "unconfirmed"},
        {"request_id", id}, {"object_id", objectId}};
    if (!ReservePortalNonce(id, fingerprint, receipt))
        return Failure("chest nonce capacity reached");
    BYTE packet[9] = {0x13, UNIT_OBJECT, 0, 0, 0};
    const DWORD number = static_cast<DWORD>(objectId);
    memcpy(packet + 5, &number, sizeof(number));
    if (D2NET_SendPacket(sizeof(packet), 1, packet))
        receipt = {{"ok", true}, {"status", "requested"},
            {"request_id", id}, {"object_id", objectId}, {"from_level", from}};
    {
        std::lock_guard<std::mutex> lock(mutex);
        portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json SelectRoguelikeTrigger(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 4 || !PortalNonce(args, id, epoch))
        return Failure("request_id, expected_epoch, from_level and object_id required");
    int64_t from, objectId;
    if (!args.contains("from_level") || !args.contains("object_id") ||
        !Integer(args["from_level"], 1, 254, from) ||
        (from != 1 && from != 20 &&
         !((from >= 13 && from <= 16) || (from >= 138 && from <= 254))) ||
        !Integer(args["object_id"], 0, UINT32_MAX, objectId))
        return Failure("invalid arena or object ID");
    const std::string fingerprint = "roguelike-select:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!PortalArea(player, static_cast<int>(from))) return Failure("wrong source area");
    UnitAny* object = D2CLIENT_FindServerSideUnit(static_cast<DWORD>(objectId), UNIT_OBJECT);
    if (!object || object->dwType != UNIT_OBJECT || object->dwUnitId != objectId ||
        object->dwTxtFileNo != 60 || !object->pObjectPath ||
        !object->pObjectPath->pRoom1 || !object->pObjectPath->pRoom1->pRoom2 ||
        !object->pObjectPath->pRoom1->pRoom2->pLevel ||
        object->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo != from)
        return Failure("choice object absent in source arena");
    if (D2COMMON_GetUnitDistance(player, object) >= 9)
        return Failure("choice object is outside interaction range");
    const int destination = CreatedTriggerDestination(static_cast<DWORD>(objectId),
        static_cast<int>(from), epoch);
    // Only an owned town entry may select the authored Tower or a verified
    // fresh dynamic root; choices from an arena always require a fresh world.
    const bool freshDestination = destination > 0 && destination < 255 &&
        !runUsedAreas[destination] && TargetUnused(player, destination);
    const bool availableTownRoot = freshDestination &&
        (destination == 20 || IsRunArenaLevel(destination));
    if (from == 1 ? !availableTownRoot : !freshDestination)
        return Failure("choice destination is not available for this run");
    json receipt = {{"ok", false}, {"status", "unconfirmed"}, {"request_id", id},
        {"object_id", objectId}};
    if (!ReservePortalNonce(id, fingerprint, receipt))
        return Failure("selection nonce capacity reached");
    BYTE packet[9] = {0x13, UNIT_OBJECT, 0, 0, 0};
    const DWORD number = static_cast<DWORD>(objectId);
    memcpy(packet + 5, &number, sizeof(number));
    if (D2NET_SendPacket(sizeof(packet), 1, packet))
        receipt = {{"ok", true}, {"status", "requested"}, {"request_id", id},
            {"object_id", objectId}, {"destination_level", destination}};
    {
        std::lock_guard<std::mutex> lock(mutex);
        portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json HireRoguelikeMerc(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 4 || !PortalNonce(args, id, epoch) ||
        !args.contains("npc_id") || !args.contains("merc_name"))
        return Failure("request_id, expected_epoch, npc_id and observed merc_name required");
    int64_t npcId, mercName;
    if (!Integer(args["npc_id"], 1, UINT32_MAX, npcId) ||
        !Integer(args["merc_name"], 1, UINT16_MAX, mercName))
        return Failure("invalid Kashya NPC or mercenary name ID");
    const std::string fingerprint = "roguelike-hire:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || !player->pPath || !player->pPath->pRoom1 ||
        !player->pPath->pRoom1->pRoom2 || !player->pPath->pRoom1->pRoom2->pLevel)
        return Failure("player market area unavailable");
    const int area = player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
    if (area >= 255 || runGame.load(std::memory_order_acquire) == nullptr ||
        runEpoch != epoch || runStage.load(std::memory_order_acquire) != area ||
        zoneSlots[area].kind != Pending::ZoneKind::Merchant || !runCleared[area])
        return Failure("not in an active cleared run market");
    UnitAny* npc = D2CLIENT_GetCurrentInteractingNPC();
    if (!npc || npc->dwUnitId != npcId || npc->dwType != UNIT_MONSTER ||
        npc->dwTxtFileNo != 150 || !D2CLIENT_GetUIState(UI_NPCMENU) ||
        D2CLIENT_GetUIState(UI_NPCSHOP) ||
        D2CLIENT_FindServerSideUnit(static_cast<DWORD>(npcId), UNIT_MONSTER) != npc ||
        D2COMMON_GetUnitDistance(player, npc) >= 9)
        return Failure("specified Kashya menu is not active and in reach");
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (observedHireNames.epoch != epoch ||
            observedHireNames.area != area ||
            observedHireNames.playerId != player->dwUnitId ||
            std::find(observedHireNames.available.begin(),
                observedHireNames.available.end(), static_cast<WORD>(mercName)) ==
                observedHireNames.available.end())
            return Failure("mercenary name was not observed available on this server market");
    }
    json receipt = {{"ok", false}, {"status", "unconfirmed"}, {"request_id", id}};
    if (!ReservePortalNonce(id, fingerprint, receipt))
        return Failure("hire nonce capacity reached");
    BYTE packet[9] = {0x36};
    const DWORD npcNumber = static_cast<DWORD>(npcId);
    const WORD nameNumber = static_cast<WORD>(mercName);
    memcpy(packet + 1, &npcNumber, sizeof(npcNumber));
    memcpy(packet + 5, &nameNumber, sizeof(nameNumber));
    if (D2NET_SendPacket(sizeof(packet), 0, packet))
        receipt = {{"ok", true}, {"status", "requested"}, {"request_id", id},
            {"npc_id", npcId}, {"merc_name", mercName}};
    {
        std::lock_guard<std::mutex> lock(mutex);
        portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json InteractEventShrine(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 3 || !PortalNonce(args, id, epoch) ||
        !args.contains("object_id"))
        return Failure("request_id, expected_epoch and owned shrine object_id required");
    int64_t objectId;
    if (!Integer(args["object_id"], 0, UINT32_MAX, objectId))
        return Failure("invalid shrine object ID");
    const std::string fingerprint = "roguelike-shrine:" + args.dump();
    json cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || !player->pPath || !player->pPath->pRoom1 ||
        !player->pPath->pRoom1->pRoom2 ||
        !player->pPath->pRoom1->pRoom2->pLevel)
        return Failure("player area unavailable");
    const DWORD area = player->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
    if (area >= 255 || runGame.load(std::memory_order_acquire) == nullptr ||
        runEpoch != epoch || runStage.load(std::memory_order_acquire) != area ||
        std::find(runShrineIds[area].begin(), runShrineIds[area].end(),
            static_cast<DWORD>(objectId)) == runShrineIds[area].end())
        return Failure("shrine is not owned by the current cleared event zone");
    UnitAny* object = D2CLIENT_FindServerSideUnit(static_cast<DWORD>(objectId), UNIT_OBJECT);
    if (!object || object->dwType != UNIT_OBJECT || object->dwUnitId != objectId ||
        (object->dwTxtFileNo != 2 && object->dwTxtFileNo != 77) ||
        !object->pObjectData || !object->pObjectPath ||
        !object->pObjectPath->pRoom1 || !object->pObjectPath->pRoom1->pRoom2 ||
        !object->pObjectPath->pRoom1->pRoom2->pLevel ||
        object->pObjectPath->pRoom1->pRoom2->pLevel->dwLevelNo != area ||
        D2COMMON_GetUnitDistance(player, object) >= 9)
        return Failure("owned shrine is absent or outside interaction range");
    json receipt = {{"ok", false}, {"status", "unconfirmed"}, {"request_id", id},
        {"object_id", objectId}};
    if (!ReservePortalNonce(id, fingerprint, receipt))
        return Failure("shrine interaction nonce capacity reached");
    BYTE packet[9] = {0x13, UNIT_OBJECT, 0, 0, 0};
    const DWORD number = static_cast<DWORD>(objectId);
    memcpy(packet + 5, &number, sizeof(number));
    if (D2NET_SendPacket(sizeof(packet), 1, packet))
        receipt = {{"ok", true}, {"status", "requested"},
            {"request_id", id}, {"object_id", objectId}};
    {
        std::lock_guard<std::mutex> lock(mutex);
        portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json RetireRoguelikePortals(const json& args) {
    std::string id, epoch;
    if (!args.is_object() || args.size() != 4 || !PortalNonce(args, id, epoch) ||
        !args.contains("from_level") || !args.contains("commit") ||
        !args["commit"].is_boolean())
        return Failure("request_id, expected_epoch, source and boolean commit required");
    int64_t from;
    if (!Integer(args["from_level"], 1, 254, from) ||
        (from != 1 && from != 20 &&
         !((from >= 13 && from <= 16) || (from >= 138 && from <= 254))))
        return Failure("retirement source is not an approved arena or town entrance");
    const std::string fingerprint = "roguelike-retire:" + args.dump();
    json cached = Cached(id, fingerprint);
    if (!cached.is_null()) return cached;
    cached = PortalCache(id, fingerprint);
    if (!cached.is_null()) return cached;
    UnitAny* player = D2CLIENT_GetPlayerUnit();
    if (!player || player->dwType != UNIT_PLAYER)
        return Failure("player unavailable");
    auto pending = std::make_shared<Pending>();
    pending->id = id;
    pending->fingerprint = fingerprint;
    pending->expectedEpoch = epoch;
    pending->playerId = player->dwUnitId;
    pending->retireFrom = static_cast<int>(from);
    pending->retireJob = true;
    pending->retireCommit = args["commit"].get<bool>();
    if (!ReservePortalNonce(id, fingerprint, {{"ok", false}, {"status", "unconfirmed"},
            {"request_id", id}})) return Failure("retire nonce capacity reached");
    json receipt = Submit(pending);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending->completed) {
            if (!pending->result.is_null())
                portalReceipts.at(id).second = std::move(pending->result);
        } else portalReceipts.at(id).second = receipt;
    }
    return receipt;
}
json RequestEnemies(const json& args) { return EnemyRequest(args, ServerMonsters::Operation::Spawn); }
json InspectEnemies(const json& args) { return EnemyRequest(args, ServerMonsters::Operation::Inspect); }
json InspectArea(const json& args) { return EnemyRequest(args, ServerMonsters::Operation::Area); }
json Observe(const json& args) { return EnemyRequest(args, ServerMonsters::Operation::Observe); }
json AwaitObservation(const json& args, int timeoutMs) {
    if (!args.is_object() || !args.contains("request_id") || !args["request_id"].is_string() ||
        timeoutMs < 0) return Failure("invalid observation wait");
    const std::string id = args["request_id"].get<std::string>();
    const std::string fingerprint = "observation:" + args.dump();
    std::unique_lock<std::mutex> lock(mutex);
    std::shared_ptr<Pending> pending;
    for (const auto& value : requests) if (value->id == id) {
        pending = value;
        break;
    }
    if (!pending || pending->fingerprint != fingerprint || !pending->monsterJob ||
        pending->monsterJob->operation != ServerMonsters::Operation::Observe)
        return {{"ok", false}, {"code", "unknown_request"},
            {"error", "correlated observation request unavailable"}, {"request_id", id}};
    if (!completedRequest.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [&pending] { return pending->completed; }))
        return {{"ok", false}, {"code", "observation_pending"}, {"status", "pending"},
            {"request_id", id}, {"stage", pending->running ? "executing" : "queued"}};
    return pending->result;
}
json StartTrace(const json& args) { return TraceRequest(args); }
json Result(const json& args) {
    if (!args.is_object() || !args.contains("request_id") || !args["request_id"].is_string()) return Failure("request_id is required");
    std::string id = args["request_id"].get<std::string>();
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& pending : requests) if (pending->id == id) {
        if (!pending->completed)
            return {{"ok", true}, {"status", "pending"}, {"request_id", id},
                {"stage", pending->running ? "executing" : "queued"}};
        if (!pending->portalJob && !pending->triggerJob && !pending->zoneJob && !pending->retireJob) return pending->result;
        break;
    }
    auto portal = portalReceipts.find(id);
    if (portal != portalReceipts.end()) return portal->second.second;
    return {{"ok", false}, {"code", "unknown_request"}, {"error", "unknown server spawn request"}};
}

}
