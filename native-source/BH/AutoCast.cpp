#include "AutoCast.h"
#include "D2Ptrs.h"
#include "D2Stubs.h"
#include "GameState.h"
#include "GameCallQueue.h"
#include <mutex>
#include <cmath>

namespace AutoCast {

    static Config g_config;
    static std::mutex g_mutex;
    static DWORD g_lastCastTick = 0;
    static WORD g_savedRightSkill = 0;
    static WORD g_savedLeftSkill = 0;

    void Init() {
        g_config = Config();
        g_lastCastTick = 0;
    }

    Config GetConfig() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_config;
    }

    void SetConfig(const Config& cfg) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = cfg;
    }

    void SetEnabled(bool on) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config.enabled = on;
    }

    bool IsEnabled() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_config.enabled;
    }

    // ─── Helpers (all run on game thread) ──────────────────────────────

    static bool IsExcludedClassId(DWORD classId) {
        for (int i = 0; i < EXCLUDE_COUNT; i++)
            if (EXCLUDE_CLASS_IDS[i] == classId) return true;
        return false;
    }

    // Read current right/left skill ID from player unit
    static WORD GetCurrentSkillId(bool left) {
        __try {
            UnitAny* p = D2CLIENT_GetPlayerUnit();
            if (!p || !p->pInfo) return 0;
            Skill* s = left ? p->pInfo->pLeftSkill : p->pInfo->pRightSkill;
            if (s && s->pSkillInfo) return s->pSkillInfo->wSkillId;
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }

    // Switch skill via packet (same as funmixxed SetSkill)
    static void SwitchSkill(WORD skillId, bool left) {
        BYTE packet[9] = {0x3C, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
        *(WORD*)&packet[1] = skillId;
        if (left) packet[4] = 0x80;
        D2NET_SendPacket(9, 0, packet);
    }

    // Cast at location via packet
    static void CastAtLocation(WORD x, WORD y, bool left) {
        BYTE packet[5] = {};
        packet[0] = left ? 0x08 : 0x0F;
        *(WORD*)&packet[1] = x;
        *(WORD*)&packet[3] = y;
        D2NET_SendPacket(5, 0, packet);
    }

    // Check if monster is immune to a damage type
    // Returns true if monster has >= 100 resistance to the skill's element
    static bool IsImmuneToSkill(UnitAny* pMonster, WORD skillId) {
        // Determine element type from skill ID
        // Lightning skills: 38(Charged Bolt), 48(Nova), 49(Lightning), 53(Chain Lightning), 57(Thunder Storm)
        // Fire skills: 36(Fire Bolt), 47(Fire Ball), 56(Meteor), etc.
        // Cold skills: 39(Ice Bolt), 59(Blizzard), 64(Frozen Orb), etc.

        // For now check lightning resistance for lightning skills
        // TODO: expand to all elements using skill data tables
        __try {
            int lightRes = D2COMMON_GetUnitStat(pMonster, 43, 0); // 43 = lightresist
            int fireRes = D2COMMON_GetUnitStat(pMonster, 39, 0);  // 39 = fireresist
            int coldRes = D2COMMON_GetUnitStat(pMonster, 41, 0);  // 41 = coldresist
            int poisRes = D2COMMON_GetUnitStat(pMonster, 45, 0);  // 45 = poisonresist
            int magRes = D2COMMON_GetUnitStat(pMonster, 37, 0);   // 37 = magicresist

            // Lightning skills
            if (skillId == 38 || skillId == 48 || skillId == 49 ||
                skillId == 53 || skillId == 57 || skillId == 42) {
                return lightRes >= 100;
            }
            // Fire skills
            if (skillId == 36 || skillId == 37 || skillId == 47 ||
                skillId == 56 || skillId == 51 || skillId == 46) {
                return fireRes >= 100;
            }
            // Cold skills
            if (skillId == 39 || skillId == 44 || skillId == 45 ||
                skillId == 55 || skillId == 59 || skillId == 64) {
                return coldRes >= 100;
            }
            // Poison skills
            if (skillId == 73 || skillId == 25) {
                return poisRes >= 100;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    // Find the best target monster
    struct Target {
        UnitAny* pUnit = nullptr;
        DWORD unitId = 0;
        WORD x = 0, y = 0;
        int distance = 0;
        int hp = 0;
        bool isBoss = false;
        bool isChampion = false;
    };

    struct FindTargetHelper {
        static bool Run(Target& best, int range, TargetPriority priority, WORD skillId, WORD backupSkillId) {
            __try {
                return RunInner(best, range, priority, skillId, backupSkillId);
            } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        static bool RunInner(Target& best, int range, TargetPriority priority, WORD skillId, WORD backupSkillId) {
            UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
            if (!pPlayer || !pPlayer->pPath) return false;
            int px = pPlayer->pPath->xPos;
            int py = pPlayer->pPath->yPos;

            // Iterate nearby units via rooms
            Room1* pRoom = pPlayer->pPath->pRoom1;
            if (!pRoom) return false;

            // Check units in the current room and nearby rooms
            UnitAny* pUnit = pRoom->pUnitFirst;
            for (; pUnit; pUnit = pUnit->pListNext) {
                if (pUnit->dwType != 1) continue; // only monsters
                if (IsExcludedClassId(pUnit->dwTxtFileNo)) continue;

                // Check alive
                DWORD mode = pUnit->dwMode;
                if (mode == 0 || mode == 12) continue; // dead modes

                // Check flags
                if (pUnit->dwFlags & 0x10000) continue; // invisible/burrowed

                // Get position
                if (!pUnit->pPath) continue;
                int mx = pUnit->pPath->xPos;
                int my = pUnit->pPath->yPos;
                int dx = mx - px, dy = my - py;
                int dist = (int)sqrt((double)(dx*dx + dy*dy));

                if (dist > range) continue;

                // Check immunity
                bool immune = IsImmuneToSkill(pUnit, skillId);
                if (immune && backupSkillId > 0) {
                    immune = IsImmuneToSkill(pUnit, backupSkillId);
                }
                if (immune) continue; // skip fully immune

                // Get HP for priority
                int hp = D2COMMON_GetUnitStat(pUnit, 6, 0); // stat 6 = hitpoints
                bool boss = (pUnit->dwFlags & 0x02) != 0;      // super unique
                bool champ = (pUnit->dwFlags & 0x04) != 0;     // champion

                // Evaluate based on priority
                bool better = false;
                switch (priority) {
                    case TargetPriority::Nearest:
                        better = dist < best.distance;
                        break;
                    case TargetPriority::LowestHP:
                        better = (best.pUnit == nullptr) || (hp < best.hp);
                        break;
                    case TargetPriority::HighestHP:
                        better = (best.pUnit == nullptr) || (hp > best.hp);
                        break;
                    case TargetPriority::BossPriority:
                        if (boss && !best.isBoss) better = true;
                        else if (champ && !best.isChampion && !best.isBoss) better = true;
                        else if ((boss == best.isBoss) && (champ == best.isChampion))
                            better = dist < best.distance;
                        break;
                }

                if (better) {
                    best.pUnit = pUnit;
                    best.unitId = pUnit->dwUnitId;
                    best.x = (WORD)mx;
                    best.y = (WORD)my;
                    best.distance = dist;
                    best.hp = hp;
                    best.isBoss = boss;
                    best.isChampion = champ;
                }
            }
            return true;
        }
    };

    static Target FindTarget(int range, TargetPriority priority, WORD skillId, WORD backupSkillId) {
        Target best;
        best.distance = range + 1;
        FindTargetHelper::Run(best, range, priority, skillId, backupSkillId);
        return best;
    }

    // ─── Quick Cast (switch → cast → switch back) ─────────────────────

    static void QuickCast(WORD skillId, WORD targetX, WORD targetY, bool left) {
        WORD currentSkill = GetCurrentSkillId(left);

        if (currentSkill != skillId) {
            SwitchSkill(skillId, left);
        }

        CastAtLocation(targetX, targetY, left);

        // Switch back to previous skill
        if (currentSkill != skillId && currentSkill != 0) {
            SwitchSkill(currentSkill, left);
        }
    }

    // ─── Auto-Buff Logic ──────────────────────────────────────────────

    static void UpdateBuffs(Config& cfg) {
        DWORD now = GetTickCount();

        for (auto& buff : cfg.buffs) {
            if (!buff.enabled || buff.skillId == 0) continue;
            if (buff.durationSec <= 0) continue;

            DWORD elapsed = now - buff.lastCastTick;
            DWORD thresholdMs = (DWORD)(buff.durationSec * buff.recastPct * 1000.0f);

            if (buff.lastCastTick == 0 || elapsed >= thresholdMs) {
                // Quick-cast the buff (right click)
                QuickCast(buff.skillId, 0, 0, false);
                buff.lastCastTick = now;
            }
        }
    }

    // ─── Main Update (called from GameLoop every frame) ───────────────

    void Update() {
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            cfg = g_config;
        }

        if (!cfg.enabled) return;
        if (!GameState::IsGameReady()) return;

        // Check mana reserve
        auto ps = GameState::GetPlayerState();
        if (!ps.valid) return;
        if (ps.maxMana > 0) {
            int manaPct = (ps.mana * 100) / ps.maxMana;
            if (manaPct < cfg.manaReservePct) return;
        }

        // Check if in town — don't auto-cast in town
        static const int TOWN_AREAS[] = {1, 40, 75, 103, 109};
        for (int i = 0; i < 5; i++) {
            if (ps.area == TOWN_AREAS[i]) {
                // Still update buffs in town
                UpdateBuffs(cfg);
                // Write back buff timestamps
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_config.buffs = cfg.buffs;
                }
                return;
            }
        }

        // Update buffs
        UpdateBuffs(cfg);

        // Check cooldown
        DWORD now = GetTickCount();
        if ((int)(now - g_lastCastTick) < cfg.castCooldownMs) return;

        // Check cast-while-moving setting
        if (!cfg.castWhileMoving) {
            struct MoveCheck {
                static bool IsMoving() {
                    __try {
                        UnitAny* p = D2CLIENT_GetPlayerUnit();
                        if (p && (p->dwMode == 2 || p->dwMode == 3 || p->dwMode == 5))
                            return true;
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    return false;
                }
            };
            if (MoveCheck::IsMoving()) return;
        }

        // Try right-click auto-cast
        if (cfg.rightSkill.enabled && cfg.rightSkill.skillId > 0) {
            int range = cfg.useSkillRange ? cfg.castRange : cfg.castRange;
            Target target = FindTarget(range, cfg.priority,
                                        cfg.rightSkill.skillId, cfg.rightSkill.backupSkillId);

            if (target.pUnit) {
                // Determine which skill to use
                WORD useSkill = cfg.rightSkill.skillId;
                if (IsImmuneToSkill(target.pUnit, useSkill) && cfg.rightSkill.backupSkillId > 0) {
                    useSkill = cfg.rightSkill.backupSkillId;
                }

                QuickCast(useSkill, target.x, target.y, false);
                g_lastCastTick = now;
            }
        }

        // Try left-click auto-cast
        if (cfg.leftSkill.enabled && cfg.leftSkill.skillId > 0) {
            int range = cfg.castRange;
            Target target = FindTarget(range, cfg.priority,
                                        cfg.leftSkill.skillId, cfg.leftSkill.backupSkillId);

            if (target.pUnit) {
                WORD useSkill = cfg.leftSkill.skillId;
                if (IsImmuneToSkill(target.pUnit, useSkill) && cfg.leftSkill.backupSkillId > 0) {
                    useSkill = cfg.leftSkill.backupSkillId;
                }

                QuickCast(useSkill, target.x, target.y, true);
                g_lastCastTick = now;
            }
        }

        // Write back buff timestamps
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_config.buffs = cfg.buffs;
        }
    }

    // ─── Helpers for UI/MCP ───────────────────────────────────────────

    void SetRightFromCurrent() {
        std::lock_guard<std::mutex> lock(g_mutex);
        WORD id = GetCurrentSkillId(false);
        if (id > 0) {
            g_config.rightSkill.skillId = id;
            g_config.rightSkill.enabled = true;
        }
    }

    void SetLeftFromCurrent() {
        std::lock_guard<std::mutex> lock(g_mutex);
        WORD id = GetCurrentSkillId(true);
        if (id > 0) {
            g_config.leftSkill.skillId = id;
            g_config.leftSkill.enabled = true;
        }
    }

    std::vector<SkillInfo> GetAvailableSkills() {
        std::vector<SkillInfo> result;
        struct Helper {
            static void Run(std::vector<SkillInfo>& out) {
                __try {
                    UnitAny* p = D2CLIENT_GetPlayerUnit();
                    if (!p || !p->pInfo) return;
                    for (Skill* s = p->pInfo->pFirstSkill; s; s = s->pNextSkill) {
                        if (!s->pSkillInfo) continue;
                        WORD id = s->pSkillInfo->wSkillId;
                        if (id <= 5) continue;
                        SkillInfo info;
                        info.id = id;
                        info.level = 1; // base level
                        snprintf(info.name, sizeof(info.name), "Skill %d", id);
                        out.push_back(info);
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        };
        Helper::Run(result);
        return result;
    }
}
