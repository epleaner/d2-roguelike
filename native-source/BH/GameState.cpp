#include "GameState.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"
#include "ObservationTelemetry.h"
#include "CombatTelemetry.h"
#include "ServerSpawns.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {
    std::mutex g_mutex;
    GameState::PlayerState g_player;
    GameState::BeltState g_belt;
    std::vector<GameState::NearbyUnit> g_units;
    bool g_ready = false;
    bool g_sessionInvalidated = true;

    // Session stats (persist for DLL lifetime)
    GameState::SessionStats g_sessionStats;
    bool g_wasAlive = true;   // edge detection for death counting
    bool g_wasInGame = false; // edge detection for game entry counting

    const char* GetClassName(int classId) {
        switch (classId) {
            case 0: return "Amazon";
            case 1: return "Sorceress";
            case 2: return "Necromancer";
            case 3: return "Paladin";
            case 4: return "Barbarian";
            case 5: return "Druid";
            case 6: return "Assassin";
            default: return "Unknown";
        }
    }

    void UpdatePlayerState() {
        GameState::PlayerState ps;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_player = ps;
            return;
        }

        ps.valid = true;
        ps.classId = pPlayer->dwTxtFileNo;
        ps.mode = pPlayer->dwMode;

        if (pPlayer->pPlayerData) {
            strncpy_s(ps.name, pPlayer->pPlayerData->szName, sizeof(ps.name) - 1);
        }

        // Read only fields exposed by the bounded backend; legacy panels are not compiled.
        #define GS(statId) D2COMMON_GetUnitStat(pPlayer, statId, 0)

        // Core
        ps.level = GS(STAT_LEVEL);
        ps.hp = GS(STAT_HP);
        ps.maxHp = GS(STAT_MAXHP);
        ps.mana = GS(STAT_MANA);
        ps.maxMana = GS(STAT_MAXMANA);
        ps.gold = GS(STAT_GOLD);
        ps.goldStash = GS(STAT_GOLDBANK);
        ps.strength = GS(STAT_STRENGTH);
        ps.energy = GS(STAT_ENERGY);
        ps.vitality = GS(STAT_VITALITY);
        ps.dexterity = GS(STAT_DEXTERITY);
        #undef GS


        // Position
        if (pPlayer->pPath) {
            ps.x = pPlayer->pPath->xPos;
            ps.y = pPlayer->pPath->yPos;

            if (pPlayer->pPath->pRoom1 &&
                pPlayer->pPath->pRoom1->pRoom2 &&
                pPlayer->pPath->pRoom1->pRoom2->pLevel) {
                ps.area = pPlayer->pPath->pRoom1->pRoom2->pLevel->dwLevelNo;
            }
        }

        // Area name from level data
        try {
            if (ps.area > 0) {
                wchar_t* wName = D2CLIENT_GetLevelName(ps.area);
                if (wName) {
                    WideCharToMultiByte(CP_UTF8, 0, wName, -1, ps.areaName, sizeof(ps.areaName) - 1, nullptr, nullptr);
                }
            }
        } catch (...) {}

        std::lock_guard<std::mutex> lock(g_mutex);
        g_player = ps;
    }

    void UpdateBeltState() {
        GameState::BeltState bs;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer || !pPlayer->pInventory) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_belt = bs;
            return;
        }

        bs.rows = 4; // assume max belt size, show all 16 slots
        bs.columns = 4;

        // Walk inventory items — belt items have NodePage == NODEPAGE_BELTSLOTS
        // ItemPath.dwPosX contains the flat belt slot index (0-15)
        UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
        while (pItem) {
            if (pItem->pItemData && pItem->pItemData->NodePage == NODEPAGE_BELTSLOTS) {
                int idx = 0;
                if (pItem->pPath) {
                    ItemPath* ip = (ItemPath*)pItem->pPath;
                    idx = (int)ip->dwPosX;
                }

                if (idx >= 0 && idx < 16) {
                    GameState::BeltSlot& slot = bs.slots[idx];
                    slot.occupied = true;
                    slot.itemCode = pItem->dwTxtFileNo;

                    // Get the real item name from the game's data tables
                    char fullName[64] = {};
                    try {
                        wchar_t* wName = D2CLIENT_GetUnitName(pItem);
                        if (wName) {
                            WideCharToMultiByte(CP_UTF8, 0, wName, -1, fullName, sizeof(fullName) - 1, nullptr, nullptr);
                        }
                    } catch (...) {}

                    // Store full name for MCP/tooltip
                    if (fullName[0]) {
                        strncpy_s(slot.fullName, fullName, sizeof(slot.fullName) - 1);
                    }

                    // Derive short label from full name
                    if (fullName[0]) {
                        // Map known full names to short labels
                        // Health potions
                        if (strstr(fullName, "Minor Heal"))       strncpy_s(slot.name, "HP1", sizeof(slot.name));
                        else if (strstr(fullName, "Light Heal"))  strncpy_s(slot.name, "HP2", sizeof(slot.name));
                        else if (strstr(fullName, "Greater Heal"))strncpy_s(slot.name, "HP4", sizeof(slot.name));
                        else if (strstr(fullName, "Super Heal"))  strncpy_s(slot.name, "HP5", sizeof(slot.name));
                        else if (strstr(fullName, "Heal"))        strncpy_s(slot.name, "HP3", sizeof(slot.name));
                        // Mana potions
                        else if (strstr(fullName, "Minor Mana"))  strncpy_s(slot.name, "MP1", sizeof(slot.name));
                        else if (strstr(fullName, "Light Mana"))  strncpy_s(slot.name, "MP2", sizeof(slot.name));
                        else if (strstr(fullName, "Greater Mana"))strncpy_s(slot.name, "MP4", sizeof(slot.name));
                        else if (strstr(fullName, "Super Mana"))  strncpy_s(slot.name, "MP5", sizeof(slot.name));
                        else if (strstr(fullName, "Mana"))        strncpy_s(slot.name, "MP3", sizeof(slot.name));
                        // Rejuv
                        else if (strstr(fullName, "Full Rejuv"))  strncpy_s(slot.name, "FRJ", sizeof(slot.name));
                        else if (strstr(fullName, "Rejuv"))       strncpy_s(slot.name, "RJ", sizeof(slot.name));
                        // Scrolls
                        else if (strstr(fullName, "Town Portal")) strncpy_s(slot.name, "TP", sizeof(slot.name));
                        else if (strstr(fullName, "Identify"))    strncpy_s(slot.name, "ID", sizeof(slot.name));
                        // Utility
                        else if (strstr(fullName, "Antidote"))    strncpy_s(slot.name, "Antd", sizeof(slot.name));
                        else if (strstr(fullName, "Thawing"))     strncpy_s(slot.name, "Thaw", sizeof(slot.name));
                        else if (strstr(fullName, "Stamina"))     strncpy_s(slot.name, "Stam", sizeof(slot.name));
                        else if (strstr(fullName, "Key"))         strncpy_s(slot.name, "Key", sizeof(slot.name));
                        // Unknown — use abbreviated full name
                        else {
                            strncpy_s(slot.name, fullName, sizeof(slot.name) - 1);
                            // Truncate to fit button
                            if (strlen(slot.name) > 8) slot.name[8] = '\0';
                        }
                    } else {
                        snprintf(slot.name, sizeof(slot.name), "#%d", pItem->dwTxtFileNo);
                    }
                }
            }

            pItem = D2COMMON_GetNextItemFromInventory(pItem);
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        g_belt = bs;
    }

    void UpdateNearbyUnits() {
        std::vector<GameState::NearbyUnit> units;

        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (!pPlayer || !pPlayer->pPath || !pPlayer->pAct) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_units = units;
            return;
        }

        int playerX = pPlayer->pPath->xPos;
        int playerY = pPlayer->pPath->yPos;

        for (Room1* room1 = pPlayer->pAct->pRoom1; room1; room1 = room1->pRoomNext) {
            for (UnitAny* pUnit = room1->pUnitFirst; pUnit; pUnit = pUnit->pRoomNext) {
                if (pUnit == pPlayer) continue;
                if (pUnit->dwType != 0 && pUnit->dwType != 1 && pUnit->dwType != 4) continue;

                GameState::NearbyUnit nu;
                nu.type = pUnit->dwType;
                nu.classId = pUnit->dwTxtFileNo;
                nu.unitId = pUnit->dwUnitId;
                nu.mode = pUnit->dwMode;

                if (pUnit->pPath) {
                    nu.x = pUnit->pPath->xPos;
                    nu.y = pUnit->pPath->yPos;
                } else {
                    nu.x = pUnit->wX;
                    nu.y = pUnit->wY;
                }

                int dx = nu.x - playerX;
                int dy = nu.y - playerY;
                nu.distance = (int)sqrt((double)(dx * dx + dy * dy));

                // Get unit name from game data tables
                try {
                    wchar_t* wName = D2CLIENT_GetUnitName(pUnit);
                    if (wName) {
                        WideCharToMultiByte(CP_UTF8, 0, wName, -1, nu.name, sizeof(nu.name) - 1, nullptr, nullptr);
                    }
                } catch (...) {}

                if (pUnit->dwType == 1) { // Monster
                    nu.hp = D2COMMON_GetUnitStat(pUnit, STAT_HP, 0);
                    nu.maxHp = D2COMMON_GetUnitStat(pUnit, STAT_MAXHP, 0);

                    if (pUnit->pMonsterData) {
                        BYTE flags = pUnit->pMonsterData->fBoss;
                        nu.isBoss = (flags & 1) != 0;
                        nu.isChampion = (flags & 2) != 0;
                        nu.isMinion = (flags & 4) != 0;
                    }

                    // Read monster resistances for immunity detection
                    nu.fireRes = D2COMMON_GetUnitStat(pUnit, STAT_FIRERESIST, 0);
                    nu.coldRes = D2COMMON_GetUnitStat(pUnit, STAT_COLDRESIST, 0);
                    nu.lightRes = D2COMMON_GetUnitStat(pUnit, STAT_LIGHTNINGRESIST, 0);
                    nu.poisonRes = D2COMMON_GetUnitStat(pUnit, STAT_POISONRESIST, 0);
                    nu.physRes = D2COMMON_GetUnitStat(pUnit, STAT_DMGREDUCTIONPCT, 0);
                    nu.magicRes = D2COMMON_GetUnitStat(pUnit, STAT_MAGICDMGREDUCTIONPCT, 0);

                    // Fallback if name lookup failed
                    if (!nu.name[0]) snprintf(nu.name, sizeof(nu.name), "Monster#%d", pUnit->dwTxtFileNo);
                } else if (pUnit->dwType == 0) { // Player
                    nu.hp = D2COMMON_GetUnitStat(pUnit, STAT_HP, 0);
                    nu.maxHp = D2COMMON_GetUnitStat(pUnit, STAT_MAXHP, 0);
                    // Players: prefer pPlayerData->szName (ASCII, no lookup needed)
                    if (pUnit->pPlayerData && !nu.name[0]) {
                        strncpy_s(nu.name, pUnit->pPlayerData->szName, sizeof(nu.name) - 1);
                    }
                } else if (pUnit->dwType == 4) { // Item
                    if (!nu.name[0]) snprintf(nu.name, sizeof(nu.name), "Item#%d", pUnit->dwTxtFileNo);
                }

                units.push_back(nu);
            }
        }

        // Sort by distance
        std::sort(units.begin(), units.end(),
            [](const GameState::NearbyUnit& a, const GameState::NearbyUnit& b) {
                return a.distance < b.distance;
            });

        std::lock_guard<std::mutex> lock(g_mutex);
        g_units = std::move(units);
    }

    // Keep SEH in a POD-only frame. Update's mutex guards require C++ unwinding.
    bool TryUpdatePlayerState() {
        __try {
            UpdatePlayerState();
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
}

namespace GameState {
    void EndSession() {
        if (g_sessionInvalidated) return;
        ServerSpawns::RestoreRoguelikeZones();
        g_sessionInvalidated = true;
        CombatTelemetry::Shutdown();
        ObservationTelemetry::Invalidate();
    }
    void Update() {
        bool wasReady = g_ready;
        g_ready = IsGameReady();
        if (wasReady && !g_ready) EndSession();
        if (g_ready && !wasReady) {
            g_sessionStats.gamesEntered++;
            g_wasAlive = true; // reset death edge detection for new game
            g_sessionInvalidated = false;
            ServerSpawns::EnsureInstalled();
        }
        if (!g_ready) {
            g_wasInGame = false;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_player = PlayerState{};
            g_belt = BeltState{};
            g_units.clear();
            return;
        }

        if (!TryUpdatePlayerState()) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_player = PlayerState{};
        }
    }

    PlayerState GetPlayerState() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_player;
    }

    BeltState GetBeltState() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_belt;
    }

    std::vector<NearbyUnit> GetNearbyUnits(int maxDistance) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (maxDistance <= 0) return g_units;

        std::vector<NearbyUnit> filtered;
        for (const auto& u : g_units) {
            if (u.distance <= maxDistance) filtered.push_back(u);
        }
        return filtered;
    }

    SessionStats GetSessionStats() {
        // No mutex needed — reads are atomic for ints
        return g_sessionStats;
    }

    void RecordGameEntered() {
        g_sessionStats.gamesEntered++;
    }

    bool IsGameReady() {
        return ::IsGameReady();
    }
}
