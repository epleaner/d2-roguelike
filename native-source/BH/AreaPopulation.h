#pragma once

#include <nlohmann/json.hpp>
#include <string>

struct UnitAny;
namespace ServerMonsters { enum class RoomClearState; }

namespace AreaPopulation {
// Adds stored, currently inactive monsters without modifying the active census enemies.
// False means population_complete=false; the returned count is only an observed lower bound.
bool AppendInactive(void* game, UnitAny* player, nlohmann::json& census, std::string& error);
bool PrepareRoomClear(std::string& error);
// Game-thread query. No diagnostic census, allocations or retained game pointers.
ServerMonsters::RoomClearState QueryInactiveRoomClear(void* game, UnitAny* player, unsigned expectedLevel);
}
