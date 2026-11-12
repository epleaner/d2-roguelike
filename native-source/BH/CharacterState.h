#pragma once

#include "../ThirdParty/nlohmann/json.hpp"
#include <cstdint>
#include <string>

// Called on the client game thread, never from the RPC worker.
struct UnitAny;
namespace CharacterState {
    enum class ProgressionField { Level, StatPoints, SkillPoints };
    enum class DeathMode { Unknown, Softcore, Hardcore };
    bool BindingsReady();
    bool SetProgression(void* game, UnitAny* player, ProgressionField field, uint32_t value);
    DeathMode GetDeathMode(UnitAny* player);
    bool ResetToLevelOne(void* game, UnitAny* player, std::string& error);
    bool RestoreLevelOneLife(void* game, UnitAny* player, std::string& error);
    bool AwardClearLevel(void* game, UnitAny* player);
    nlohmann::json Catalog(const nlohmann::json& args);
    nlohmann::json Inspect(const nlohmann::json& args);
}
