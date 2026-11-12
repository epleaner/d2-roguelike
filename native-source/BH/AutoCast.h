#pragma once
#include <Windows.h>
#include <vector>

// AutoCast — Game-thread auto-combat system
// Runs every frame in GameLoop, similar to AutoPotion.
// Quick-casts skills at nearby monsters, handles immunity fallback,
// and maintains buff uptime.

namespace AutoCast {

    enum class TargetPriority {
        Nearest = 0,
        LowestHP,
        HighestHP,
        BossPriority,
        COUNT
    };

    static const char* TargetPriorityNames[] = {
        "Nearest", "Lowest HP", "Highest HP", "Boss/Champ First"
    };

    // Per-side (left/right click) auto-cast config
    struct SkillSlot {
        bool enabled = false;
        WORD skillId = 0;           // primary skill to cast
        WORD backupSkillId = 0;     // backup if primary is resisted
        char skillName[32] = {};
        char backupName[32] = {};
    };

    // Auto-buff slot (Thunderstorm, Energy Shield, etc.)
    struct BuffSlot {
        bool enabled = false;
        WORD skillId = 0;
        char skillName[32] = {};
        float durationSec = 0;      // total duration from game data
        DWORD lastCastTick = 0;     // when we last cast it
        float recastPct = 0.90f;    // recast at 90% elapsed
    };

    struct Config {
        // Right-click auto-cast
        SkillSlot rightSkill;
        // Left-click auto-cast
        SkillSlot leftSkill;

        // Range and targeting
        int castRange = 25;         // units — default from skill range
        bool useSkillRange = true;  // if true, override castRange with skill's actual range
        TargetPriority priority = TargetPriority::Nearest;

        // Mana management
        int manaReservePct = 20;    // don't cast below this mana %

        // Movement interaction
        bool castWhileMoving = true;

        // Buff slots (user adds/removes, max 6)
        std::vector<BuffSlot> buffs;

        // Cooldown between casts (ms)
        int castCooldownMs = 200;

        // Global enable
        bool enabled = false;
    };

    // Get/set config (thread-safe via copy)
    Config GetConfig();
    void SetConfig(const Config& cfg);

    // Called from GameLoop every frame
    void Update();

    // Initialize (call once at startup)
    void Init();

    // MCP helpers
    void SetEnabled(bool on);
    bool IsEnabled();

    // Set right/left skill from current active skill
    void SetRightFromCurrent();
    void SetLeftFromCurrent();

    // Get available castable skills for dropdowns
    struct SkillInfo {
        WORD id;
        char name[32];
        int level;
    };
    std::vector<SkillInfo> GetAvailableSkills();

    // Merc/summon class IDs to exclude from targeting
    static const DWORD EXCLUDE_CLASS_IDS[] = {271, 338, 359, 560, 561};
    static const int EXCLUDE_COUNT = 5;
}
