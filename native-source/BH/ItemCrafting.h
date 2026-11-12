#pragma once

#include <nlohmann/json.hpp>

struct UnitAny;

// Engine-thread-only actions. Success denotes a single requested native action,
// never observed server acceptance. WorldState verifies the subsequent item state.
namespace ItemCrafting {
    nlohmann::json Catalog(const nlohmann::json& args);
    nlohmann::json Socket(const nlohmann::json& args);
    nlohmann::json Cube(const nlohmann::json& args);
    nlohmann::json DescribeSockets(UnitAny* item);
}
