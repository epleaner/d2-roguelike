#pragma once

#include <nlohmann/json.hpp>
#include <string>

struct UnitAny;

namespace CombatTelemetry {
// Start runs on the owned nonce server callback, with its real game/player context.
nlohmann::json Start(void* game, UnitAny* serverPlayer, const nlohmann::json& args);
using KillListener = void (*)(void*, UnitAny*);
using DamageListener = void (*)(void*, UnitAny*, UnitAny*, int, void*);
bool ListenForRoguelikeKills(KillListener listener, DamageListener damage,
    std::string& error);
void StopRoguelikeKills();
// Readers are bounded, read-only and never dereference native unit pointers.
nlohmann::json Read(const nlohmann::json& args);
nlohmann::json Stop(const nlohmann::json& args);
// Called on actual game leave and on module shutdown; idempotent.
void Shutdown();
}
