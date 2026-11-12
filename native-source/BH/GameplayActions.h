#pragma once

#include "../ThirdParty/nlohmann/json.hpp"

// Caller dispatches these operations through GameCallQueue on the client game thread.
// A successful action means the native request was issued, not that the server applied it.
namespace GameplayActions {
    nlohmann::json Move(const nlohmann::json& args);             // {"x":1..65535,"y":1..65535,"mode":"run"|"walk"?}
    nlohmann::json Approach(const nlohmann::json& args);         // {"unit_id":uint32,"unit_type":1|2,"mode":"run"|"walk"?}
    nlohmann::json Attack(const nlohmann::json& args);           // {"unit_id":uint32,"skill_id":uint16?,"hand":"left"|"right"?}; absent skill = basic attack
    nlohmann::json Cast(const nlohmann::json& args);             // {"skill_id":uint16,"hand":"left"|"right",("x","y")|unit_id}
    nlohmann::json InteractNpc(const nlohmann::json& args);      // {"unit_id":uint32}
    nlohmann::json CloseInteraction(const nlohmann::json& args); // {}
    nlohmann::json NpcState(const nlohmann::json& args);        // {}
    void TickNpc();  // Complete approach and interaction in one engine frame.
    void ResetNpc(); // Cancel previous-game actions at the menu boundary.
}
