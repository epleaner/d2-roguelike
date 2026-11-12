#pragma once
#include <nlohmann/json.hpp>

struct UnitAny;

namespace SetItems {
    nlohmann::json Catalog(const nlohmann::json& args);
    // Fields to merge into the ordinary item description for quality-five items.
    nlohmann::json Describe(UnitAny* item);
}
