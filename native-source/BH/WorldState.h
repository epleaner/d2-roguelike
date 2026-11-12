#pragma once

#include "../ThirdParty/nlohmann/json.hpp"

struct UnitAny;

// Live, on-demand reads. Call only on the client engine thread, while in game.
namespace WorldState {
    nlohmann::json Units(const nlohmann::json& args);
    nlohmann::json Map(const nlohmann::json& args);
    nlohmann::json Catalog(const nlohmann::json& args);
    nlohmann::json Inventory(const nlohmann::json& args);
    nlohmann::json ItemStats(const nlohmann::json& args);
    nlohmann::json Ui(const nlohmann::json& args);
    nlohmann::json DescribeItem(UnitAny* item);
}
