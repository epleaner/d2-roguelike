#pragma once

#include <nlohmann/json.hpp>

namespace ItemCommerce {
nlohmann::json OpenShop(const nlohmann::json& args);
nlohmann::json ShopState(const nlohmann::json& args);
nlohmann::json Buy(const nlohmann::json& args);
}
