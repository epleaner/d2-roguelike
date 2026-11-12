#include "AutoPickup.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"
#include "BH.h"

#include <mutex>
#include <cmath>
#include <map>

namespace {
    std::mutex g_mutex;
    AutoPickup::Config g_config;
    AutoPickup::BeltSnapshot g_snapshot;
    DWORD g_lastPickTime = 0;
    bool g_wasEnabled = false;

    // Blacklist: items we tried to pick up but failed
    // Maps unit ID → tick when blacklisted
    static const DWORD BLACKLIST_DURATION_MS = 3000;
    std::map<DWORD, DWORD> g_blacklist;

    bool IsBlacklisted(DWORD unitId, DWORD now) {
        auto it = g_blacklist.find(unitId);
        if (it == g_blacklist.end()) return false;
        if (now - it->second >= BLACKLIST_DURATION_MS) {
            g_blacklist.erase(it);
            return false;
        }
        return true;
    }

    void PruneBlacklist(DWORD now) {
        for (auto it = g_blacklist.begin(); it != g_blacklist.end(); ) {
            if (now - it->second >= BLACKLIST_DURATION_MS)
                it = g_blacklist.erase(it);
            else
                ++it;
        }
    }

    // ---- Item Classification ----

    // HP potion tier (1-5), 0 if not HP
    int GetHpTier(DWORD code) {
        // PD2-S12
        if (code >= 602 && code <= 606) return (int)(code - 601);
        // Older PD2
        if (code >= 589 && code <= 593) return (int)(code - 588);
        return 0;
    }

    // MP potion tier (1-5), 0 if not MP
    int GetMpTier(DWORD code) {
        if (code >= 607 && code <= 611) return (int)(code - 606);
        if (code >= 594 && code <= 598) return (int)(code - 593);
        return 0;
    }

    // Rejuv tier: 1=RJ, 2=FRJ, 0 if not rejuv
    int GetRejuvTier(DWORD code) {
        if (code == 517) return 1;  // Rejuvenation
        if (code == 518 || code == 531) return 2;  // Full Rejuvenation
        return 0;
    }

    // Get the category of an item: "hp", "mp", "rejuv", or "" for non-potion
    enum PotionCategory { CAT_NONE = 0, CAT_HP, CAT_MP, CAT_REJUV, CAT_OTHER };

    PotionCategory GetCategory(DWORD code) {
        if (GetHpTier(code) > 0) return CAT_HP;
        if (GetMpTier(code) > 0) return CAT_MP;
        if (GetRejuvTier(code) > 0) return CAT_REJUV;
        return CAT_OTHER;
    }

    int GetTier(DWORD code) {
        int t = GetHpTier(code);
        if (t > 0) return t;
        t = GetMpTier(code);
        if (t > 0) return t;
        t = GetRejuvTier(code);
        if (t > 0) return t;
        return 0;
    }

    // Get the code for a specific tier in a category (PD2-S12 codes)
    DWORD GetCodeForTier(PotionCategory cat, int tier) {
        switch (cat) {
            case CAT_HP: return (tier >= 1 && tier <= 5) ? (DWORD)(601 + tier) : 0;
            case CAT_MP: return (tier >= 1 && tier <= 5) ? (DWORD)(606 + tier) : 0;
            case CAT_REJUV: return (tier == 1) ? 517 : (tier == 2) ? 518 : 0;
            default: return 0;
        }
    }

    // Check if groundCode is acceptable for a column that prefers preferredCode
    // Returns true if groundCode is same category and >= preferred tier
    bool IsAcceptablePickup(DWORD groundCode, DWORD preferredCode) {
        PotionCategory prefCat = GetCategory(preferredCode);
        PotionCategory groundCat = GetCategory(groundCode);

        if (prefCat == CAT_OTHER || groundCat == CAT_OTHER) {
            // Non-tiered items: exact match only
            return groundCode == preferredCode;
        }

        if (prefCat != groundCat) return false;

        int prefTier = GetTier(preferredCode);
        int groundTier = GetTier(groundCode);

        // Accept same or better tier
        return groundTier >= prefTier;
    }

    // Get the fallback code (one tier lower) for dire situations
    DWORD GetFallbackCode(DWORD preferredCode) {
        PotionCategory cat = GetCategory(preferredCode);
        int tier = GetTier(preferredCode);
        if (tier <= 1 || cat == CAT_OTHER) return 0;  // no fallback possible
        return GetCodeForTier(cat, tier - 1);
    }

    // ---- Belt Analysis ----

    // Count how many potions of a given category are in the belt
    int CountBeltPotionsOfCategory(UnitAny* pPlayer, PotionCategory cat) {
        if (!pPlayer || !pPlayer->pInventory) return 0;
        int count = 0;
        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData && pItem->pItemData->NodePage == NODEPAGE_BELTSLOTS) {
                if (GetCategory(pItem->dwTxtFileNo) == cat) count++;
            }
            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }
        return count;
    }

    // Check which columns have any empty slots (across all 4 rows)
    // Returns bitmask: bit 0 = col 0 has empty slot, bit 1 = col 1 has empty slot, etc.
    int GetEmptyColumns(UnitAny* pPlayer) {
        if (!pPlayer || !pPlayer->pInventory) return 0;

        // Count items per column
        int colCount[4] = {};
        int totalBeltItems = 0;
        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData && pItem->pItemData->NodePage == NODEPAGE_BELTSLOTS) {
                totalBeltItems++;
                if (pItem->pPath) {
                    ItemPath* ip = (ItemPath*)pItem->pPath;
                    int slot = (int)ip->dwPosX;
                    int col = slot % 4;
                    if (col >= 0 && col < 4) colCount[col]++;
                }
            }
            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }

        // Determine belt rows (4 rows max for war belt)
        // If total items <= 4, assume at least 1 row available per column
        int maxRows = 4; // assume max belt

        int emptyMask = 0;
        for (int i = 0; i < 4; i++) {
            if (colCount[i] < maxRows) emptyMask |= (1 << i);
        }
        return emptyMask;
    }

    // Snapshot the current belt: read what's in each column (row 0)
    void TakeSnapshot(UnitAny* pPlayer) {
        g_snapshot = {};
        if (!pPlayer || !pPlayer->pInventory) return;

        // Read the bottom row (slots 0-3) to determine column types
        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData && pItem->pItemData->NodePage == NODEPAGE_BELTSLOTS) {
                if (pItem->pPath) {
                    ItemPath* ip = (ItemPath*)pItem->pPath;
                    int slot = (int)ip->dwPosX;
                    int col = slot % 4;
                    int row = slot / 4;
                    // Prefer bottom row (row 0) for snapshot, but use any row if bottom is empty
                    if (col >= 0 && col < 4) {
                        if (row == 0 || g_snapshot.preferredCode[col] == 0) {
                            g_snapshot.preferredCode[col] = pItem->dwTxtFileNo;
                        }
                    }
                }
            }
            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }
        g_snapshot.valid = true;
    }

    // ---- Scroll/Tome Logic ----
    // Use game name lookup instead of hardcoded codes since PD2 remaps item IDs.

    static const int MAX_TOME_CHARGES = 80; // PD2 tomes hold more than vanilla's 20

    // Check if an item's name contains a fragment — uses SEH since
    // D2CLIENT_GetUnitName can crash on invalid/transitioning units
    static bool SafeGetUnitName(UnitAny* pItem, char* outName, int outSize) {
        __try {
            wchar_t* wName = D2CLIENT_GetUnitName(pItem);
            if (wName) {
                WideCharToMultiByte(CP_UTF8, 0, wName, -1, outName, outSize - 1, nullptr, nullptr);
                return true;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    bool IsItemByName(UnitAny* pItem, const char* nameFragment) {
        if (!pItem) return false;
        char name[64] = {};
        if (SafeGetUnitName(pItem, name, sizeof(name))) {
            return strstr(name, nameFragment) != nullptr;
        }
        return false;
    }

    // Check if a ground item is a TP or ID scroll
    bool IsTpScrollUnit(UnitAny* pItem) { return IsItemByName(pItem, "Town Portal"); }
    bool IsIdScrollUnit(UnitAny* pItem) { return IsItemByName(pItem, "Identify"); }

    // Find a tome in inventory and return its charge count
    // Gets the full name once and checks for both "Tome" and the scroll type
    int GetTomeCharges(UnitAny* pPlayer, bool isTp) {
        if (!pPlayer || !pPlayer->pInventory) return -1;
        const char* scrollType = isTp ? "Town Portal" : "Identify";

        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData) {
                char name[64] = {};
                if (SafeGetUnitName(pItem, name, sizeof(name))) {
                    // Match "Tome" AND the scroll type in one name lookup
                    if (strstr(name, "Tome") && strstr(name, scrollType)) {
                        return D2COMMON_GetUnitStat(pItem, STAT_AMMOQUANTITY, 0);
                    }
                }
            }
            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }
        return -1;
    }

    bool g_needTpScrolls = false;
    bool g_needIdScrolls = false;

    // ---- Pickup ----

    void PickupItem(DWORD itemId) {
        BYTE packet[13] = {};
        packet[0] = 0x16;
        *(DWORD*)&packet[1] = 0x04;
        *(DWORD*)&packet[5] = itemId;
        D2NET_SendPacket(13, 0, packet);
    }
}

namespace AutoPickup {

    void Update() {
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            cfg = g_config;
        }

        if (!cfg.enabled) {
            g_wasEnabled = false;
            return;
        }
        if (!IsGameReady()) return;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer || !pPlayer->pPath || !pPlayer->pAct || !pPlayer->pInventory) return;
        if (pPlayer->pInventory->pCursorItem) return;

        g_wasEnabled = true;

        // Cooldown
        DWORD now = GetTickCount();
        if (now - g_lastPickTime < (DWORD)cfg.cooldownMs) return;

        // Prune expired blacklist entries periodically
        PruneBlacklist(now);

        // Check tome charges for scroll pickup
        g_needTpScrolls = false;
        g_needIdScrolls = false;
        if (cfg.pickTpScrolls) {
            int c = GetTomeCharges(pPlayer, true);
            g_needTpScrolls = (c >= 0 && c < MAX_TOME_CHARGES);
        }
        if (cfg.pickIdScrolls) {
            int c = GetTomeCharges(pPlayer, false);
            g_needIdScrolls = (c >= 0 && c < MAX_TOME_CHARGES);
        }

        // Check which belt columns need refill
        int emptyMask = GetEmptyColumns(pPlayer);
        bool needBeltRefill = (emptyMask != 0);
        bool needScrolls = g_needTpScrolls || g_needIdScrolls;

        if (!needBeltRefill && !needScrolls) return;

        int playerX = pPlayer->pPath->xPos;
        int playerY = pPlayer->pPath->yPos;

        // Scan ground items
        UnitAny* bestItem = nullptr;
        int bestDist = cfg.maxDistance + 1;
        int bestPriority = -1; // higher = better match

        for (Room1* room1 = pPlayer->pAct->pRoom1; room1; room1 = room1->pRoomNext) {
            for (UnitAny* pUnit = room1->pUnitFirst; pUnit; pUnit = pUnit->pListNext) {
                if (pUnit->dwType != 4) continue;
                if (!pUnit->pItemData) continue;
                if (pUnit->dwMode != 3 && pUnit->dwMode != 5) continue;
                if (IsBlacklisted(pUnit->dwUnitId, now)) continue;

                DWORD code = pUnit->dwTxtFileNo;

                // Calculate distance
                int ix = 0, iy = 0;
                if (pUnit->pPath) {
                    ItemPath* ip = (ItemPath*)pUnit->pPath;
                    ix = ip->dwPosX;
                    iy = ip->dwPosY;
                }
                int dx = ix - playerX;
                int dy = iy - playerY;
                int dist = (int)sqrt((double)(dx * dx + dy * dy));
                if (dist > cfg.maxDistance) continue;

                int priority = 0;

                // Check scrolls first (they go to inventory, not belt)
                // Use name-based detection since PD2 remaps item codes
                if (g_needTpScrolls && IsTpScrollUnit(pUnit)) { priority = 1; }
                else if (g_needIdScrolls && IsIdScrollUnit(pUnit)) { priority = 1; }
                // Check belt refill
                else if (needBeltRefill) {
                    // Only pick up HP, Mana, or Rejuv potions
                    bool isHp = IsItemByName(pUnit, "Healing");
                    bool isMp = IsItemByName(pUnit, "Mana");
                    bool isRejuv = IsItemByName(pUnit, "Rejuv");
                    bool isBeltablePotion = isHp || isMp || isRejuv;

                    if (!isBeltablePotion) {} // skip non-potions
                    else {
                    // Find which empty column this item could fill
                    for (int col = 0; col < 4; col++) {
                        if (!(emptyMask & (1 << col))) continue;

                        DWORD preferred = 0;
                        if (g_snapshot.valid) preferred = g_snapshot.preferredCode[col];

                        if (preferred == 0) {
                            // No preference — accept any HP/Mana/Rejuv potion
                            priority = 2;
                            break;
                        }

                        // Exact code match — always pick up
                        if (code == preferred) {
                            priority = 20;
                            break;
                        }

                        // Same category, better tier
                        if (IsAcceptablePickup(code, preferred)) {
                            priority = 10 + GetTier(code);
                            break;
                        }

                        // Fallback: if last of this category in belt, accept one tier lower
                        PotionCategory cat = GetCategory(preferred);
                        if (cat != CAT_OTHER && cat != CAT_NONE) {
                            int remaining = CountBeltPotionsOfCategory(pPlayer, cat);
                            if (remaining <= 1) {
                                DWORD fallback = GetFallbackCode(preferred);
                                if (fallback > 0 && code == fallback) {
                                    priority = 5;
                                    break;
                                }
                            }
                        }
                    }
                    } // else (isConsumable)
                }

                if (priority > 0 && (priority > bestPriority || (priority == bestPriority && dist < bestDist))) {
                    bestPriority = priority;
                    bestDist = dist;
                    bestItem = pUnit;
                }
            }
        }

        if (bestItem) {
            PickupItem(bestItem->dwUnitId);
            g_blacklist[bestItem->dwUnitId] = now;
            g_lastPickTime = now;
        }
    }

    Config GetConfig() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_config;
    }

    void SetConfig(const Config& cfg) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = cfg;
    }

    BeltSnapshot GetSnapshot() {
        return g_snapshot;
    }

    void SetSnapshot(const BeltSnapshot& snap) {
        g_snapshot = snap;
    }

    void ResnapBelt() {
        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (pPlayer) {
            TakeSnapshot(pPlayer);
            // Save snapshot to App config for persistence
            App.autoPickup.snapCol0.value = g_snapshot.preferredCode[0];
            App.autoPickup.snapCol1.value = g_snapshot.preferredCode[1];
            App.autoPickup.snapCol2.value = g_snapshot.preferredCode[2];
            App.autoPickup.snapCol3.value = g_snapshot.preferredCode[3];
        }
    }
}
