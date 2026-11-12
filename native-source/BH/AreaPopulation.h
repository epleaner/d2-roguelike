#pragma once

#include <nlohmann/json.hpp>
#include <string>

struct UnitAny;

namespace AreaPopulation {
// Adds stored, currently inactive monsters without modifying the active census enemies.
// False means population_complete=false; the returned count is only an observed lower bound.
bool AppendInactive(void* game, UnitAny* player, nlohmann::json& census, std::string& error);
}
