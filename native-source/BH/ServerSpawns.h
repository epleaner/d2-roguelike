#pragma once
#include <nlohmann/json.hpp>

struct UnitAny;

namespace ServerSpawns {
    // Called by the owned client queue; generation itself runs in the server callback.
    void EnsureInstalled();
    nlohmann::json Request(const nlohmann::json& args);
    nlohmann::json Result(const nlohmann::json& args);
    nlohmann::json ConfigureRoguelikeZone(const nlohmann::json& args);
    nlohmann::json GetRoguelikeLevels(const nlohmann::json& args);
    void RestoreRoguelikeZones();
    nlohmann::json RunSnapshotStatus();
    void ShutdownMarketPriceHook();
    nlohmann::json ArmRoguelikeRun(const nlohmann::json& args);
    nlohmann::json StartRoguelikeRun(const nlohmann::json& args);
    void OnNaturalKill(void* game, UnitAny* defender);
    void ScaleRoguelikeDamage(void* game, UnitAny* attacker, UnitAny* defender,
        int bMissile, void* damage);
    void ApplyStockMapMonster(void* game, UnitAny* monster, int area);
    int CombatRunDepth(void* game, int area);
    bool PreserveNativeMonsterClass(void* game, int area, int monsterClass);
    void OnPopulationCompleted(int level);
    void OnWorldPopulationCompleted();
    void SettleRunFrame();
    nlohmann::json CreateRoguelikePortal(const nlohmann::json& args);
    nlohmann::json CreateRoguelikeTrigger(const nlohmann::json& args);
    nlohmann::json InteractRoguelikePortal(const nlohmann::json& args);
    nlohmann::json SelectRoguelikeTrigger(const nlohmann::json& args);
    nlohmann::json HireRoguelikeMerc(const nlohmann::json& args);
    nlohmann::json UseRoguelikeScroll(const nlohmann::json& args);
    nlohmann::json UseRoguelikeAura(const nlohmann::json& args);
    nlohmann::json InteractRoguelikeChest(const nlohmann::json& args);
    nlohmann::json InteractEventShrine(const nlohmann::json& args);
    nlohmann::json RetireRoguelikePortals(const nlohmann::json& args);
    nlohmann::json RequestEnemies(const nlohmann::json& args);
    nlohmann::json InspectEnemies(const nlohmann::json& args);
    nlohmann::json InspectArea(const nlohmann::json& args);
    nlohmann::json Observe(const nlohmann::json& args);
    // Called only by the MCP transport worker after Observe has left the game queue.
    nlohmann::json AwaitObservation(const nlohmann::json& args, int timeoutMs);
    nlohmann::json StartTrace(const nlohmann::json& args);
}
