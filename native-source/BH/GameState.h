#pragma once

// GameState - Reads live game state for the debug panel and MCP server.
// Thread-safe snapshot model: game thread populates, UI/MCP threads read.

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

namespace GameState {

    struct PlayerState {
        bool valid = false;
        char name[16] = {};
        int classId = 0;        // 0=Amazon, 1=Sorc, 2=Necro, 3=Pala, 4=Barb, 5=Druid, 6=Assassin
        int mode = 0;           // native player animation/movement mode
        int level = 0;
        int hp = 0;             // current (raw — >> 8 for display)
        int maxHp = 0;
        int mana = 0;
        int maxMana = 0;
        int stamina = 0;
        int maxStamina = 0;
        int x = 0;              // world position
        int y = 0;
        int area = 0;           // level number
        int act = 0;            // 0-4
        int gold = 0;
        int goldStash = 0;
        int addXp = 0;          // additional experience %

        // Resistances (with penalty applied)
        int fireRes = 0, maxFireRes = 0;
        int coldRes = 0, maxColdRes = 0;
        int lightRes = 0, maxLightRes = 0;
        int poisonRes = 0, maxPoisonRes = 0;
        int curseRes = 0, curseLenReduce = 0;
        int poisonLenReduce = 0;
        int halfFreeze = 0;
        int cannotBeFrozen = 0;

        // Absorption (flat / percent)
        int fireAbsorb = 0, fireAbsorbPct = 0;
        int coldAbsorb = 0, coldAbsorbPct = 0;
        int lightAbsorb = 0, lightAbsorbPct = 0;
        int magicAbsorb = 0, magicAbsorbPct = 0;

        // Damage reduction
        int dmgReduction = 0, dmgReductionPct = 0;
        int magDmgReduction = 0, magDmgReductionPct = 0;
        int attackerTakesDmg = 0, attackerTakesLtng = 0;

        // Elemental mastery & pierce (%)
        int fireMastery = 0, coldMastery = 0, lightMastery = 0, poisonMastery = 0, magicMastery = 0;
        int firePierce = 0, coldPierce = 0, lightPierce = 0, poisonPierce = 0, magicPierce = 0;

        // Attack / defense
        int strength = 0, energy = 0, vitality = 0;
        int dexterity = 0;
        int attackRating = 0;
        int defense = 0;
        int minDmg = 0, maxDmg = 0;         // 1h
        int minDmg2 = 0, maxDmg2 = 0;       // 2h

        // Rates
        int fcr = 0, fhr = 0, fbr = 0, ias = 0, frw = 0;
        int attackRate = 0;

        // Combat stats
        int crushingBlow = 0;
        int openWounds = 0, deepWounds = 0;
        int deadlyStrike = 0, maxDeadlyStrike = 0;
        int criticalStrike = 0;
        int lifeLeech = 0, manaLeech = 0;
        int piercingAttack = 0, pierce = 0;
        int lifePerKill = 0, manaPerKill = 0;

        // Elemental damage (raw — apply mastery for display)
        int minFireDmg = 0, maxFireDmg = 0;
        int minColdDmg = 0, maxColdDmg = 0;
        int minLightDmg = 0, maxLightDmg = 0;
        int minPoisonDmg = 0, maxPoisonDmg = 0, poisonLength = 0, poisonLenOverride = 0;
        int minMagicDmg = 0, maxMagicDmg = 0;
        int addedDamage = 0;

        // Find
        int mf = 0, gf = 0;

        // Breakpoints
        struct BreakpointInfo {
            int currentValue = 0;
            int activeIndex = -1;       // which breakpoint is active (-1 = below first)
            int values[16] = {};
            int count = 0;
            char label[32] = {};        // "FCR", "FHR", "IAS"
        };
        BreakpointInfo bpFCR;
        BreakpointInfo bpFHR;
        char bpSkillName[64] = {};      // skill name for breakpoint context (e.g., "Teleport")

        // Game context
        int difficulty = 0;         // 0=Normal, 1=Nightmare, 2=Hell
        int playerCount = 0;        // players in game (from roster list)
        int resPenalty = 0;         // resistance penalty for current difficulty
        bool isExpansion = false;
        int charClassNum = 0;       // raw class number from BnData

        // XP
        unsigned int currentXp = 0;   // STAT_EXP
        unsigned int lastLevelXp = 0; // STAT_LASTEXPERIENCE
        unsigned int nextLevelXp = 0; // STAT_NEXTEXPERIENCE
        float xpPctToNext = 0;        // percentage to next level

        // Area
        char areaName[64] = {};
    };

    struct BeltSlot {
        bool occupied = false;
        int itemCode = 0;       // TxtFileNo
        char name[32] = {};     // short label (HP5, MP3, FRJ, etc.)
        char fullName[64] = {}; // full game name
        int quantity = 0;       // for stackable items
    };

    struct BeltState {
        int columns = 4;
        int rows = 1;           // 1-4 depending on belt type
        BeltSlot slots[16];     // 4 columns x 4 rows max
    };

    struct NearbyUnit {
        int type = 0;           // UNIT_PLAYER=0, UNIT_MONSTER=1, UNIT_OBJECT=2, UNIT_ITEM=4
        int classId = 0;
        int unitId = 0;
        int x = 0;
        int y = 0;
        int hp = 0;             // current (divide by 256)
        int maxHp = 0;          // max (divide by 256)
        int mode = 0;           // 0=dead, etc.
        int distance = 0;       // from player
        char name[64] = {};
        bool isBoss = false;
        bool isChampion = false;
        bool isMinion = false;
        // Monster resistances (raw values, immune if >= 100)
        int fireRes = 0;
        int coldRes = 0;
        int lightRes = 0;
        int poisonRes = 0;
        int physRes = 0;
        int magicRes = 0;
    };

    // Session statistics (persist across games within a single DLL session)
    struct SessionStats {
        int deaths = 0;         // times player died
        int gamesEntered = 0;   // times a game was entered (restarts)
    };

    // Update game state snapshot (call from game thread)
    void Update();
    // Idempotent normal-exit transition; Update also calls this on an observed leave.
    void EndSession();

    // Thread-safe accessors (call from any thread)
    PlayerState GetPlayerState();
    BeltState GetBeltState();
    std::vector<NearbyUnit> GetNearbyUnits(int maxDistance = 40);
    SessionStats GetSessionStats();

    // Record events
    void RecordGameEntered();

    // Is the game in a state where we can read data?
    bool IsGameReady();
}
