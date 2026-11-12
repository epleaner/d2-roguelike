#include "AutoPotion.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"

#include <mutex>

// Potion affect states — checked to avoid drinking while one is active
#define AFFECT_HEALTHPOT 100
#define AFFECT_MANAPOT   106

// Town area IDs
static bool IsTownArea(int areaId) {
    return areaId == 1 || areaId == 40 || areaId == 75 || areaId == 103 || areaId == 109;
}

namespace {
    std::mutex g_mutex;
    AutoPotion::Config g_config;
    DWORD g_lastHpPotTime = 0;
    DWORD g_lastMpPotTime = 0;
    DWORD g_lastRejuvTime = 0;

    // Send a "use belt item" packet (0x26) with the item's unit ID
    void UseBeltItem(UnitAny* pItem) {
        if (!pItem) return;
        BYTE packet[13] = {};
        packet[0] = 0x26;
        *(DWORD*)&packet[1] = pItem->dwUnitId;
        *(DWORD*)&packet[5] = 0;
        *(DWORD*)&packet[9] = 0;
        D2NET_SendPacket(13, 1, packet);
    }

    // Get item name from game data tables (cached per call)
    bool ItemNameContains(UnitAny* pItem, const char* fragment) {
        if (!pItem) return false;
        try {
            wchar_t* wName = D2CLIENT_GetUnitName(pItem);
            if (wName) {
                char name[64] = {};
                WideCharToMultiByte(CP_UTF8, 0, wName, -1, name, sizeof(name) - 1, nullptr, nullptr);
                return strstr(name, fragment) != nullptr;
            }
        } catch (...) {}
        return false;
    }

    // Find a belt potion by name fragment (e.g., "Healing", "Mana", "Rejuv")
    UnitAny* FindBeltPotionByName(UnitAny* pPlayer, const char* nameFragment) {
        if (!pPlayer || !pPlayer->pInventory) return nullptr;

        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData && pItem->pItemData->NodePage == NODEPAGE_BELTSLOTS) {
                if (ItemNameContains(pItem, nameFragment)) return pItem;
            }
            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }
        return nullptr;
    }
}

namespace AutoPotion {

    void Update() {
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            cfg = g_config;
        }

        if (!cfg.enabled || !IsGameReady()) return;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer || !pPlayer->pPath || !pPlayer->pInventory) return;

        // Skip in town
        if (cfg.skipInTown) {
            if (pPlayer->pPath->pRoom1 &&
                pPlayer->pPath->pRoom1->pRoom2 &&
                pPlayer->pPath->pRoom1->pRoom2->pLevel) {
                int area = pPlayer->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
                if (IsTownArea(area)) return;
            }
        }

        // Don't use potions if cursor is holding an item
        if (pPlayer->pInventory->pCursorItem) return;

        int hp = D2COMMON_GetUnitStat(pPlayer, STAT_HP, 0) >> 8;
        int maxHp = D2COMMON_GetUnitStat(pPlayer, STAT_MAXHP, 0) >> 8;
        int mp = D2COMMON_GetUnitStat(pPlayer, STAT_MANA, 0) >> 8;
        int maxMp = D2COMMON_GetUnitStat(pPlayer, STAT_MAXMANA, 0) >> 8;

        if (maxHp == 0 || maxMp == 0) return;

        int hpPct = (hp * 100) / maxHp;
        int mpPct = (mp * 100) / maxMp;
        DWORD now = GetTickCount();

        // Rejuv — critical health, highest priority
        // Rejuvs are instant (no active state to check), just use cooldown
        if (cfg.rejuvThreshold > 0 && hpPct <= cfg.rejuvThreshold) {
            if (now - g_lastRejuvTime >= (DWORD)cfg.cooldownMs) {
                // Prefer Full Rejuv, fallback to regular Rejuv
                UnitAny* pot = FindBeltPotionByName(pPlayer, "Full Rejuv");
                if (!pot) pot = FindBeltPotionByName(pPlayer, "Rejuv");
                if (pot) {
                    UseBeltItem(pot);
                    g_lastRejuvTime = now;
                    return;
                }
            }
        }

        // HP potion
        if (cfg.hpThreshold > 0 && hpPct <= cfg.hpThreshold) {
            if (now - g_lastHpPotTime >= (DWORD)cfg.cooldownMs) {
                if (!D2COMMON_GetUnitState(pPlayer, AFFECT_HEALTHPOT)) {
                    UnitAny* pot = FindBeltPotionByName(pPlayer, "Healing");
                    if (pot) {
                        UseBeltItem(pot);
                        g_lastHpPotTime = now;
                        return;
                    }
                }
            }
        }

        // MP potion
        if (cfg.mpThreshold > 0 && mpPct <= cfg.mpThreshold) {
            if (now - g_lastMpPotTime >= (DWORD)cfg.cooldownMs) {
                if (!D2COMMON_GetUnitState(pPlayer, AFFECT_MANAPOT)) {
                    UnitAny* pot = FindBeltPotionByName(pPlayer, "Mana");
                    if (pot) {
                        UseBeltItem(pot);
                        g_lastMpPotTime = now;
                        return;
                    }
                }
            }
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
}
