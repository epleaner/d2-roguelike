#pragma once

#include <nlohmann/json.hpp>

struct Inventory;
// Called only on the client game thread (via GameCallQueue). Requests report
// packet submission, not completion; compare subsequent WorldState receipts.
namespace InventoryActions {
    bool GridSize(Inventory* inventory, unsigned storage, unsigned& width, unsigned& height);
    nlohmann::json OpenStash(const nlohmann::json& args);
    nlohmann::json CloseStash(const nlohmann::json& args);
    nlohmann::json Pickup(const nlohmann::json& args);
    nlohmann::json Drop(const nlohmann::json& args);
    nlohmann::json Move(const nlohmann::json& args);
    nlohmann::json Stash(const nlohmann::json& args);
    nlohmann::json CubeMove(const nlohmann::json& args);
}
