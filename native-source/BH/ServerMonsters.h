#pragma once

#include <Windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct UnitAny;

namespace ServerMonsters {
constexpr size_t kAttributeCount = 13;
enum class Rank { Normal, Champion, Unique };
struct Enemy {
    int classId;
    int superuniqueId = -1;
    Rank rank = Rank::Normal;
    int count;
    std::array<int, kAttributeCount> values{};
    uint16_t present = 0;
};
enum class Operation { Spawn, Inspect, Area, Observe };
struct Request {
    Operation operation = Operation::Spawn;
    std::vector<Enemy> enemies;
    std::vector<DWORD> unitIds;
    std::string expectedEpoch;
    int encounterLevel = 0; // Internal single-group zone encounter; 0 keeps normal spawns.
};
bool Parse(const nlohmann::json& args, Operation operation, Request& output, std::string& error);
nlohmann::json AttributeSchema();
nlohmann::json Catalog(const nlohmann::json& args);
nlohmann::json Execute(void* game, UnitAny* serverPlayer, const Request& request);
nlohmann::json ObserveSnapshot(void* game, UnitAny* serverPlayer, const Request& request,
    bool includeRuntimeIdentity);
nlohmann::json NaturalCounters();
bool EnableNaturalZone(void* game, const std::string& epoch, int level,
    const std::array<int, 4>& monsterClasses, unsigned monsterClassCount,
    int monsterLevel, std::string& error);
nlohmann::json SpawnEventNpc(void* game, UnitAny* serverPlayer, int classId,
    int x, int y);
void Clear();
}
