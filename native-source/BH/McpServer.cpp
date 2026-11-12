// Must include winsock2 before windows.h to avoid v1/v2 conflict
#include <winsock2.h>
#include <ws2tcpip.h>
#define CPPHTTPLIB_NO_EXCEPTIONS
#include "McpServer.h"
#include "GameState.h"
#include "GameCallQueue.h"
#include "LabProfile.h"
#include "D2Ptrs.h"
#include "D2Stubs.h"
#include "GameplayActions.h"
#include "InventoryActions.h"
#include "WorldState.h"
#include "ServerSpawns.h"
#include "ServerMonsters.h"
#include "ObservationTelemetry.h"
#include "CombatTelemetry.h"
#include "CharacterState.h"
#include "ItemCommerce.h"
#include "ItemCrafting.h"
#include <windows.h>
#include <thread>
#include <atomic>
#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <map>
#include <algorithm>
#include <functional>
#include <chrono>
#include <iterator>
#include <cstdlib>
#include "../ThirdParty/httplib.h"
#include "../ThirdParty/nlohmann/json.hpp"

using json = nlohmann::json;

namespace {
constexpr int kMaxControls = 128;
constexpr int kMaxTextNodes = 32;
constexpr int kMaxTextLines = 32;
constexpr int kMaxTextChars = 255;
constexpr char kVersion[] = "2024-11-05";
std::thread g_thread;
std::atomic<bool> g_running{false};
std::atomic<bool> g_shouldRun{false};
std::atomic<int> g_requestCount{0};
std::atomic<int> g_nextSessionId{1};
std::atomic<bool> g_offlineMenuChosen{false};
DWORD g_processId = 0;
DWORD g_processSession = 0;
std::string g_processStartFiletime;
std::atomic<bool> g_copyrightKeyAttempted{false};
int g_port = 21337;
std::atomic<httplib::Server*> g_server{nullptr};

// Reacquire Winsock for each listener lifetime: D2 teardown can invalidate shared
// sockets. Release the lease only after the server and workers are destroyed.
struct SocketLease {
    bool valid;
    SocketLease() {
        WSADATA data;
        valid = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketLease() { if (valid) WSACleanup(); }
};

struct Session {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::string> messages;
    bool closed = false;
};
std::mutex g_sessionsMutex;
std::map<int, std::shared_ptr<Session>> g_sessions;

void SendSseMessage(int id, const std::string& data) {
    std::lock_guard<std::mutex> lock(g_sessionsMutex);
    auto it = g_sessions.find(id);
    if (it == g_sessions.end()) return;
    auto session = it->second;
    std::lock_guard<std::mutex> slock(session->mutex);
    session->messages.push("event: message\ndata: " + data + "\n\n");
    session->cv.notify_one();
}

bool Authorized(const httplib::Request& req, httplib::Response& res) {
    const std::string& token = LabProfile::Token();
    const std::string header = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    size_t diff = header.size() ^ (prefix.size() + token.size());
    // Do not disclose the token (or even a prefix) in a response or a log.
    for (size_t i = 0; i < prefix.size() + token.size(); ++i) {
        unsigned char actual = i < header.size() ? static_cast<unsigned char>(header[i]) : 0;
        unsigned char expected = i < prefix.size() ? static_cast<unsigned char>(prefix[i])
            : static_cast<unsigned char>(token[i - prefix.size()]);
        diff |= actual ^ expected;
    }
    if (token.size() != 64 || diff) {
        res.status = 401;
        res.set_content("Unauthorized", "text/plain");
        return false;
    }
    return true;
}

json Result(const json& body, bool error = false) {
    json result = {{"content", json::array({{{"type", "text"}, {"text", body.dump()}}})}};
    if (error) result["isError"] = true;
    return result;
}
json Error(const std::string& code) { return Result({{"status", code}}, true); }

// The queue owns each closure; every captured argument and result must survive a timeout.
json OnGameThread(std::function<json()> action) {
    auto result = std::make_shared<json>();
    auto dispatch = GameCallQueue::Invoke([action = std::move(action), result]() {
        *result = action();
    });
    switch (dispatch) {
        case GameCallQueue::DispatchResult::Completed: return *result;
        case GameCallQueue::DispatchResult::Cancelled: return Error("cancelled");
        case GameCallQueue::DispatchResult::Unconfirmed: return Error("unconfirmed");
        default: return Error("failed");
    }
}

// Only raw, fixed-size POD data is handled in SEH regions. Never traverse untrusted
// native pointers in an HTTP thread, or put a C++ object with a destructor in __try.
bool ReadControl(Control* p, Control& copy) {
    __try { copy = *p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadTextNode(ControlText* p, ControlText& copy) {
    __try { copy = *p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadTextHead(TextBox* p, ControlText*& first) {
    __try { first = p->pFirstText; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadWide(const wchar_t* p, wchar_t (&out)[kMaxTextChars + 1]) {
    if (!p) return false;
    __try {
        for (int i = 0; i < kMaxTextChars; ++i) {
            out[i] = p[i];
            if (!out[i]) return true;
        }
        out[kMaxTextChars] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string Utf8(const wchar_t* source) {
    wchar_t wide[kMaxTextChars + 1] = {};
    if (!ReadWide(source, wide)) return {};
    char utf8[(kMaxTextChars + 1) * 4] = {};
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return length > 0 ? std::string(utf8, length - 1) : std::string();
}

bool ReadFirstControl(Control*& p) {
    __try { p = *p_D2WIN_FirstControl; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadControls(Control* (&controls)[kMaxControls], int& count) {
    count = 0;
    Control* p = nullptr;
    if (!ReadFirstControl(p)) return false;
    while (p) {
        if (count == kMaxControls || std::find(controls, controls + count, p) != controls + count) return false;
        controls[count++] = p;
        Control copy = {};
        if (!ReadControl(p, copy)) return false;
        p = copy.pNext;
    }
    return true;
}

bool ReadLines(Control* p, json& lines) {
    lines = json::array();
    Control copy = {};
    if (!ReadControl(p, copy)) return false;
    if (copy.dwType == 6 || copy.dwType == 1) {
        // S13 buttons use inline UTF-16 at +0x64; editboxes at +0x5c.
        const char* base = reinterpret_cast<const char*>(p);
        std::string text = Utf8(reinterpret_cast<const wchar_t*>(base + (copy.dwType == 6 ? 0x64 : 0x5c)));
        if (!text.empty()) lines.push_back(text);
    } else if (copy.dwType == 4) {
        ControlText* node = nullptr;
        if (!ReadTextHead(reinterpret_cast<TextBox*>(p), node)) return false;
        ControlText* visited[kMaxTextNodes] = {};
        int count = 0;
        while (node) {
            if (count == kMaxTextNodes || std::find(visited, visited + count, node) != visited + count) return false;
            visited[count++] = node;
            ControlText entry = {};
            if (!ReadTextNode(node, entry)) return false;
            for (int field = 0; field < 5 && lines.size() < kMaxTextLines; ++field) {
                std::string text = Utf8(entry.wText[field]);
                if (!text.empty()) lines.push_back(text);
            }
            node = entry.pNext;
        }
    }
    return true;
}

bool Enabled(const Control& c) { return (c.dwState & 0x5) == 0x5; }

std::string ScreenState(bool hasControls) {
    if (GameState::IsGameReady()) return "in_game";
    if (hasControls) return "menu";
    return "loading";
}

// Offline menu labels only. Unknown or online/multiplayer controls cannot be activated.
bool OfflineButton(const std::string& label) {
    static const char* const labels[] = {
        "Single Player", "SINGLE PLAYER", "Back", "BACK", "Cancel", "CANCEL",
        "OK", "Normal", "Nightmare", "Hell", "Play", "PLAY",
        "Exit Diablo II", "EXIT DIABLO II", "Quit", "QUIT"
    };
    return std::find_if(std::begin(labels), std::end(labels),
        [&label](const char* allowed) { return label == allowed; }) != std::end(labels);
}

bool Press(Control* p, const Control& c) {
    if (!c.OnPress) return false;
    __try { c.OnPress(p); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SelectCharacterRow(Control* p, const Control& c) {
    // Pinned S13 D2Launch character rows take ControlMsg*, unlike button callbacks.
    HMODULE launch = GetModuleHandleA("D2Launch.dll");
    if (!launch || reinterpret_cast<uintptr_t>(c.OnPress) !=
        reinterpret_cast<uintptr_t>(launch) + 0xE140) return false;
    // Release selects without the callback's button-down double-click activation.
    ControlMsg message = {p, WM_LBUTTONUP, 0};
    auto select = reinterpret_cast<BOOL (__stdcall*)(ControlMsg*)>(c.OnPress);
    __try { return select(&message) != FALSE; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

using GameplayCall = json (*)(const json&);
GameplayCall GameplayHandler(const std::string& name) {
    struct Binding { const char* name; GameplayCall call; };
    static constexpr Binding bindings[] = {
        {"move_player", GameplayActions::Move}, {"attack_unit", GameplayActions::Attack},
        {"approach_unit", GameplayActions::Approach},
        {"cast_skill", GameplayActions::Cast},
        {"get_character_catalog", CharacterState::Catalog},
        {"get_build_state", CharacterState::Inspect},
        {"get_npc_state", GameplayActions::NpcState}, {"interact_npc", GameplayActions::InteractNpc},
        {"close_npc", GameplayActions::CloseInteraction}, {"get_units", WorldState::Units},
        {"get_map", WorldState::Map}, {"get_inventory", WorldState::Inventory},
        {"get_ui", WorldState::Ui}, {"get_item_catalog", WorldState::Catalog},
        {"pickup_item", InventoryActions::Pickup}, {"drop_item", InventoryActions::Drop},
        {"move_item", InventoryActions::Move}, {"open_stash", InventoryActions::OpenStash},
        {"close_stash", InventoryActions::CloseStash}, {"stash_item", InventoryActions::Stash},
        {"spawn_item", ServerSpawns::Request}, {"get_spawn_result", ServerSpawns::Result},
        {"start_roguelike_run", ServerSpawns::StartRoguelikeRun},
        {"arm_roguelike_run", ServerSpawns::ArmRoguelikeRun},
        {"configure_roguelike_zone", ServerSpawns::ConfigureRoguelikeZone},
        {"get_roguelike_levels", ServerSpawns::GetRoguelikeLevels},
        {"create_roguelike_portal", ServerSpawns::CreateRoguelikePortal},
        {"interact_roguelike_portal", ServerSpawns::InteractRoguelikePortal},
        {"create_roguelike_trigger", ServerSpawns::CreateRoguelikeTrigger},
        {"select_roguelike_trigger", ServerSpawns::SelectRoguelikeTrigger},
        {"hire_roguelike_merc", ServerSpawns::HireRoguelikeMerc},
        {"use_roguelike_scroll", ServerSpawns::UseRoguelikeScroll},
        {"use_roguelike_aura", ServerSpawns::UseRoguelikeAura},
        {"interact_roguelike_chest", ServerSpawns::InteractRoguelikeChest},
        {"interact_event_shrine", ServerSpawns::InteractEventShrine},
        {"retire_roguelike_portals", ServerSpawns::RetireRoguelikePortals},
        {"spawn_enemies", ServerSpawns::RequestEnemies},
        {"get_enemy_state", ServerSpawns::InspectEnemies},
        {"get_area_state", ServerSpawns::InspectArea},
        {"start_combat_trace", ServerSpawns::StartTrace},
        {"open_shop", ItemCommerce::OpenShop}, {"get_shop_state", ItemCommerce::ShopState},
        {"buy_item", ItemCommerce::Buy}, {"get_item_stats", WorldState::ItemStats},
        {"get_crafting_catalog", ItemCrafting::Catalog}, {"socket_item", ItemCrafting::Socket},
        {"cube_action", ItemCrafting::Cube}, {"move_cube_item", InventoryActions::CubeMove},
        {"get_monster_catalog", ServerMonsters::Catalog}
    };
    for (const auto& binding : bindings) if (name == binding.name) return binding.call;
    return nullptr;
}

json ToolsList() {
    json tools = json::array();
    auto add = [&tools](const char* name, const char* description, json properties, json required) {
        tools.push_back({{"name", name}, {"description", description},
            {"inputSchema", {{"type", "object"}, {"properties", properties}, {"required", required}}}});
    };
    add("ping", "Test the authenticated native lab connection.", json::object(), json::array());
    add("get_game_state", "Read loading, menu, or in-game state from the native toolkit.", json::object(), json::array());
    add("get_player_state", "Read the in-game player snapshot.", json::object(), json::array());
    add("get_controls", "Read bounded native UI controls and their labels.", json::object(), json::array());
    add("skip_copyright", "Queue one Escape down/up only to the owned game window on the exact verified copyright screen; never change foreground focus.", json::object(), json::array());
    add("click_control", "Press an enabled offline menu control matching the observed index, type and label.",
        {{"index", {{"type", "integer"}}}, {"expected_type", {{"type", "integer"}}},
         {"expected_label", {{"type", "string"}}}},
        json::array({"index", "expected_type", "expected_label"}));
    add("select_character", "Select an offline character slot whose displayed name exactly matches the requested name.",
        {{"name", {{"type", "string"}}}}, json::array({"name"}));
    add("exit_game", "Exit the current offline game normally.", json::object(), json::array());
    add("quit_game", "Request normal close of the owned game window.", json::object(), json::array());
    const json integer = {{"type", "integer"}}, text = {{"type", "string"}};
    add("get_character_catalog", "Read exact season class growth, experience thresholds and saved-skill slot metadata.",
        {{"class_id", {{"type", "integer"}, {"minimum", 0}, {"maximum", 6}}}}, json::array({"class_id"}));
    add("get_build_state", "Read native base/effective attributes, hard/effective skills, vitals and typed combat stats.",
        json::object(), json::array());
    add("cast_skill", "Submit one owned skill at coordinates or an observed unit without melee approach; observe its effect.",
        {{"skill_id", integer}, {"hand", {{"enum", {"left", "right"}}}},
         {"x", integer}, {"y", integer}, {"unit_id", integer}}, json::array({"skill_id", "hand"}));
    add("move_player", "Request offline world-tile movement; observe the player position afterwards.",
        {{"x", integer}, {"y", integer}, {"mode", {{"enum", {"walk", "run"}}}}}, json::array({"x", "y"}));
    add("approach_unit", "Follow an observed nearby monster/NPC or object with native unit-target movement.",
        {{"unit_id", integer}, {"unit_type", {{"enum", {1, 2}}}},
         {"mode", {{"enum", {"walk", "run"}}}}}, json::array({"unit_id", "unit_type"}));
    add("attack_unit", "Attack a live hostile monster using an owned skill; observe HP or death.",
        {{"unit_id", integer}, {"skill_id", integer}, {"hand", {{"enum", {"left", "right"}}}}}, json::array({"unit_id"}));
    add("get_units", "Read bounded actual current-area monsters, NPCs, objects and ground items.",
        {{"radius", integer}, {"max_units", integer}, {"max_rooms", integer},
         {"objects_only", {{"type", "boolean"}}}}, json::array());
    add("get_map", "Read loaded collision and generated room/exit metadata without loading unseen rooms.",
        {{"max_rooms", integer}, {"max_cells", integer},
         {"current_level_only", {{"type", "boolean"}}}}, json::array());
    add("get_inventory", "Read actual player items and cursor with identities, locations and native properties.",
        {{"max_items", integer}}, json::array());
    add("get_ui", "Read actual in-game panel flags.", json::object(), json::array());
    add("get_npc_state", "Read actual current NPC interaction and shop/menu flags.", json::object(), json::array());
    add("interact_npc", "Request interaction with a live NPC in the current area.",
        {{"unit_id", integer}}, json::array({"unit_id"}));
    add("close_npc", "Close the native NPC interaction normally.", json::object(), json::array());
    add("pickup_item", "Request a real ground item onto the empty cursor.",
        {{"item_id", integer}, {"expected_epoch", text}}, json::array({"item_id"}));
    add("drop_item", "Drop the specified cursor item onto actual ground.",
        {{"item_id", integer}}, json::array({"item_id"}));
    add("move_item", "Perform one item cursor or placement transition; observe it before another action.",
        {{"item_id", integer}, {"destination", {{"enum", {"cursor", "inventory", "equipment"}}}},
         {"x", integer}, {"y", integer}, {"slot", integer}, {"expected_epoch", text}}, json::array({"item_id", "destination"}));
    add("open_stash", "Interact with a nearby live town stash object.",
        {{"object_id", integer}}, json::array());
    add("close_stash", "Close the native stash interaction with an empty cursor.", json::object(), json::array());
    add("stash_item", "Deposit or retrieve an item on the currently visible stash page.",
        {{"item_id", integer}, {"direction", {{"enum", {"deposit", "retrieve"}}}},
         {"tab", {{"enum", {"current"}}}}, {"x", integer}, {"y", integer}, {"expected_epoch", text}},
        json::array({"item_id", "direction", "tab"}));
    add("open_shop", "Open the real trade menu of the currently interacting vendor.",
        {{"npc_id", integer}}, json::array({"npc_id"}));
    add("get_shop_state", "Read bounded native vendor stock, quoted buy prices and player gold.",
        {{"max_items", integer}}, json::array());
    add("buy_item", "Submit one real native purchase within an explicit quoted-price cap; observe item and gold.",
        {{"npc_id", integer}, {"item_id", integer}, {"max_price", integer}},
        json::array({"npc_id", "item_id", "max_price"}));
    add("get_item_stats", "Read selected native computed stat values of an owned item, including socket/runeword effects.",
        {{"item_id", integer}, {"stats", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
            {"items", {{"type", "object"}, {"properties", {{"stat_id", integer}, {"layer", integer}}},
                {"required", json::array({"stat_id"})}}}}}}, json::array({"item_id", "stats"}));
    add("get_crafting_catalog", "Page actual season runeword and cube recipe table records.",
        {{"table", {{"enum", {"runewords", "cube_recipes"}}}}, {"offset", integer}, {"limit", integer},
         {"base_item_id", integer}},
        json::array({"table"}));
    add("socket_item", "Insert one owned cursor gem/rune into an owned target via the native socket action.",
        {{"socketable_id", integer}, {"target_id", integer}}, json::array({"socketable_id", "target_id"}));
    add("cube_action", "Open/close the owned Horadric Cube or submit one real native transmutation.",
        {{"action", {{"enum", {"open", "close", "transmute"}}}}}, json::array({"action"}));
    add("move_cube_item", "Deposit/retrieve one item in the actual owned cube grid.",
        {{"item_id", integer}, {"direction", {{"enum", {"deposit", "retrieve"}}}}, {"x", integer}, {"y", integer},
         {"expected_epoch", text}},
        json::array({"item_id", "direction"}));
    add("get_item_catalog", "Page exact loaded-season tables; bases also support 1-8 exact codes with complete loaded-table coverage, explicit missing and ambiguous codes (no pagination with codes).",
        {{"table", {{"enum", {"bases", "uniques", "sets", "affixes"}}}}, {"offset", integer}, {"limit", integer},
         {"codes", {{"type", "array"}, {"minItems", 1}, {"maxItems", 8}, {"uniqueItems", true},
             {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 4}}}}}},
        json::array({"table"}));
    add("spawn_item", "Generate an exact requested item in the offline server callback; query its correlated result and ground appearance.",
        {{"request_id", text}, {"expected_epoch", text}, {"code", text}, {"quality", {{"enum", {"normal", "magic", "rare", "unique", "set"}}}},
         {"unique_id", integer}, {"unique_name", text}, {"set_item_id", integer}, {"set_item_name", text},
         {"item_level", integer}, {"seed", integer},
         {"prefixes", {{"type", "array"}, {"items", integer}}},
         {"suffixes", {{"type", "array"}, {"items", integer}}},
         {"stats", {{"type", "array"}, {"items", {{"type", "object"},
             {"properties", {{"stat_id", integer}, {"layer", integer}, {"value", integer}}},
             {"required", json::array({"stat_id", "value"})}}}}}},
        json::array({"request_id", "quality"}));
    add("get_spawn_result", "Read the correlated server generation result without reissuing the mutation.",
        {{"request_id", text}}, json::array({"request_id"}));
    add("start_roguelike_run", "Open one unlinked, labeled Tower Arena entrance in Rogue Encampment; manual in-game click starts the native run.",
        {{"request_id", text}, {"expected_epoch", text}},
        json::array({"request_id", "expected_epoch"}));
    add("arm_roguelike_run", "Arm game-owned full-clear offers in the Tower after entering from town; retire the initial pair and inspect the correlated result.",
        {{"request_id", text}, {"expected_epoch", text}},
        json::array({"request_id", "expected_epoch"}));
    add("configure_roguelike_zone", "Configure an unused cave's natural monster, level, density and drop-quality magic find. Query get_spawn_result.",
        {{"request_id", text}, {"expected_epoch", text},
         {"to_level", {{"type", "integer"}, {"minimum", 13}, {"maximum", 254}}}, {"monster_class", integer},
         {"monster_level", integer}, {"density", integer}, {"magic_find", integer}},
        json::array({"request_id", "expected_epoch", "to_level", "monster_class",
                     "monster_level", "density", "magic_find"}));
    add("get_roguelike_levels", "Inspect arena 13..16,138..254 Room2 freshness and source monster region without generating rooms.",
        {{"request_id", text}, {"expected_epoch", text}},
        json::array({"request_id", "expected_epoch"}));
    add("create_roguelike_portal", "Request one offline server portal object; inspect get_spawn_result without replay.",
        {{"request_id", text}, {"expected_epoch", text},
         {"from_level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 254}}}, {"to_level", {{"type", "integer"}, {"minimum", 13}, {"maximum", 254}}}},
        json::array({"request_id", "expected_epoch", "from_level", "to_level"}));
    add("interact_roguelike_portal", "Submit one verified portal interaction; observe actual destination separately.",
        {{"request_id", text}, {"expected_epoch", text},
         {"from_level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 254}}}, {"object_id", integer}},
        json::array({"request_id", "expected_epoch", "from_level", "object_id"}));
    add("create_roguelike_trigger", "Create a deterministic test-zone choice without changing natural zone rolls.",
        {{"request_id", text}, {"expected_epoch", text},
         {"from_level", {{"type", "integer"}, {"minimum", 13}, {"maximum", 254}}},
         {"to_level", {{"type", "integer"}, {"minimum", 13}, {"maximum", 254}}},
         {"monster_class", integer}, {"monster_level", integer}, {"density", integer},
         {"magic_find", integer},
         {"monster_classes", {{"type", "array"}, {"minItems", 1},
             {"maxItems", 4}, {"items", {{"type", "integer"},
                 {"minimum", 0}, {"maximum", 32767}}}}},
         {"run_level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 99}}},
         {"zone_type", {{"type", "string"}, {"enum", {"monster", "boss", "merchant", "shrine", "chest"}}}},
         {"boss_superunique_id", {{"type", "integer"}, {"minimum", 0}, {"maximum", 32767}}},
         {"reward_focus", {{"type", "string"}, {"enum", {"equipment", "socketables", "unique", "set", "jewelry", "charms"}}}}},
        json::array({"request_id", "expected_epoch", "from_level", "to_level",
                     "monster_class", "monster_level", "density", "magic_find"}));
    add("select_roguelike_trigger", "Select an owned standalone arena choice in reach; server applies its roll and materializes only this destination.",
        {{"request_id", text}, {"expected_epoch", text},
         {"from_level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 254}}},
         {"object_id", integer}}, json::array({"request_id", "expected_epoch", "from_level", "object_id"}));
    add("hire_roguelike_merc", "Request one native Kashya hire from a server-observed available name in the active market; verify mercenary and gold separately.",
        {{"request_id", text}, {"expected_epoch", text},
         {"npc_id", integer}, {"merc_name", integer}},
        json::array({"request_id", "expected_epoch", "npc_id", "merc_name"}));
    add("use_roguelike_scroll", "Submit the stock right-click item-use packet for one owned reroll consumable (Talisman or Edict); observe arm before clicking an offered choice.",
        {{"request_id", text}, {"expected_epoch", text}, {"from_level", integer}, {"item_id", integer}},
        json::array({"request_id", "expected_epoch", "from_level", "item_id"}));
    add("use_roguelike_aura", "Use one owned Paladin Aura Elixir in an active run; observe its native aura level and item debit separately.",
        {{"request_id", text}, {"expected_epoch", text}, {"from_level", integer}, {"item_id", integer}},
        json::array({"request_id", "expected_epoch", "from_level", "item_id"}));
    add("interact_roguelike_chest", "Click one observed stock locked chest in the current run zone; verify key use and reward through native state.",
        {{"request_id", text}, {"expected_epoch", text}, {"from_level", integer}, {"object_id", integer}},
        json::array({"request_id", "expected_epoch", "from_level", "object_id"}));
    add("retire_roguelike_portals", "After arrival, retire the town entrance and Tower return (from_level 1), or the selected round's owned objects.",
        {{"request_id", text}, {"expected_epoch", text},
         {"from_level", {{"type", "integer"}, {"minimum", 1}, {"maximum", 254}}}, {"commit", {{"type", "boolean"}}}},
        json::array({"request_id", "expected_epoch", "from_level", "commit"}));
    add("interact_event_shrine", "Click one owned shrine object in the active monster-free run area.",
        {{"request_id", text}, {"expected_epoch", text}, {"object_id", integer}},
        json::array({"request_id", "expected_epoch", "object_id"}));
    add("spawn_enemies", "Create bounded native table monsters, champion/unique ranks or a superunique definition in the offline server.",
        {{"request_id", text}, {"enemies", {{"type", "array"}, {"minItems", 1}, {"maxItems", 16},
            {"items", {{"type", "object"}, {"additionalProperties", false},
                {"properties", {{"class_id", integer}, {"superunique_id", integer},
                    {"rank", {{"enum", {"normal", "champion", "unique"}}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}},
                    {"attributes", ServerMonsters::AttributeSchema()}}},
                {"oneOf", json::array({json{{"required", json::array({"class_id"})}},
                    json{{"required", json::array({"superunique_id"})}}})}}}}}},
        json::array({"request_id", "enemies"}));
    add("get_enemy_state", "Read authoritative server identity and attributes for bounded current-area monster IDs.",
        {{"request_id", text}, {"unit_ids", {{"type", "array"}, {"items", integer},
            {"minItems", 1}, {"maxItems", 16}, {"uniqueItems", true}}}}, json::array({"request_id", "unit_ids"}));
    add("get_area_state", "Census actual hostile monster identities and generated/loaded rooms in the current offline server area.",
        {{"request_id", text}}, json::array({"request_id"}));
    add("get_observation", "Capture a coherent nonce-correlated server frame; the MCP worker awaits the server result for up to three seconds after game-thread submission. On pending, inspect get_spawn_result with the same request_id; never replay.",
        {{"request_id", text}, {"expected_epoch", text},
         {"unit_ids", {{"type", "array"}, {"items", integer}, {"maxItems", 16}, {"uniqueItems", true}}}},
        json::array({"request_id"}));
    add("start_combat_trace", "Start one bounded native combat event trace in the offline server nonce callback.",
        {{"request_id", text}, {"expected_epoch", text}, {"duration", {{"type", "integer"}, {"minimum", 1}, {"maximum", 60}}},
         {"unit_ids", {{"type", "array"}, {"items", integer}, {"maxItems", 15}, {"uniqueItems", true}}}},
        json::array({"request_id", "expected_epoch", "unit_ids"}));
    add("read_combat_trace", "Read bounded actual native combat events, with overflow and coverage.",
        {{"trace_id", text}, {"expected_epoch", text}, {"after_sequence", integer},
         {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}}, json::array());
    add("stop_combat_trace", "Stop the active native combat trace and restore its original hooks.",
        {{"trace_id", text}, {"expected_epoch", text}}, json::array());
    add("get_monster_catalog", "Page live native monster classes, superunique definitions or unique modifier metadata.",
        {{"table", {{"enum", {"monsters", "superuniques", "modifiers"}}}},
         {"offset", integer}, {"limit", integer}}, json::array());
    return tools;
}

json HandleToolCall(const std::string& name, const json& args) {
    if (!args.is_object()) return Error("invalid_arguments");
    if (name == "ping") return Result({{"status", "ok"}, {"source", "native_toolkit"}});
    // Trace readers touch only copied POD ring data; allow post-exit evidence
    // retrieval even after the in-game queue stops accepting gameplay calls.
    if (name == "read_combat_trace" || name == "stop_combat_trace") {
        json value = name == "read_combat_trace" ? CombatTelemetry::Read(args) : CombatTelemetry::Stop(args);
        return Result(value, !value.value("ok", false));
    }
    if (name == "get_observation") {
        // Only nonce submission touches client bindings on the game queue. This
        // worker waits after that closure returns, never inside GameLoop/ChatHook.
        json submission = OnGameThread([args]() {
            if (!g_offlineMenuChosen || !GameState::IsGameReady()) return Error("offline_game_required");
            return ServerSpawns::Observe(args);
        });
        if (submission.contains("content")) return submission; // queue/guard error
        if (!submission.value("ok", false)) return Result(submission, true);
        if (submission.value("status", "") == "requested" ||
            submission.value("status", "") == "pending")
            submission = ServerSpawns::AwaitObservation(args, 3000);
        return Result(submission, !submission.value("ok", false));
    }
    if (GameplayCall call = GameplayHandler(name)) {
        return OnGameThread([call, args, name]() {
            if (!g_offlineMenuChosen || !GameState::IsGameReady()) return Error("offline_game_required");
            if (name == "spawn_item" || name == "pickup_item" || name == "move_item" ||
                name == "stash_item" || name == "move_cube_item") {
                auto epoch = args.find("expected_epoch");
                if (epoch != args.end()) {
                    if (!epoch->is_string() || epoch->get_ref<const std::string&>().empty())
                        return Error("invalid_expected_epoch");
                    if (!ObservationTelemetry::MatchesEpoch(epoch->get_ref<const std::string&>()))
                        return Error("stale_epoch");
                }
            }
            json value = call(args);
            return Result(value, !value.value("ok", false));
        });
    }
    if (name == "get_player_state") {
        return OnGameThread([]() {
        if (!GameState::IsGameReady()) return Error("not_in_game");
        auto p = GameState::GetPlayerState();
        if (!p.valid) return Error("snapshot_unavailable");
        const char* classes[] = {"Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin"};
        return Result({{"source", "native_toolkit"}, {"name", p.name},
            {"class", p.classId <= 6 && p.classId >= 0 ? classes[p.classId] : "Unknown"},
            {"class_id", p.classId}, {"level", p.level},
            {"mode", p.mode},
            {"strength", p.strength}, {"energy", p.energy}, {"dexterity", p.dexterity}, {"vitality", p.vitality},
            {"gold", p.gold}, {"gold_stash", p.goldStash},
            {"hp", p.hp >> 8}, {"max_hp", p.maxHp >> 8},
            {"mana", p.mana >> 8}, {"max_mana", p.maxMana >> 8},
            {"position", {{"x", p.x}, {"y", p.y}}}, {"area", p.area}, {"area_name", p.areaName}});
        });
    }
    if (name == "get_game_state") {
        return OnGameThread([]() {
            Control* controls[kMaxControls] = {};
            int count = 0;
            if (!ReadControls(controls, count)) return Error("control_traversal_failed");
            json state = {{"source", "native_toolkit"}, {"state", ScreenState(count != 0)}};
            if (GameState::IsGameReady()) {
                auto player = GameState::GetPlayerState();
                if (player.valid) {
                    state["area"] = player.area;
                    state["area_name"] = player.areaName;
                }
            }
            return Result(state);
        });
    }
    if (name == "get_controls") {
        return OnGameThread([]() {
            Control* controls[kMaxControls] = {};
            int count = 0;
            if (!ReadControls(controls, count)) return Error("control_traversal_failed");
            json list = json::array();
            for (int i = 0; i < count; ++i) {
                Control c = {};
                json lines;
                if (!ReadControl(controls[i], c) || !ReadLines(controls[i], lines)) return Error("control_read_failed");
                list.push_back({{"index", i}, {"type", static_cast<int>(c.dwType)},
                    {"state", static_cast<int>(c.dwState)}, {"enabled", Enabled(c)},
                    {"x", c.dwPosX}, {"y", c.dwPosY}, {"w", c.dwSizeX}, {"h", c.dwSizeY},
                    {"has_on_press", c.OnPress != nullptr}, {"text", lines},
                    {"label", lines.empty() ? "" : lines[0].get<std::string>()}});
            }
            return Result({{"source", "native_toolkit"}, {"state", ScreenState(count != 0)},
                {"process_id", g_processId}, {"process_session", g_processSession},
                {"process_start_filetime", g_processStartFiletime},
                {"count", count}, {"controls", list}});
        });
    }
    if (name == "skip_copyright") {
        if (!args.empty()) return Error("invalid_arguments");
        return OnGameThread([]() {
            if (GameState::IsGameReady() || g_offlineMenuChosen.load()) return Error("not_on_copyright_screen");
            Control* controls[kMaxControls] = {};
            int count = 0;
            if (!ReadControls(controls, count) || count != 5) return Error("not_on_copyright_screen");
            const DWORD types[] = {2, 8, 2, 2, 4};
            Control last = {};
            for (int i = 0; i < count; ++i) {
                if (!ReadControl(controls[i], last) || last.dwType != types[i]) return Error("not_on_copyright_screen");
            }
            json lines;
            if (!ReadLines(controls[4], lines) || lines != json::array({"\xC2\xA9 Copyright 2001 Blizzard Entertainment", "All Rights Reserved."}) ||
                last.dwPosX != 100 || last.dwPosY != 580 || last.dwSizeX != 600 || last.dwSizeY != 80 || !Enabled(last))
                return Error("not_on_copyright_screen");
            HWND window = D2GFX_GetHwnd();
            DWORD owner = 0;
            if (!window || !IsWindow(window) || !GetWindowThreadProcessId(window, &owner) || owner != g_processId)
                return Error("game_window_unavailable");
            if (g_copyrightKeyAttempted.exchange(true)) return Error("startup_key_already_attempted");
            // Target this verified guest window, not the global keyboard or
            // whichever application owns foreground focus. Never replay a
            // partial pair; menu transition is a separate observation.
            if (!PostMessage(window, WM_KEYDOWN, VK_ESCAPE, static_cast<LPARAM>(0x00010001u)) ||
                !PostMessage(window, WM_KEYUP, VK_ESCAPE, static_cast<LPARAM>(0xC0010001u)))
                return Error("startup_key_unconfirmed");
            return Result({{"status", "input_queued"}, {"process_id", g_processId},
                {"process_start_filetime", g_processStartFiletime}, {"key", "escape"},
                {"delivery", "owned_window_messages"},
                {"verification", "main_menu_must_be_observed_separately"}});
        });
    }
    if (name == "click_control") {
        if (!args.contains("index") || !args["index"].is_number_integer() ||
            !args.contains("expected_type") || !args["expected_type"].is_number_integer() ||
            !args.contains("expected_label") || !args["expected_label"].is_string()) return Error("invalid_arguments");
        int index = args["index"].get<int>();
        int type = args["expected_type"].get<int>();
        std::string label = args["expected_label"].get<std::string>();
        if (index < 0 || index >= kMaxControls || type != 6 || !OfflineButton(label)) return Error("control_not_allowed");
        return OnGameThread([index, type, label]() {
            if (GameState::IsGameReady()) return Error("not_in_menu");
            const bool singlePlayer = label == "Single Player" || label == "SINGLE PLAYER";
            if (!singlePlayer && !g_offlineMenuChosen) return Error("offline_menu_not_selected");
            Control* controls[kMaxControls] = {};
            int count = 0;
            if (!ReadControls(controls, count)) return Error("control_traversal_failed");
            if (index >= count) return Error("stale_control");
            Control c = {};
            json lines;
            if (!ReadControl(controls[index], c) || !ReadLines(controls[index], lines)) return Error("control_read_failed");
            if (static_cast<int>(c.dwType) != type || !Enabled(c) || lines.empty() || lines[0] != label)
                return Error("stale_control");
            if (!Press(controls[index], c)) return Error("press_failed");
            if (singlePlayer) g_offlineMenuChosen = true;
            if (label == "Back" || label == "BACK" || label == "Cancel" || label == "CANCEL" ||
                label == "Exit Diablo II" || label == "EXIT DIABLO II" || label == "Quit" || label == "QUIT")
                g_offlineMenuChosen = false;
            return Result({{"status", "clicked"}, {"index", index}, {"type", type}, {"label", label}});
        });
    }
    if (name == "select_character") {
        if (!args.contains("name") || !args["name"].is_string()) return Error("invalid_arguments");
        std::string requested = args["name"].get<std::string>();
        if (requested.empty() || requested.size() > 64) return Error("invalid_arguments");
        if (!g_offlineMenuChosen) return Error("offline_menu_not_selected");
        return OnGameThread([requested]() {
            if (GameState::IsGameReady()) return Error("not_in_menu");
            Control* controls[kMaxControls] = {};
            int count = 0;
            if (!ReadControls(controls, count)) return Error("control_traversal_failed");
            Control* match = nullptr;
            Control selected = {};
            for (int i = 0; i < count; ++i) {
                Control c = {};
                if (!ReadControl(controls[i], c)) return Error("control_read_failed");
                if (c.dwType != 4 || !Enabled(c) || !c.OnPress || c.dwPosY < 150 || c.dwSizeX < 150) continue;
                json lines;
                if (!ReadLines(controls[i], lines)) return Error("control_read_failed");
                if (!lines.empty() && lines[0] == requested) {
                    if (match) return Error("ambiguous_character");
                    match = controls[i];
                    selected = c;
                }
            }
            if (!match) return Error("character_not_found");
            if (!SelectCharacterRow(match, selected)) return Error("selection_failed");
            for (int i = 0; i < count; ++i) {
                Control c = {};
                if (!ReadControl(controls[i], c)) return Error("control_read_failed");
                if (c.dwType != 4 || c.OnPress || c.dwPosY >= 150 || c.dwSizeX < 400) continue;
                json lines;
                if (!ReadLines(controls[i], lines)) return Error("control_read_failed");
                if (!lines.empty() && lines[0] == requested)
                    return Result({{"status", "selected"}, {"character", lines[0]}});
            }
            return Error("selection_not_observed");
        });
    }
    if (name == "exit_game") {
        return OnGameThread([]() {
            if (!GameState::IsGameReady()) return Error("not_in_game");
            ServerMonsters::Clear();
            GameState::EndSession();
            json routeSnapshot = ServerSpawns::RunSnapshotStatus();
            D2CLIENT_ExitGame();
            return Result({{"status", "exit_requested"},
                {"route_snapshot", std::move(routeSnapshot)}});
        });
    }
    if (name == "quit_game") {
        return OnGameThread([]() {
            HWND window = D2GFX_GetHwnd();
            DWORD owner = 0;
            if (!window || !IsWindow(window) || !GetWindowThreadProcessId(window, &owner) || owner != GetCurrentProcessId())
                return Error("game_window_unavailable");
            if (!PostMessage(window, WM_CLOSE, 0, 0)) return Error("close_failed");
            return Result({{"status", "close_requested"}});
        });
    }
    return Error("unknown_tool");
}

json ProcessRequest(const json& request) {
    if (!request.is_object()) return {{"jsonrpc", "2.0"}, {"id", nullptr},
        {"error", {{"code", -32600}, {"message", "Invalid request"}}}};
    json id = request.value("id", json(nullptr));
    if (!request.contains("method") || !request["method"].is_string()) return {{"jsonrpc", "2.0"}, {"id", id},
        {"error", {{"code", -32600}, {"message", "Invalid method"}}}};
    std::string method = request["method"].get<std::string>();
    json result;
    if (method == "initialize") result = {{"protocolVersion", kVersion},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", "d2-mod-toolkit-lab"}, {"version", "0.1.0"}}}};
    else if (method == "notifications/initialized") return nullptr;
    else if (method == "tools/list") result = {{"tools", ToolsList()}};
    else if (method == "tools/call") {
        json params = request.value("params", json::object());
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) result = Error("invalid_arguments");
        else result = HandleToolCall(params["name"].get<std::string>(), params.value("arguments", json::object()));
    } else return {{"jsonrpc", "2.0"}, {"id", id},
        {"error", {{"code", -32601}, {"message", "Method not found"}}}};
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

void ServerThread() {
    while (g_shouldRun) {
        SocketLease sockets;
        if (!sockets.valid) {
            Sleep(1000);
            continue;
        }
        httplib::Server server;
        g_server.store(&server);
        server.set_keep_alive_max_count(1);
        server.set_read_timeout(10, 0);
        server.set_write_timeout(10, 0);
        server.set_default_headers({{"Connection", "close"}, {"Cache-Control", "no-store"}});
        server.Options("/mcp", [](const httplib::Request& req, httplib::Response& res) {
            if (Authorized(req, res)) res.status = 204;
        });
        server.Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
            if (!Authorized(req, res)) return;
            ++g_requestCount;
            int sessionId = 0;
            if (req.has_param("sessionId")) {
                std::string raw = req.get_param_value("sessionId");
                if (raw.empty() || raw.size() > 10 || !std::all_of(raw.begin(), raw.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                    res.status = 400; return;
                }
                sessionId = std::atoi(raw.c_str());
                std::lock_guard<std::mutex> lock(g_sessionsMutex);
                if (sessionId <= 0 || g_sessions.find(sessionId) == g_sessions.end()) { res.status = 404; return; }
            }
            json response;
            try { response = ProcessRequest(json::parse(req.body)); }
            catch (const json::parse_error&) { response = {{"jsonrpc", "2.0"}, {"id", nullptr},
                {"error", {{"code", -32700}, {"message", "Parse error"}}}}; }
            catch (...) { response = {{"jsonrpc", "2.0"}, {"id", nullptr},
                {"error", {{"code", -32603}, {"message", "Internal error"}}}}; }
            if (response.is_null()) { res.status = 202; return; }
            if (sessionId) { SendSseMessage(sessionId, response.dump()); res.status = 202; }
            else res.set_content(response.dump(), "application/json; charset=utf-8");
        });
        server.Get("/mcp/sse", [](const httplib::Request& req, httplib::Response& res) {
            if (!Authorized(req, res)) return;
            int id = g_nextSessionId++;
            auto session = std::make_shared<Session>();
            {
                std::lock_guard<std::mutex> lock(g_sessionsMutex);
                g_sessions[id] = session;
            }
            res.set_header("X-Accel-Buffering", "no");
            auto sent = std::make_shared<bool>(false);
            res.set_header("Connection", "keep-alive");
            res.set_chunked_content_provider("text/event-stream",
                [session, id, sent](size_t, httplib::DataSink& sink) {
                    if (!*sent) {
                        std::string endpoint = "event: endpoint\ndata: /mcp?sessionId=" + std::to_string(id) + "\n\n";
                        if (!sink.write(endpoint.c_str(), endpoint.size())) return false;
                        *sent = true;
                    }
                    std::unique_lock<std::mutex> lock(session->mutex);
                    session->cv.wait_for(lock, std::chrono::seconds(15), [&session]() {
                        return !session->messages.empty() || session->closed;
                    });
                    if (session->closed) return false;
                    while (!session->messages.empty()) {
                        std::string message = std::move(session->messages.front());
                        session->messages.pop();
                        if (!sink.write(message.c_str(), message.size())) return false;
                    }
                    static const char keepalive[] = ": keepalive\n\n";
                    return sink.write(keepalive, sizeof(keepalive) - 1);
                },
                [id](bool) {
                    std::lock_guard<std::mutex> lock(g_sessionsMutex);
                    g_sessions.erase(id);
                });
        });
        server.Get("/health", [](const httplib::Request& req, httplib::Response& res) {
            if (!Authorized(req, res)) return;
            res.set_content(json({{"status", "ok"}, {"source", "native_toolkit"}}).dump(), "application/json; charset=utf-8");
        });
        g_running = true;
        server.listen("127.0.0.1", g_port);
        g_running = false;
        g_server.store(nullptr);
        if (g_shouldRun) Sleep(1000);
    }
}
} // namespace

namespace McpServer {
void Init(int port) {
    if (g_shouldRun) return;
    // The verified profile, not an HTTP request or caller default, owns the bind port.
    (void)port;
    if (LabProfile::Token().size() != 64) return;
    // Bind out-of-game controls to this verified backend's process, without
    // requiring a guest UI worker to open another process or elevate its token.
    const DWORD processId = GetCurrentProcessId();
    DWORD processSession = 0;
    FILETIME created = {}, exited = {}, kernel = {}, user = {};
    if (!ProcessIdToSessionId(processId, &processSession) ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return;
    ULARGE_INTEGER start = {};
    start.LowPart = created.dwLowDateTime;
    start.HighPart = created.dwHighDateTime;
    if (!start.QuadPart) return;
    g_processId = processId;
    g_processSession = processSession;
    g_processStartFiletime = std::to_string(start.QuadPart);
    g_port = LabProfile::Port();
    g_requestCount = 0;
    g_offlineMenuChosen = false;
    g_shouldRun = true;
    g_thread = std::thread(ServerThread);
}
void Shutdown() {
    g_shouldRun = false;
    if (auto* server = g_server.load()) server->stop();
    {
        std::lock_guard<std::mutex> lock(g_sessionsMutex);
        for (auto& entry : g_sessions) {
            std::lock_guard<std::mutex> slock(entry.second->mutex);
            entry.second->closed = true;
            entry.second->cv.notify_one();
        }
    }
    if (g_thread.joinable()) g_thread.join();
}
void AbandonOnProcessExit() {
    if (g_thread.joinable()) g_thread.detach();
}
bool IsRunning() { return g_running; }
int GetPort() { return g_port; }
int GetRequestCount() { return g_requestCount; }
}
