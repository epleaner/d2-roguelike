#pragma once

#include <nlohmann/json.hpp>
#include <string>

struct UnitAny;

namespace ServerMonsters { struct Request; }
namespace ObservationTelemetry {
// Called by the real game-leave transition before a subsequent game can reuse its address.
void Invalidate();
// Compare against the last observed server session; never mint or refresh an epoch.
bool MatchesEpoch(const std::string& expectedEpoch);
// Server callback additionally proves the cached game/player still own the nonce.
bool MatchesContext(const std::string& expectedEpoch, void* game, UnitAny* serverPlayer);
// Called only from the owned nonce server callback, never from the client request thread.
nlohmann::json Capture(void* game, UnitAny* serverPlayer, const ServerMonsters::Request& request);
}
