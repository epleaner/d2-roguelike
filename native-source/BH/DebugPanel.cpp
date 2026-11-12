#include "DebugPanel.h"
#include "McpServer.h"
#include "GameState.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"
#include "Constants.h"
#include "StreamStats.h"
#include "AutoPotion.h"
#include "AutoPickup.h"
#include "AutoCast.h"
#include "HookManager.h"
#include "CrashCatcher.h"
#include "PatchManager.h"
#include "GamePause.h"
#include "GameCallQueue.h"
#include "MemWatch.h"
#include "BH.h"

#include <windows.h>
#include <shellscalingapi.h>
#include <map>
#include <vector>
#include <string>
#include <d3d9.h>
#include <thread>
#include <atomic>

#include "../ThirdParty/imgui/imgui.h"
#include "../ThirdParty/imgui/imgui_impl_dx9.h"
#include "../ThirdParty/imgui/imgui_impl_win32.h"

#pragma comment(lib, "Shcore.lib")

// Forward declare the Win32 message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
    std::thread g_thread;
    std::atomic<bool> g_running{false};
    std::atomic<bool> g_shutdownRequested{false};

    static const char* WINDOW_CLASS = "D2ModToolkitDebugPanel";
    static const char* WINDOW_TITLE = "d2-mod-toolkit Debug Panel";

    // Stash tab scan cache
    struct StashItem {
        int unitId;
        int code;
        int gridX, gridY;
        int tab;
        char name[64];
        char category[32];
    };
    static std::vector<StashItem> g_stashScanItems;
    static int g_stashScanTab = -1; // -1 = no scan, 0-9 = single tab, 10 = all
    static bool g_stashScanning = false;
    static int g_stashScanProgress = 0;

    const char* CategorizeItemName(const char* name) {
        // Simplified categorization for display
        if (strstr(name, "Grand Charm")) return "Grand Charm";
        if (strstr(name, "Large Charm")) return "Large Charm";
        if (strstr(name, "Small Charm")) return "Small Charm";
        if (strstr(name, "Charm")) return "Charm";
        if (strstr(name, "Ring")) return "Ring";
        if (strstr(name, "Amulet")) return "Amulet";
        if (strstr(name, "Jewel")) return "Jewel";
        if (strstr(name, "Token")) return "Token";
        if (strstr(name, "Potion")) return "Potion";
        if (strstr(name, "Key")) return "Key";
        if (strstr(name, "Tome")) return "Tome";
        if (strstr(name, "Scroll")) return "Scroll";
        if (strstr(name, "Ear")) return "Ear";
        if (strstr(name, "Boot") || strstr(name, "Greave")) return "Boots";
        if (strstr(name, "Glove") || strstr(name, "Gaunt")) return "Gloves";
        if (strstr(name, "Belt") || strstr(name, "Sash")) return "Belt";
        if (strstr(name, "Armor") || strstr(name, "Plate") || strstr(name, "Mail") || strstr(name, "Sacred")) return "Body Armor";
        if (strstr(name, "Helm") || strstr(name, "Crown") || strstr(name, "Mask") || strstr(name, "Visage") || strstr(name, "Diadem")) return "Helmet";
        if (strstr(name, "Shield") || strstr(name, "Pavise") || strstr(name, "Monarch")) return "Shield";
        if (strstr(name, "Sword") || strstr(name, "Blade") || strstr(name, "Crystal")) return "Sword";
        if (strstr(name, "Axe") || strstr(name, "Hatchet")) return "Axe";
        if (strstr(name, "Spear") || strstr(name, "Javelin") || strstr(name, "Pilum") || strstr(name, "Mancatcher")) return "Spear/Jav";
        if (strstr(name, "Staff") || strstr(name, "Wand") || strstr(name, "Orb") || strstr(name, "Scepter")) return "Caster Wpn";
        if (strstr(name, "Bow") || strstr(name, "Crossbow")) return "Ranged";
        if (strstr(name, "Claw") || strstr(name, "Katar")) return "Claw";
        if (strstr(name, "Mace") || strstr(name, "Club") || strstr(name, "Hammer") || strstr(name, "Flail")) return "Mace";
        return "Other";
    }

    // Base sizes at 96 DPI (100% scaling)
    static const float BASE_FONT_SIZE = 24.0f;
    static const int   BASE_WINDOW_W = 900;
    static const int   BASE_WINDOW_H = 700;

    // Manual DPI override presets
    static const float DPI_PRESETS[] = { 0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f };
    static const int   DPI_PRESET_COUNT = sizeof(DPI_PRESETS) / sizeof(DPI_PRESETS[0]);
    static int         g_dpiPresetIndex = 0;  // 0 = auto
    static float       g_dpiAutoScale = 1.0f; // the detected system DPI

    HWND                g_hwnd = nullptr;
    LPDIRECT3D9         g_pD3D = nullptr;
    LPDIRECT3DDEVICE9   g_pd3dDevice = nullptr;
    D3DPRESENT_PARAMETERS g_d3dpp = {};
    float               g_dpiScale = 1.0f;
    bool                g_fontRebuildNeeded = false;

    // Get DPI scale factor for a monitor
    float GetDpiScaleForMonitor(HMONITOR hMonitor) {
        UINT dpiX = 96, dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
            return (float)dpiX / 96.0f;
        }
        // Fallback: try GetDeviceCaps
        HDC hdc = GetDC(nullptr);
        float scale = (float)GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, hdc);
        return scale;
    }

    // Get DPI scale for a specific window
    float GetDpiScaleForWindow(HWND hwnd) {
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        return GetDpiScaleForMonitor(hMon);
    }

    // Rebuild the font atlas at the current DPI scale
    void RebuildFonts() {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        float fontSize = BASE_FONT_SIZE * g_dpiScale;

        // Load a scalable TTF system font instead of the fixed-size bitmap default
        const char* fontPaths[] = {
            "C:\\Windows\\Fonts\\segoeui.ttf",   // Segoe UI (Windows 10/11)
            "C:\\Windows\\Fonts\\arial.ttf",      // Arial fallback
            "C:\\Windows\\Fonts\\tahoma.ttf",     // Tahoma fallback
            nullptr
        };

        // Include arrows and symbols glyph range for icons
        static const ImWchar glyphRanges[] = {
            0x0020, 0x00FF, // Basic Latin + Latin Supplement
            0x2190, 0x21FF, // Arrows (includes refresh ↻ U+21BB)
            0x2700, 0x27BF, // Dingbats
            0,
        };

        bool fontLoaded = false;
        for (int i = 0; fontPaths[i]; ++i) {
            if (GetFileAttributesA(fontPaths[i]) != INVALID_FILE_ATTRIBUTES) {
                io.Fonts->AddFontFromFileTTF(fontPaths[i], fontSize, nullptr, glyphRanges);
                fontLoaded = true;
                break;
            }
        }

        if (!fontLoaded) {
            // Last resort: scaled bitmap font (won't look great but won't crash)
            ImFontConfig cfg;
            cfg.SizePixels = fontSize;
            io.Fonts->AddFontDefault(&cfg);
        }

        // Rebuild font texture
        ImGui_ImplDX9_InvalidateDeviceObjects();
        io.Fonts->Build();
        ImGui_ImplDX9_CreateDeviceObjects();

        // Scale style too
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle(); // reset to defaults
        ImGui::StyleColorsDark();
        style.WindowRounding = 0.0f;
        style.FrameRounding = 2.0f;
        style.TabRounding = 2.0f;
        style.ScaleAllSizes(g_dpiScale);
    }

    bool CreateDeviceD3D(HWND hWnd) {
        g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
        if (!g_pD3D) return false;

        ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
        g_d3dpp.Windowed = TRUE;
        g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
        g_d3dpp.EnableAutoDepthStencil = TRUE;
        g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
        g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

        HRESULT hr = g_pD3D->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            &g_d3dpp, &g_pd3dDevice);

        if (FAILED(hr)) {
            hr = g_pD3D->CreateDevice(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                &g_d3dpp, &g_pd3dDevice);
        }

        return SUCCEEDED(hr);
    }

    void CleanupDeviceD3D() {
        if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
        if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
    }

    void ResetDevice() {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
        if (hr == D3DERR_INVALIDCALL) return;
        ImGui_ImplDX9_CreateDeviceObjects();
    }

    LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;

        switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            if (g_pd3dDevice) {
                g_d3dpp.BackBufferWidth = LOWORD(lParam);
                g_d3dpp.BackBufferHeight = HIWORD(lParam);
                ResetDevice();
            }
            return 0;
        case WM_MOVE: {
            RECT r;
            GetWindowRect(hWnd, &r);
            App.debugPanel.posX.value = r.left;
            App.debugPanel.posY.value = r.top;
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            // Save dimensions after user finishes dragging/resizing
            RECT r;
            GetWindowRect(hWnd, &r);
            App.debugPanel.posX.value = r.left;
            App.debugPanel.posY.value = r.top;
            App.debugPanel.width.value = r.right - r.left;
            App.debugPanel.height.value = r.bottom - r.top;
            return 0;
        }
        case WM_DPICHANGED: {
            // Per-monitor DPI change — window was dragged to a different monitor
            float newDpi = (float)HIWORD(wParam);
            float newScale = newDpi / 96.0f;
            g_dpiAutoScale = newScale;
            // Only apply if in auto mode
            if (g_dpiPresetIndex == 0 && newScale != g_dpiScale) {
                g_dpiScale = newScale;
                g_fontRebuildNeeded = true;
            }
            // Windows provides the suggested new rect
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hWnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    // Draw all debug panel tabs
    void DrawDebugUI() {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Debug Panel", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            ImGui::Text("d2-mod-toolkit v0.1");
            ImGui::Separator();

            // Clickable DPI selector — cycles through presets
            char dpiLabel[32];
            if (g_dpiPresetIndex == 0)
                snprintf(dpiLabel, sizeof(dpiLabel), "DPI: Auto (%.0f%%)", g_dpiScale * 100.0f);
            else
                snprintf(dpiLabel, sizeof(dpiLabel), "DPI: %.0f%%", g_dpiScale * 100.0f);

            if (ImGui::SmallButton(dpiLabel)) {
                g_dpiPresetIndex = (g_dpiPresetIndex + 1) % DPI_PRESET_COUNT;
                if (g_dpiPresetIndex == 0) {
                    g_dpiScale = g_dpiAutoScale;
                } else {
                    g_dpiScale = DPI_PRESETS[g_dpiPresetIndex];
                }
                g_fontRebuildNeeded = true;
                App.debugPanel.dpiPreset.value = g_dpiPresetIndex;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Click to cycle: Auto, 100%%, 125%%, 150%%, 175%%, 200%%, 250%%, 300%%");

            ImGui::Separator();
            if (McpServer::IsRunning()) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                    "MCP: :%d (%d reqs)", McpServer::GetPort(), McpServer::GetRequestCount());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "MCP: offline");
            }

            ImGui::Separator();
            if (GamePause::IsPaused()) {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.2f, 1.0f), "PAUSED (F%d)", GamePause::GetFrameCount());
                ImGui::SameLine();
                if (ImGui::SmallButton("Resume")) GamePause::Resume();
                ImGui::SameLine();
                if (ImGui::SmallButton("Step")) GamePause::Step();
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "F%d", GamePause::GetFrameCount());
                ImGui::SameLine();
                if (ImGui::SmallButton("Pause")) GamePause::Pause();
            }
            ImGui::EndMenuBar();
        }

        if (ImGui::BeginTabBar("DebugTabs")) {
            // Common colors for all tabs
            ImVec4 cGold(0.85f, 0.72f, 0.45f, 1.0f);
            ImVec4 cWhite(1.0f, 1.0f, 1.0f, 1.0f);
            ImVec4 cGray6(0.6f, 0.6f, 0.6f, 1.0f);
            ImVec4 cRed(1.0f, 0.3f, 0.3f, 1.0f);
            ImVec4 cGreen(0.3f, 1.0f, 0.3f, 1.0f);
            ImVec4 cBlue(0.4f, 0.6f, 1.0f, 1.0f);
            ImVec4 cYellow(1.0f, 1.0f, 0.3f, 1.0f);

            // === STREAM TAB (default, visible to audience) ===
            if (ImGui::BeginTabItem("Stream")) {
                auto ss = StreamStats::GetStats();
                DWORD elapsed = (GetTickCount() - ss.sessionStartTime) / 1000;
                int hours = elapsed / 3600, mins = (elapsed % 3600) / 60, secs = elapsed % 60;

                // Line 1: AI Status (big and colorful)
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "AI:"); ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", ss.status);
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("00:00:00").x);
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%02d:%02d:%02d", hours, mins, secs);

                // Line 2: Core stats — Deaths | Kills | Items | Uniques
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Deaths:%d", ss.deaths); ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), " Kills:%d", ss.monstersKilled); ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), " Loot:%d", ss.itemsPickedUp); ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), " Uniques:%d", ss.uniquesFound); ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), " Chickens:%d", ss.chickens);

                // Line 3: Run info
                ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "Runs:%d", ss.runsCompleted); ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), " Games:%d", ss.gamesEntered); ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), " Vendored:%d", ss.itemsVendored);
                if (ss.lastRunSeconds > 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), " Last:%.0fs", ss.lastRunSeconds);
                }

                // Line 4: Last event
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), ">"); ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", ss.lastEvent);

                // Line 5: Fun/audience message
                ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", ss.funMessage);

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Player")) {
                if (!GameState::IsGameReady()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Not in game");
                } else {
                    auto ps = GameState::GetPlayerState();
                    const char* classNames[] = {"Amazon", "Sorceress", "Necromancer", "Paladin", "Barbarian", "Druid", "Assassin"};
                    const char* cls = (ps.classId >= 0 && ps.classId <= 6) ? classNames[ps.classId] : "Unknown";

                    // Colors matching BH StatsDisplay
                    ImVec4 cRed(1.0f, 0.3f, 0.3f, 1.0f);
                    ImVec4 cBlue(0.4f, 0.6f, 1.0f, 1.0f);
                    ImVec4 cYellow(1.0f, 1.0f, 0.3f, 1.0f);
                    ImVec4 cGreen(0.3f, 1.0f, 0.3f, 1.0f);
                    ImVec4 cGold(0.85f, 0.72f, 0.45f, 1.0f);
                    ImVec4 cWhite(1.0f, 1.0f, 1.0f, 1.0f);
                    ImVec4 cGray(0.6f, 0.6f, 0.6f, 1.0f);
                    ImVec4 cPurple(0.7f, 0.4f, 1.0f, 1.0f);

                    // Resistance penalty from game state (dynamic based on difficulty)
                    int penalty = ps.resPenalty;

                    // -- Name / Level / Class / Difficulty --
                    float rightEdge = ImGui::GetWindowContentRegionMax().x;
                    const char* diffNames[] = {"Normal", "Nightmare", "Hell"};
                    const char* diff = (ps.difficulty >= 0 && ps.difficulty <= 2) ? diffNames[ps.difficulty] : "?";

                    ImGui::TextColored(cGold, "Name:"); ImGui::SameLine();
                    ImGui::Text("%s", ps.name);
                    char levelBuf[64]; snprintf(levelBuf, sizeof(levelBuf), "Level: %d %s (%s)", ps.level, cls, diff);
                    ImGui::SameLine(rightEdge - ImGui::CalcTextSize(levelBuf).x);
                    ImGui::TextColored(cGold, "%s", levelBuf);

                    // XP and session stats (same line)
                    {
                        auto ss = GameState::GetSessionStats();
                        ImGui::TextColored(cGold, "Deaths:"); ImGui::SameLine();
                        ImGui::TextColored(ss.deaths > 0 ? cRed : cGreen, "%d", ss.deaths);
                        ImGui::SameLine();
                        ImGui::TextColored(cGold, "  Games:"); ImGui::SameLine();
                        ImGui::Text("%d", ss.gamesEntered);

                        char xpBuf[64]; snprintf(xpBuf, sizeof(xpBuf), "XP: %.2f%% / Additional XP: %d%%", ps.xpPctToNext, ps.addXp);
                        ImGui::SameLine(rightEdge - ImGui::CalcTextSize(xpBuf).x);
                        ImGui::TextColored(cGold, "%s", xpBuf);
                    }

                    // Area and player count (line 3)
                    ImGui::TextColored(cGold, "Area:"); ImGui::SameLine();
                    ImGui::Text("%s (%d)", ps.areaName[0] ? ps.areaName : "Unknown", ps.area);
                    char playerBuf[32]; snprintf(playerBuf, sizeof(playerBuf), "Players: %d", ps.playerCount);
                    ImGui::SameLine(rightEdge - ImGui::CalcTextSize(playerBuf).x);
                    ImGui::TextColored(cGold, "%s", playerBuf);
                    ImGui::Separator();

                    // -- Resistances --
                    ImGui::TextColored(cRed, "Fire Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d", ps.fireRes + penalty); ImGui::SameLine();
                    ImGui::Text("/ %d", ps.maxFireRes);

                    ImGui::TextColored(cBlue, "Cold Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d", ps.coldRes + penalty); ImGui::SameLine();
                    ImGui::Text("/ %d", ps.maxColdRes); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "  Length: %d%%", ps.cannotBeFrozen > 0 ? 0 : (100 - 50 * (ps.halfFreeze > 2 ? 2 : ps.halfFreeze)));

                    ImGui::TextColored(cYellow, "Lightning Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cYellow, "%d", ps.lightRes + penalty); ImGui::SameLine();
                    ImGui::Text("/ %d", ps.maxLightRes);

                    ImGui::TextColored(cGreen, "Poison Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cGreen, "%d", ps.poisonRes + penalty); ImGui::SameLine();
                    ImGui::Text("/ %d", ps.maxPoisonRes); ImGui::SameLine();
                    ImGui::Text("  Length: %d%%", 100 - penalty - ps.poisonLenReduce);

                    int curseEff = ps.curseRes < 75 ? ps.curseRes : 75;
                    int curseLen = 100 - ps.curseLenReduce;
                    if (curseLen < 25) curseLen = 25;
                    ImGui::TextColored(cGold, "Curse Resist:"); ImGui::SameLine();
                    ImGui::TextColored(cGray, "%d / 75", curseEff); ImGui::SameLine();
                    ImGui::Text("Length: %d%%", curseLen);

                    ImGui::Separator();

                    // -- Absorption --
                    ImGui::TextColored(cGold, "Absorption:");
                    ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d/%d%%", ps.fireAbsorb, ps.fireAbsorbPct); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d/%d%%", ps.coldAbsorb, ps.coldAbsorbPct); ImGui::SameLine();
                    ImGui::TextColored(cYellow, "%d/%d%%", ps.lightAbsorb, ps.lightAbsorbPct); ImGui::SameLine();
                    ImGui::TextColored(cPurple, "%d/%d%%", ps.magicAbsorb, ps.magicAbsorbPct);

                    // -- Damage Reduction --
                    ImGui::TextColored(cGold, "Damage Reduction:"); ImGui::SameLine();
                    ImGui::Text("%d/%d%%", ps.dmgReduction, ps.dmgReductionPct); ImGui::SameLine();
                    ImGui::TextColored(cPurple, "%d/%d%%", ps.magDmgReduction, ps.magDmgReductionPct);

                    ImGui::TextColored(cGold, "Attacker Takes Damage:"); ImGui::SameLine();
                    ImGui::Text("%d", ps.attackerTakesDmg); ImGui::SameLine();
                    ImGui::TextColored(cYellow, " %d", ps.attackerTakesLtng);

                    ImGui::Separator();

                    // -- Elemental Mastery & Pierce --
                    ImGui::TextColored(cGold, "Elemental Mastery:");
                    ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d%%", ps.fireMastery); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d%%", ps.coldMastery); ImGui::SameLine();
                    ImGui::TextColored(cYellow, "%d%%", ps.lightMastery); ImGui::SameLine();
                    ImGui::TextColored(cGreen, "%d%%", ps.poisonMastery); ImGui::SameLine();
                    ImGui::TextColored(cPurple, "%d%%", ps.magicMastery);

                    ImGui::TextColored(cGold, "Elemental Pierce:");
                    ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d%%", ps.firePierce); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d%%", ps.coldPierce); ImGui::SameLine();
                    ImGui::TextColored(cYellow, "%d%%", ps.lightPierce); ImGui::SameLine();
                    ImGui::TextColored(cGreen, "%d%%", ps.poisonPierce); ImGui::SameLine();
                    ImGui::TextColored(cPurple, "%d%%", ps.magicPierce);

                    // -- AR / Defense / Base Damage --
                    int dexAR = ps.dexterity * 5;
                    ImGui::TextColored(cGold, "Base AR:"); ImGui::SameLine();
                    ImGui::Text("dex: %d  equip: %d  total: %d", dexAR, ps.attackRating, dexAR + ps.attackRating);

                    int dexDef = ps.dexterity / 4;
                    ImGui::TextColored(cGold, "Base Def:"); ImGui::SameLine();
                    ImGui::Text("dex: %d  equip: %d  total: %d", dexDef, ps.defense, dexDef + ps.defense);

                    ImGui::TextColored(cGold, "Base Damage:"); ImGui::SameLine();
                    ImGui::Text("1h: %d-%d  2h: %d-%d", ps.minDmg, ps.maxDmg, ps.minDmg2, ps.maxDmg2);

                    ImGui::Separator();

                    // -- Breakpoints (includes rates) --
                    float col2 = ImGui::GetContentRegionAvail().x * 0.5f;
                    ImGui::TextColored(cGold, "Breakpoints (%s):", ps.bpSkillName[0] ? ps.bpSkillName : "N/A");

                    // Helper lambda to render a breakpoint line
                    auto drawBP = [&](const GameState::PlayerState::BreakpointInfo& bp) {
                        if (bp.count == 0) return;
                        ImGui::TextColored(cGold, "%s:", bp.label); ImGui::SameLine();
                        ImGui::TextColored(cGold, "(%d)", bp.currentValue); ImGui::SameLine();
                        for (int i = 0; i < bp.count; i++) {
                            if (i == bp.activeIndex) {
                                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.0f, 1.0f), "%d", bp.values[i]);
                            } else {
                                ImGui::Text("%d", bp.values[i]);
                            }
                            if (i < bp.count - 1) {
                                ImGui::SameLine(); ImGui::TextColored(cGray, "/"); ImGui::SameLine();
                            }
                        }
                    };

                    drawBP(ps.bpFCR);
                    drawBP(ps.bpFHR);
                    ImGui::TextColored(cGold, "IAS (Frames):"); ImGui::SameLine(); ImGui::Text("N/A");

                    // Remaining rates on one line
                    ImGui::TextColored(cGold, "Block Rate:"); ImGui::SameLine(); ImGui::Text("%d", ps.fbr);
                    ImGui::SameLine(); ImGui::TextColored(cGold, "  Run/Walk:"); ImGui::SameLine(); ImGui::Text("%d", ps.frw);
                    ImGui::SameLine(); ImGui::TextColored(cGold, "  Attack Rate:"); ImGui::SameLine(); ImGui::Text("%d", ps.attackRate);
                    ImGui::SameLine(); ImGui::TextColored(cGold, "  IAS:"); ImGui::SameLine(); ImGui::Text("%d", ps.ias);

                    // -- Combat stats (2-column) --
                    int dsMax = 75 + ps.maxDeadlyStrike;
                    if (dsMax > 100) dsMax = 100;
                    int dsVal = ps.deadlyStrike < dsMax ? ps.deadlyStrike : dsMax;

                    ImGui::TextColored(cGold, "Crushing Blow:"); ImGui::SameLine(); ImGui::Text("%d", ps.crushingBlow);
                    ImGui::SameLine(col2); ImGui::TextColored(cGold, "Open Wounds:"); ImGui::SameLine(); ImGui::Text("%d%%/+%d", ps.openWounds, ps.deepWounds);
                    ImGui::TextColored(cGold, "Deadly Strike:"); ImGui::SameLine(); ImGui::Text("%d / %d", dsVal, dsMax);
                    ImGui::SameLine(col2); ImGui::TextColored(cGold, "Critical Strike:"); ImGui::SameLine(); ImGui::Text("%d", ps.criticalStrike < 75 ? ps.criticalStrike : 75);
                    ImGui::TextColored(cRed, "Life Leech:"); ImGui::SameLine(); ImGui::TextColored(cRed, "%d", ps.lifeLeech);
                    ImGui::SameLine(col2); ImGui::TextColored(cBlue, "Mana Leech:"); ImGui::SameLine(); ImGui::TextColored(cBlue, "%d", ps.manaLeech);
                    ImGui::TextColored(cGold, "Projectile Pierce:"); ImGui::SameLine(); ImGui::Text("%d", ps.piercingAttack + ps.pierce);
                    ImGui::SameLine(col2); ImGui::TextColored(cGold, "HP/MP per Kill:"); ImGui::SameLine();
                    ImGui::TextColored(cRed, "%d", ps.lifePerKill); ImGui::SameLine(); ImGui::Text("/"); ImGui::SameLine();
                    ImGui::TextColored(cBlue, "%d", ps.manaPerKill);

                    ImGui::Separator();

                    // -- Elemental damage (single line) --
                    auto applyMastery = [](int val, int mastery) { return val + (val * mastery / 100); };
                    int fMin = applyMastery(ps.minFireDmg, ps.fireMastery), fMax = applyMastery(ps.maxFireDmg, ps.fireMastery);
                    int cMin = applyMastery(ps.minColdDmg, ps.coldMastery), cMax = applyMastery(ps.maxColdDmg, ps.coldMastery);
                    int lMin = applyMastery(ps.minLightDmg, ps.lightMastery), lMax = applyMastery(ps.maxLightDmg, ps.lightMastery);
                    int pMin = applyMastery(ps.minPoisonDmg, ps.poisonMastery), pMax = applyMastery(ps.maxPoisonDmg, ps.poisonMastery);
                    int pLen = ps.poisonLenOverride > 0 ? ps.poisonLenOverride : ps.poisonLength;
                    int mMin = applyMastery(ps.minMagicDmg, ps.magicMastery), mMax = applyMastery(ps.maxMagicDmg, ps.magicMastery);

                    ImGui::TextColored(cGold, "Dmg:"); ImGui::SameLine(); ImGui::Text("+%d", ps.addedDamage);
                    ImGui::SameLine(); ImGui::TextColored(cRed, "  Fire:"); ImGui::SameLine(); ImGui::TextColored(cRed, "%d-%d", fMin, fMax);
                    ImGui::SameLine(); ImGui::TextColored(cBlue, "  Cold:"); ImGui::SameLine(); ImGui::TextColored(cBlue, "%d-%d", cMin, cMax);
                    ImGui::SameLine(); ImGui::TextColored(cYellow, "  Ltng:"); ImGui::SameLine(); ImGui::TextColored(cYellow, "%d-%d", lMin, lMax);
                    ImGui::SameLine(); ImGui::TextColored(cGreen, "  Psn:"); ImGui::SameLine();
                    if (pLen > 0) ImGui::TextColored(cGreen, "%d-%d/%.1fs", (int)(pMin / 256.0 * pLen), (int)(pMax / 256.0 * pLen), pLen / 25.0);
                    else ImGui::TextColored(cGreen, "0-0");
                    ImGui::SameLine(); ImGui::TextColored(cPurple, "  Mag:"); ImGui::SameLine(); ImGui::TextColored(cPurple, "%d-%d", mMin, mMax);

                    ImGui::Separator();

                    // -- MF / GF / Stash Gold (single line) --
                    ImGui::TextColored(cBlue, "Magic Find:"); ImGui::SameLine(); ImGui::TextColored(cBlue, "%d", ps.mf);
                    ImGui::SameLine(); ImGui::TextColored(cYellow, "  Gold Find:"); ImGui::SameLine(); ImGui::TextColored(cYellow, "%d", ps.gf);
                    ImGui::SameLine(); ImGui::TextColored(cYellow, "  Stash Gold:"); ImGui::SameLine(); ImGui::TextColored(cYellow, "%d", ps.goldStash);

                    // -- HP / Mana / Stamina bars (single row) --
                    ImGui::Separator();
                    {
                        int hp = ps.hp >> 8, maxHp = ps.maxHp >> 8;
                        int mp = ps.mana >> 8, maxMp = ps.maxMana >> 8;
                        int stam = ps.stamina >> 8, maxStam = ps.maxStamina >> 8;
                        float barW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;

                        char hpLabel[32]; snprintf(hpLabel, sizeof(hpLabel), "HP: %d/%d", hp, maxHp);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                        ImGui::ProgressBar(maxHp > 0 ? (float)hp / maxHp : 0, ImVec2(barW, 0), hpLabel);
                        ImGui::PopStyleColor();

                        ImGui::SameLine();
                        char mpLabel[32]; snprintf(mpLabel, sizeof(mpLabel), "MP: %d/%d", mp, maxMp);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.1f, 0.2f, 0.8f, 1.0f));
                        ImGui::ProgressBar(maxMp > 0 ? (float)mp / maxMp : 0, ImVec2(barW, 0), mpLabel);
                        ImGui::PopStyleColor();

                        ImGui::SameLine();
                        char stamLabel[32]; snprintf(stamLabel, sizeof(stamLabel), "Stam: %d/%d", stam, maxStam);
                        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.6f, 0.6f, 0.1f, 1.0f));
                        ImGui::ProgressBar(maxStam > 0 ? (float)stam / maxStam : 0, ImVec2(barW, 0), stamLabel);
                        ImGui::PopStyleColor();
                    }

                    // -- Auto-Potion Controls --
                    ImGui::Separator();
                    {
                        auto apc = AutoPotion::GetConfig();
                        bool changed = false;

                        ImGui::TextColored(cGold, "Auto-Potion:"); ImGui::SameLine();
                        if (ImGui::Checkbox("##apEnable", &apc.enabled)) changed = true;

                        if (apc.enabled) {
                            ImGui::SameLine();
                            ImGui::PushItemWidth(60 * g_dpiScale);
                            ImGui::TextColored(cRed, "  HP:"); ImGui::SameLine();
                            if (ImGui::InputInt("##hp", &apc.hpThreshold, 0, 0)) changed = true;
                            ImGui::SameLine(); ImGui::Text("%%");
                            ImGui::SameLine();
                            ImGui::TextColored(cBlue, "  MP:"); ImGui::SameLine();
                            if (ImGui::InputInt("##mp", &apc.mpThreshold, 0, 0)) changed = true;
                            ImGui::SameLine(); ImGui::Text("%%");
                            ImGui::SameLine();
                            ImGui::TextColored(cPurple, "  Rejuv:"); ImGui::SameLine();
                            if (ImGui::InputInt("##rejuv", &apc.rejuvThreshold, 0, 0)) changed = true;
                            ImGui::SameLine(); ImGui::Text("%%");
                            ImGui::PopItemWidth();
                        }

                        if (changed) {
                            if (apc.hpThreshold < 0) apc.hpThreshold = 0;
                            if (apc.hpThreshold > 100) apc.hpThreshold = 100;
                            if (apc.mpThreshold < 0) apc.mpThreshold = 0;
                            if (apc.mpThreshold > 100) apc.mpThreshold = 100;
                            if (apc.rejuvThreshold < 0) apc.rejuvThreshold = 0;
                            if (apc.rejuvThreshold > 100) apc.rejuvThreshold = 100;
                            AutoPotion::SetConfig(apc);
                            // Sync to App for persistence
                            App.autoPotion.enabled.value = apc.enabled;
                            App.autoPotion.hpThreshold.value = apc.hpThreshold;
                            App.autoPotion.mpThreshold.value = apc.mpThreshold;
                            App.autoPotion.rejuvThreshold.value = apc.rejuvThreshold;
                        }
                    }

                    // -- Auto-Pickup Controls --
                    {
                        auto auc = AutoPickup::GetConfig();
                        bool changed = false;

                        ImGui::TextColored(cGold, "Auto-Pickup:"); ImGui::SameLine();
                        if (ImGui::Checkbox("##auEnable", &auc.enabled)) changed = true;

                        if (auc.enabled) {
                            ImGui::SameLine();
                            ImGui::PushItemWidth(50 * g_dpiScale);
                            ImGui::TextColored(cGold, "  Range:"); ImGui::SameLine();
                            if (ImGui::InputInt("##range", &auc.maxDistance, 0, 0)) changed = true;
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            if (ImGui::Checkbox("TP##pickTP", &auc.pickTpScrolls)) changed = true;
                            ImGui::SameLine();
                            if (ImGui::Checkbox("ID##pickID", &auc.pickIdScrolls)) changed = true;
                            ImGui::SameLine();
                            if (ImGui::SmallButton("\xe2\x86\xbb##resnap")) { // U+21BB ↻
                                AutoPickup::ResnapBelt();
                            }
                            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-snapshot belt layout");

                            // Show snapshot info using short names from belt
                            auto snap = AutoPickup::GetSnapshot();
                            if (snap.valid) {
                                // Build a code→name map from current belt items
                                auto beltForSnap = GameState::GetBeltState();
                                char snapNames[4][32] = {};
                                for (int c = 0; c < 4; c++) {
                                    if (snap.preferredCode[c] == 0) continue;
                                    // Find a belt item with this code to get its short name
                                    for (int s = 0; s < 16; s++) {
                                        if (beltForSnap.slots[s].occupied && beltForSnap.slots[s].itemCode == snap.preferredCode[c]) {
                                            strncpy_s(snapNames[c], beltForSnap.slots[s].name, sizeof(snapNames[c]));
                                            break;
                                        }
                                    }
                                    // Fallback to code if no matching belt item found
                                    if (!snapNames[c][0]) snprintf(snapNames[c], sizeof(snapNames[c]), "#%d", snap.preferredCode[c]);
                                }

                                ImGui::SameLine();
                                ImGui::TextColored(cGray, " Snap:");
                                for (int c = 0; c < 4; c++) {
                                    ImGui::SameLine();
                                    if (snap.preferredCode[c] > 0) {
                                        ImGui::Text("[%s]", snapNames[c]);
                                    } else {
                                        ImGui::TextColored(cGray, "[--]");
                                    }
                                }
                            }
                        }

                        if (changed) {
                            if (auc.maxDistance < 1) auc.maxDistance = 1;
                            if (auc.maxDistance > 40) auc.maxDistance = 40;
                            AutoPickup::SetConfig(auc);
                            // Sync to App for persistence
                            App.autoPickup.enabled.value = auc.enabled;
                            App.autoPickup.maxDistance.value = auc.maxDistance;
                            App.autoPickup.pickTpScrolls.value = auc.pickTpScrolls;
                            App.autoPickup.pickIdScrolls.value = auc.pickIdScrolls;
                        }
                    }

                    // -- Auto-Cast Controls --
                    {
                        auto acc = AutoCast::GetConfig();
                        bool changed = false;

                        // Line 1: Auto-Cast: [✓] R: [skill▼] backup: [skill▼]  [✓] L: [skill▼]
                        ImGui::TextColored(cGold, "Auto-Cast:"); ImGui::SameLine();
                        if (ImGui::Checkbox("##acEnable", &acc.enabled)) changed = true;

                        if (acc.enabled) {
                            // Get available skills for dropdowns
                            static std::vector<AutoCast::SkillInfo> s_skills;
                            static DWORD s_lastSkillRefresh = 0;
                            DWORD now = GetTickCount();
                            if (now - s_lastSkillRefresh > 2000) {
                                s_skills = AutoCast::GetAvailableSkills();
                                s_lastSkillRefresh = now;
                            }

                            // Build skill name list for combos
                            auto findSkillIdx = [&](WORD id) -> int {
                                for (int i = 0; i < (int)s_skills.size(); i++)
                                    if (s_skills[i].id == id) return i;
                                return -1;
                            };

                            ImGui::SameLine();
                            if (ImGui::Checkbox("R##acR", &acc.rightSkill.enabled)) changed = true;
                            ImGui::SameLine();
                            ImGui::PushItemWidth(80 * g_dpiScale);
                            {
                                int sel = findSkillIdx(acc.rightSkill.skillId);
                                char preview[32] = "None";
                                if (sel >= 0) snprintf(preview, sizeof(preview), "%d", s_skills[sel].id);
                                if (ImGui::BeginCombo("##acRSkill", preview, ImGuiComboFlags_NoArrowButton)) {
                                    for (int i = 0; i < (int)s_skills.size(); i++) {
                                        char label[32]; snprintf(label, sizeof(label), "%d", s_skills[i].id);
                                        if (ImGui::Selectable(label, sel == i)) {
                                            acc.rightSkill.skillId = s_skills[i].id;
                                            changed = true;
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                            }
                            ImGui::PopItemWidth();

                            ImGui::SameLine();
                            ImGui::TextColored(cGray, "bk:"); ImGui::SameLine();
                            ImGui::PushItemWidth(60 * g_dpiScale);
                            {
                                int sel = findSkillIdx(acc.rightSkill.backupSkillId);
                                char preview[32] = "--";
                                if (sel >= 0) snprintf(preview, sizeof(preview), "%d", s_skills[sel].id);
                                if (ImGui::BeginCombo("##acRBk", preview, ImGuiComboFlags_NoArrowButton)) {
                                    if (ImGui::Selectable("None", acc.rightSkill.backupSkillId == 0)) {
                                        acc.rightSkill.backupSkillId = 0; changed = true;
                                    }
                                    for (int i = 0; i < (int)s_skills.size(); i++) {
                                        char label[32]; snprintf(label, sizeof(label), "%d", s_skills[i].id);
                                        if (ImGui::Selectable(label, sel == i)) {
                                            acc.rightSkill.backupSkillId = s_skills[i].id;
                                            changed = true;
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                            }
                            ImGui::PopItemWidth();

                            ImGui::SameLine();
                            ImGui::Text(" ");
                            ImGui::SameLine();
                            if (ImGui::Checkbox("L##acL", &acc.leftSkill.enabled)) changed = true;
                            ImGui::SameLine();
                            ImGui::PushItemWidth(80 * g_dpiScale);
                            {
                                int sel = findSkillIdx(acc.leftSkill.skillId);
                                char preview[32] = "None";
                                if (sel >= 0) snprintf(preview, sizeof(preview), "%d", s_skills[sel].id);
                                if (ImGui::BeginCombo("##acLSkill", preview, ImGuiComboFlags_NoArrowButton)) {
                                    for (int i = 0; i < (int)s_skills.size(); i++) {
                                        char label[32]; snprintf(label, sizeof(label), "%d", s_skills[i].id);
                                        if (ImGui::Selectable(label, sel == i)) {
                                            acc.leftSkill.skillId = s_skills[i].id;
                                            changed = true;
                                        }
                                    }
                                    ImGui::EndCombo();
                                }
                            }
                            ImGui::PopItemWidth();

                            // Line 2: Range: [==25==]  Priority: [Nearest▼]  Mana: [==20%==]  [✓] Moving
                            ImGui::TextColored(cGold, "  Range:"); ImGui::SameLine();
                            ImGui::PushItemWidth(60 * g_dpiScale);
                            if (ImGui::InputInt("##acRange", &acc.castRange, 0, 0)) changed = true;
                            ImGui::PopItemWidth();

                            ImGui::SameLine();
                            ImGui::TextColored(cGold, " Pri:"); ImGui::SameLine();
                            ImGui::PushItemWidth(100 * g_dpiScale);
                            {
                                int priIdx = (int)acc.priority;
                                if (ImGui::Combo("##acPri", &priIdx, AutoCast::TargetPriorityNames, (int)AutoCast::TargetPriority::COUNT)) {
                                    acc.priority = (AutoCast::TargetPriority)priIdx;
                                    changed = true;
                                }
                            }
                            ImGui::PopItemWidth();

                            ImGui::SameLine();
                            ImGui::TextColored(cBlue, " Mana:"); ImGui::SameLine();
                            ImGui::PushItemWidth(45 * g_dpiScale);
                            if (ImGui::InputInt("##acMana", &acc.manaReservePct, 0, 0)) changed = true;
                            ImGui::PopItemWidth();
                            ImGui::SameLine(); ImGui::Text("%%");

                            ImGui::SameLine();
                            if (ImGui::Checkbox("Moving##acMove", &acc.castWhileMoving)) changed = true;

                            // Line 3: Auto-Buff slots
                            if (!acc.buffs.empty()) {
                                ImGui::TextColored(cGold, "  Auto-Buff:");
                                for (int b = 0; b < (int)acc.buffs.size(); b++) {
                                    ImGui::SameLine();
                                    char cbId[16]; snprintf(cbId, sizeof(cbId), "##ab%d", b);
                                    if (ImGui::Checkbox(cbId, &acc.buffs[b].enabled)) changed = true;
                                    ImGui::SameLine();
                                    ImGui::PushItemWidth(60 * g_dpiScale);
                                    char comboId[16]; snprintf(comboId, sizeof(comboId), "##abs%d", b);
                                    {
                                        int sel = findSkillIdx(acc.buffs[b].skillId);
                                        char preview[32] = "--";
                                        if (sel >= 0) snprintf(preview, sizeof(preview), "%d", s_skills[sel].id);
                                        if (ImGui::BeginCombo(comboId, preview, ImGuiComboFlags_NoArrowButton)) {
                                            for (int i = 0; i < (int)s_skills.size(); i++) {
                                                char label[32]; snprintf(label, sizeof(label), "%d", s_skills[i].id);
                                                if (ImGui::Selectable(label, sel == i)) {
                                                    acc.buffs[b].skillId = s_skills[i].id;
                                                    changed = true;
                                                }
                                            }
                                            ImGui::EndCombo();
                                        }
                                    }
                                    ImGui::PopItemWidth();
                                    if (acc.buffs[b].enabled && acc.buffs[b].lastCastTick > 0) {
                                        DWORD elapsed = (GetTickCount() - acc.buffs[b].lastCastTick) / 1000;
                                        ImGui::SameLine();
                                        ImGui::TextColored(cGray, "(%ds)", elapsed);
                                    }
                                }
                            }

                            // + button to add buff slot
                            if (acc.buffs.size() < 6) {
                                if (!acc.buffs.empty()) ImGui::SameLine();
                                else { ImGui::TextColored(cGold, "  Auto-Buff:"); ImGui::SameLine(); }
                                char addId[16]; snprintf(addId, sizeof(addId), "+##addBuff");
                                if (ImGui::SmallButton(addId)) {
                                    AutoCast::BuffSlot slot;
                                    slot.enabled = false;
                                    slot.durationSec = 120; // default 2 min
                                    acc.buffs.push_back(slot);
                                    changed = true;
                                }
                            }
                        }

                        if (changed) {
                            if (acc.castRange < 1) acc.castRange = 1;
                            if (acc.castRange > 50) acc.castRange = 50;
                            if (acc.manaReservePct < 0) acc.manaReservePct = 0;
                            if (acc.manaReservePct > 95) acc.manaReservePct = 95;
                            AutoCast::SetConfig(acc);
                        }
                    }

                    // -- Belt --
                    ImGui::Separator();
                    ImGui::TextColored(cGold, "Belt:");
                    auto belt = GameState::GetBeltState();
                    for (int row = 0; row < belt.rows; ++row) {
                        for (int col = 0; col < belt.columns; ++col) {
                            if (col > 0) ImGui::SameLine();
                            int idx = row * belt.columns + col;
                            const auto& slot = belt.slots[idx];
                            if (slot.occupied) {
                                ImGui::Button(slot.name, ImVec2(80 * g_dpiScale, 24 * g_dpiScale));
                            } else {
                                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                                ImGui::Button("--", ImVec2(80 * g_dpiScale, 24 * g_dpiScale));
                                ImGui::PopStyleColor();
                            }
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World")) {
                if (!GameState::IsGameReady()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Not in game");
                } else {
                    auto units = GameState::GetNearbyUnits(40);

                    // Split into monsters and items
                    ImGui::Text("Nearby Units: %d", (int)units.size());
                    ImGui::Separator();

                    if (ImGui::BeginTable("UnitsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70 * g_dpiScale);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("HP", ImGuiTableColumnFlags_WidthFixed, 80 * g_dpiScale);
                        ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_WidthFixed, 50 * g_dpiScale);
                        ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed, 100 * g_dpiScale);
                        ImGui::TableHeadersRow();

                        for (const auto& u : units) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();

                            const char* typeNames[] = {"Player", "Monster", "Object", "Missile", "Item", "Tile"};
                            const char* tname = (u.type >= 0 && u.type <= 5) ? typeNames[u.type] : "?";

                            if (u.isBoss) ImGui::TextColored(ImVec4(1.0f, 0.84f, 0.0f, 1.0f), "%s", tname);
                            else if (u.isChampion) ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "%s", tname);
                            else ImGui::Text("%s", tname);

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", u.name);

                            ImGui::TableNextColumn();
                            if (u.type == 1 || u.type == 0) {
                                int hp = u.hp >> 8, mhp = u.maxHp >> 8;
                                if (u.mode == 0 || u.mode == 12)
                                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Dead");
                                else
                                    ImGui::Text("%d/%d", hp, mhp);
                            }

                            ImGui::TableNextColumn();
                            ImGui::Text("%d", u.distance);

                            ImGui::TableNextColumn();
                            // Show immunities for monsters
                            if (u.type == 1) {
                                bool first = true;
                                auto showImm = [&](int res, const char* label, ImVec4 color) {
                                    if (res >= 100) {
                                        if (!first) ImGui::SameLine(0, 2);
                                        ImGui::TextColored(color, "%s", label);
                                        first = false;
                                    }
                                };
                                showImm(u.fireRes, "Fi", ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                                showImm(u.coldRes, "Co", ImVec4(0.4f, 0.6f, 1.0f, 1.0f));
                                showImm(u.lightRes, "Li", ImVec4(1.0f, 1.0f, 0.3f, 1.0f));
                                showImm(u.poisonRes, "Po", ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                                showImm(u.physRes, "Ph", ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
                                showImm(u.magicRes, "Ma", ImVec4(0.8f, 0.5f, 1.0f, 1.0f));
                                if (first) ImGui::Text("(%d,%d)", u.x, u.y);
                            } else {
                                ImGui::Text("(%d,%d)", u.x, u.y);
                            }
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Debug")) {
                ImVec4 cGold2(0.85f, 0.72f, 0.45f, 1.0f);
                ImVec4 cGreen2(0.4f, 1.0f, 0.4f, 1.0f);
                ImVec4 cRed2(1.0f, 0.4f, 0.4f, 1.0f);
                ImVec4 cGray2(0.6f, 0.6f, 0.6f, 1.0f);

                // -- Installed Hooks --
                auto hooks = HookManager::ListHooks();
                ImGui::TextColored(cGold2, "Hooks: %d", (int)hooks.size());
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove All##hooks")) {
                    HookManager::RemoveAllHooks();
                }

                if (!hooks.empty()) {
                    if (ImGui::BeginTable("HooksTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90 * g_dpiScale);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 70 * g_dpiScale);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30 * g_dpiScale);
                        ImGui::TableHeadersRow();

                        for (auto& h : hooks) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", h.config.address);
                            ImGui::Text("%s", addrBuf);
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", h.config.name.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", h.callCount);
                            ImGui::TableNextColumn();
                            char btnId[32]; snprintf(btnId, sizeof(btnId), "X##%08X", h.config.address);
                            if (ImGui::SmallButton(btnId)) {
                                HookManager::RemoveHook(h.config.address);
                            }
                        }
                        ImGui::EndTable();
                    }
                }

                ImGui::Separator();

                // -- Call Log --
                int logSize = HookManager::GetCallLogSize();
                ImGui::TextColored(cGold2, "Call Log: %d entries", logSize);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##log")) {
                    HookManager::ClearCallLog();
                }

                if (logSize > 0) {
                    // Show last N entries
                    auto records = HookManager::GetCallLog(50, 0);
                    if (ImGui::BeginTable("CallLogTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200 * g_dpiScale))) {
                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90 * g_dpiScale);
                        ImGui::TableSetupColumn("Return", ImGuiTableColumnFlags_WidthFixed, 90 * g_dpiScale);
                        ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 70 * g_dpiScale);
                        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80 * g_dpiScale);
                        ImGui::TableHeadersRow();

                        // Show in reverse order (newest first)
                        for (int i = (int)records.size() - 1; i >= 0; i--) {
                            auto& r = records[i];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            char addrBuf[16]; snprintf(addrBuf, sizeof(addrBuf), "0x%08X", r.address);
                            ImGui::Text("%s", addrBuf);
                            ImGui::TableNextColumn();
                            if (r.hasReturnValue) {
                                char retBuf[16]; snprintf(retBuf, sizeof(retBuf), "0x%08X", r.returnValue);
                                ImGui::TextColored(r.returnValue ? cGreen2 : cGray2, "%s", retBuf);
                            }
                            ImGui::TableNextColumn();
                            ImGui::Text("%u", r.threadId);
                            ImGui::TableNextColumn();
                            ImGui::Text("%u", r.timestamp);
                        }
                        ImGui::EndTable();
                    }
                }

                ImGui::Separator();

                // -- Crash Log --
                int crashCount = CrashCatcher::GetCrashCount();
                ImGui::TextColored(crashCount > 0 ? cRed2 : cGold2, "Crashes: %d", crashCount);
                if (crashCount > 0) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear##crash")) CrashCatcher::ClearCrashLog();

                    auto crashes = CrashCatcher::GetCrashLog();
                    if (ImGui::BeginTable("CrashTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 150 * g_dpiScale))) {
                        ImGui::TableSetupColumn("Exception", ImGuiTableColumnFlags_WidthFixed, 120 * g_dpiScale);
                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 130 * g_dpiScale);
                        ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthFixed, 70 * g_dpiScale);
                        ImGui::TableHeadersRow();

                        for (int i = (int)crashes.size() - 1; i >= 0; i--) {
                            auto& c = crashes[i];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextColored(cRed2, "%s", CrashCatcher::GetExceptionName(c.exceptionCode));
                            ImGui::TableNextColumn();
                            ImGui::Text("0x%08X", c.exceptionAddress);
                            ImGui::TableNextColumn();
                            ImGui::Text("%s+0x%X", c.moduleName, c.moduleOffset);
                            ImGui::TableNextColumn();
                            ImGui::Text("%u", c.threadId);

                            // Tooltip with full details on hover
                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::Text("EAX=%08X EBX=%08X ECX=%08X EDX=%08X", c.eax, c.ebx, c.ecx, c.edx);
                                ImGui::Text("ESI=%08X EDI=%08X ESP=%08X EBP=%08X", c.esi, c.edi, c.esp, c.ebp);
                                ImGui::Text("EIP=%08X EFLAGS=%08X", c.eip, c.eflags);
                                if (c.exceptionCode == 0xC0000005) {
                                    ImGui::Text("Fault Address: 0x%08X", c.faultAddress);
                                }
                                ImGui::Separator();
                                ImGui::Text("Stack Trace:");
                                for (int j = 0; j < c.stackDepth; j++) {
                                    ImGui::Text("  [%d] 0x%08X", j, c.stackTrace[j]);
                                }
                                ImGui::EndTooltip();
                            }
                        }
                        ImGui::EndTable();
                    }
                }

                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Memory")) {
                ImVec4 cGold4(0.85f, 0.72f, 0.45f, 1.0f);
                ImVec4 cGreen4(0.4f, 1.0f, 0.4f, 1.0f);
                ImVec4 cRed4(1.0f, 0.4f, 0.4f, 1.0f);
                ImVec4 cYellow4(1.0f, 1.0f, 0.3f, 1.0f);
                ImVec4 cGray4(0.6f, 0.6f, 0.6f, 1.0f);

                auto watches = MemWatch::GetWatches();
                ImGui::TextColored(cGold4, "Watch List: %d", (int)watches.size());
                if (!watches.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear All##watches")) MemWatch::RemoveAllWatches();
                }

                if (!watches.empty()) {
                    if (ImGui::BeginTable("WatchTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90 * g_dpiScale);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 90 * g_dpiScale);
                        ImGui::TableSetupColumn("Dec", ImGuiTableColumnFlags_WidthFixed, 70 * g_dpiScale);
                        ImGui::TableSetupColumn("Changes", ImGuiTableColumnFlags_WidthFixed, 60 * g_dpiScale);
                        ImGui::TableHeadersRow();

                        for (auto& w : watches) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", w.name.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("0x%08X", w.address);
                            ImGui::TableNextColumn();
                            ImGui::TextColored(w.changed ? cYellow4 : cGray4, "0x%08X", w.currentValue);
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", (int)w.currentValue);
                            ImGui::TableNextColumn();
                            ImGui::TextColored(w.changeCount > 0 ? cYellow4 : cGray4, "%d", w.changeCount);
                        }
                        ImGui::EndTable();
                    }
                } else {
                    ImGui::TextColored(cGray4, "No watches. Use MCP add_watch to monitor addresses.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Patches")) {
                ImVec4 cGold3(0.85f, 0.72f, 0.45f, 1.0f);
                ImVec4 cGreen3(0.4f, 1.0f, 0.4f, 1.0f);
                ImVec4 cRed3(1.0f, 0.4f, 0.4f, 1.0f);
                ImVec4 cGray3(0.6f, 0.6f, 0.6f, 1.0f);

                auto patches = PatchManager::ListPatches();
                ImGui::TextColored(cGold3, "Patches: %d", (int)patches.size());

                if (!patches.empty()) {
                    if (ImGui::BeginTable("PatchTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90 * g_dpiScale);
                        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 40 * g_dpiScale);
                        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 50 * g_dpiScale);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 50 * g_dpiScale);
                        ImGui::TableHeadersRow();

                        for (auto& p : patches) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", p.name.c_str());
                            if (ImGui::IsItemHovered()) {
                                ImGui::BeginTooltip();
                                ImGui::Text("Source: %s", p.source.c_str());
                                ImGui::Text("Original: %s", p.originalHex.c_str());
                                ImGui::Text("Patched:  %s", p.patchedHex.c_str());
                                ImGui::EndTooltip();
                            }
                            ImGui::TableNextColumn();
                            ImGui::Text("0x%08X", p.address);
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", p.size);
                            ImGui::TableNextColumn();
                            ImGui::TextColored(p.active ? cGreen3 : cRed3, p.active ? "ON" : "OFF");
                            ImGui::TableNextColumn();
                            char btnId[64]; snprintf(btnId, sizeof(btnId), "Toggle##%s", p.name.c_str());
                            if (ImGui::SmallButton(btnId)) {
                                PatchManager::TogglePatch(p.name);
                            }
                        }
                        ImGui::EndTable();
                    }
                } else {
                    ImGui::TextColored(cGray3, "No patches applied. Use MCP apply_patch or import_patches.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Rendering")) {
                ImVec4 cGold5(0.85f, 0.72f, 0.45f, 1.0f);
                ImVec4 cGray5(0.6f, 0.6f, 0.6f, 1.0f);

                if (!GameState::IsGameReady()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Not in game");
                } else {
                    // Screen info
                    DWORD screenW = *p_D2CLIENT_ScreenSizeX;
                    DWORD screenH = *p_D2CLIENT_ScreenSizeY;
                    DWORD fps = *p_D2CLIENT_FPS;
                    ImGui::TextColored(cGold5, "Screen:"); ImGui::SameLine();
                    ImGui::Text("%dx%d", screenW, screenH);
                    ImGui::SameLine(); ImGui::TextColored(cGold5, "  FPS:"); ImGui::SameLine();
                    ImGui::Text("%d", fps);

                    // Mouse
                    DWORD mouseX = *p_D2CLIENT_MouseX;
                    DWORD mouseY = *p_D2CLIENT_MouseY;
                    int mouseOffX = *p_D2CLIENT_MouseOffsetX;
                    int mouseOffY = *p_D2CLIENT_MouseOffsetY;
                    ImGui::TextColored(cGold5, "Mouse:"); ImGui::SameLine();
                    ImGui::Text("(%d, %d)", mouseX, mouseY);
                    ImGui::SameLine(); ImGui::TextColored(cGold5, "  Offset:"); ImGui::SameLine();
                    ImGui::Text("(%d, %d)", mouseOffX, mouseOffY);

                    // Camera/viewport offset
                    POINT* pOffset = p_D2CLIENT_Offset;
                    if (pOffset) {
                        ImGui::TextColored(cGold5, "Camera Offset:"); ImGui::SameLine();
                        ImGui::Text("(%d, %d)", pOffset->x, pOffset->y);
                    }

                    // Panel offset
                    int panelOffX = *p_D2CLIENT_PanelOffsetX;
                    ImGui::TextColored(cGold5, "Panel Offset X:"); ImGui::SameLine();
                    ImGui::Text("%d", panelOffX);

                    // Automap
                    ImGui::Separator();
                    DWORD automapOn = *p_D2CLIENT_AutomapOn;
                    int automapMode = *p_D2CLIENT_AutomapMode;
                    ImGui::TextColored(cGold5, "Automap:"); ImGui::SameLine();
                    ImGui::Text("%s (mode %d)", automapOn ? "ON" : "OFF", automapMode);

                    // Player world position
                    auto ps = GameState::GetPlayerState();
                    ImGui::TextColored(cGold5, "World Pos:"); ImGui::SameLine();
                    ImGui::Text("(%d, %d)", ps.x, ps.y);

                    // Area info from game state
                    ImGui::TextColored(cGold5, "Area:"); ImGui::SameLine();
                    ImGui::Text("%s (%d)", ps.areaName[0] ? ps.areaName : "?", ps.area);

                    // Renderer info
                    ImGui::Separator();
                    ImGui::TextColored(cGold5, "Renderer:"); ImGui::SameLine();
                    DWORD gfxScreenSize = D2GFX_GetScreenSize();
                    const char* renderers[] = {"GDI", "Software", "DirectDraw", "Glide", "OpenGL", "Direct3D"};
                    ImGui::Text("Glide (3dfx mode)");  // We always use -3dfx
                    ImGui::TextColored(cGold5, "GFX Screen Size:"); ImGui::SameLine();
                    ImGui::Text("%d", gfxScreenSize);
                }
                ImGui::EndTabItem();
            }
            // === INVENTORY TAB ===
            if (ImGui::BeginTabItem("Inventory")) {
                if (!GameState::IsGameReady()) {
                    ImGui::TextColored(cGray6, "Not in game.");
                } else {
                    UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
                    if (pPlayer && pPlayer->pInventory) {
                        // Scan all items once
                        struct InvItem {
                            DWORD unitId; int code; char name[64]; char category[32];
                            int gridX, gridY; int bodyLoc; int itemLoc; int nodePage;
                            int quality;
                        };
                        static std::vector<InvItem> equippedItems;
                        static std::vector<InvItem> backpackItems;
                        static std::vector<InvItem> cubeItems;
                        static DWORD lastScanTick = 0;

                        // Rescan every 500ms when tab is visible
                        DWORD now = GetTickCount();
                        if (now - lastScanTick > 500) {
                            lastScanTick = now;
                            equippedItems.clear();
                            backpackItems.clear();
                            cubeItems.clear();

                            UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
                            while (pItem) {
                                if (pItem->pItemData) {
                                    InvItem ii = {};
                                    ii.unitId = pItem->dwUnitId;
                                    ii.code = pItem->dwTxtFileNo;
                                    ii.nodePage = pItem->pItemData->NodePage;
                                    ii.bodyLoc = pItem->pItemData->BodyLocation;
                                    ii.itemLoc = pItem->pItemData->ItemLocation;
                                    ii.quality = pItem->pItemData->dwQuality;
                                    if (pItem->pPath) {
                                        ItemPath* ip = (ItemPath*)pItem->pPath;
                                        ii.gridX = (int)ip->dwPosX;
                                        ii.gridY = (int)ip->dwPosY;
                                    }
                                    wchar_t* wName = D2CLIENT_GetUnitName(pItem);
                                    if (wName) WideCharToMultiByte(CP_UTF8, 0, wName, -1, ii.name, sizeof(ii.name)-1, nullptr, nullptr);
                                    if (!ii.name[0]) snprintf(ii.name, sizeof(ii.name), "Item #%d", ii.code);

                                    if (ii.nodePage == NODEPAGE_EQUIP) {
                                        equippedItems.push_back(ii);
                                    } else if (ii.nodePage == NODEPAGE_STORAGE && ii.itemLoc == 0) {
                                        backpackItems.push_back(ii);
                                    } else if (ii.nodePage == NODEPAGE_STORAGE && ii.itemLoc == 3) {
                                        cubeItems.push_back(ii);
                                    }
                                }
                                pItem = D2COMMON_GetNextItemFromInventory(pItem);
                            }
                        }

                        // Quality colors
                        auto qualityColor = [](int q) -> ImVec4 {
                            switch(q) {
                                case 1: return ImVec4(0.6f,0.6f,0.6f,1); // inferior
                                case 3: return ImVec4(0.7f,0.7f,0.7f,1); // superior
                                case 4: return ImVec4(0.3f,0.5f,1.0f,1); // magic
                                case 5: return ImVec4(0.0f,0.8f,0.0f,1); // set
                                case 6: return ImVec4(1.0f,1.0f,0.3f,1); // rare
                                case 7: return ImVec4(0.85f,0.72f,0.45f,1); // unique
                                case 8: return ImVec4(1.0f,0.6f,0.2f,1); // crafted
                                default: return ImVec4(1,1,1,1); // normal/other
                            }
                        };

                        // --- EQUIPPED SECTION ---
                        ImGui::TextColored(cGold, "Equipped (%d)", (int)equippedItems.size());
                        if (!equippedItems.empty()) {
                            static const char* slotNames[] = {
                                "None", "Head", "Amulet", "Body", "Right Hand", "Left Hand",
                                "Right Ring", "Left Ring", "Belt", "Feet", "Gloves",
                                "Right Swap", "Left Swap"
                            };

                            if (ImGui::BeginTable("Equipped", 3,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                                ImGui::TableSetupColumn("Slot", 0, 1.0f);
                                ImGui::TableSetupColumn("Item", 0, 2.5f);
                                ImGui::TableSetupColumn("ID", 0, 0.5f);
                                ImGui::TableHeadersRow();

                                // Sort by body location
                                for (int slot = 1; slot <= 12; slot++) {
                                    for (auto& item : equippedItems) {
                                        if (item.bodyLoc != slot) continue;
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();
                                        ImGui::TextColored(cGray6, "%s", (slot >= 0 && slot <= 12) ? slotNames[slot] : "?");
                                        ImGui::TableNextColumn();
                                        ImGui::TextColored(qualityColor(item.quality), "%s", item.name);
                                        ImGui::TableNextColumn();
                                        ImGui::TextColored(cGray6, "%d", item.unitId);
                                    }
                                }
                                ImGui::EndTable();
                            }
                        }

                        ImGui::Spacing();

                        // --- BACKPACK SECTION ---
                        ImGui::TextColored(cGold, "Backpack (%d)", (int)backpackItems.size());
                        if (!backpackItems.empty()) {
                            if (ImGui::BeginTable("Backpack", 4,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                                ImGui::TableSetupColumn("Item", 0, 2.5f);
                                ImGui::TableSetupColumn("Pos", 0, 0.6f);
                                ImGui::TableSetupColumn("Quality", 0, 0.7f);
                                ImGui::TableSetupColumn("ID", 0, 0.5f);
                                ImGui::TableHeadersRow();

                                for (auto& item : backpackItems) {
                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(qualityColor(item.quality), "%s", item.name);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("(%d,%d)", item.gridX, item.gridY);
                                    ImGui::TableNextColumn();
                                    static const char* qualNames[] = {"?","Inferior","Normal","Superior","Magic","Set","Rare","Unique","Crafted"};
                                    int qi = item.quality;
                                    ImGui::TextColored(qualityColor(qi), "%s", (qi >= 0 && qi <= 8) ? qualNames[qi] : "?");
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(cGray6, "%d", item.unitId);
                                }
                                ImGui::EndTable();
                            }
                        } else {
                            ImGui::TextColored(cGray6, "  Empty");
                        }

                        ImGui::Spacing();

                        // --- CUBE SECTION ---
                        ImGui::TextColored(cGold, "Cube (%d)", (int)cubeItems.size());
                        if (!cubeItems.empty()) {
                            if (ImGui::BeginTable("Cube", 4,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                                ImGui::TableSetupColumn("Item", 0, 2.5f);
                                ImGui::TableSetupColumn("Pos", 0, 0.6f);
                                ImGui::TableSetupColumn("Quality", 0, 0.7f);
                                ImGui::TableSetupColumn("ID", 0, 0.5f);
                                ImGui::TableHeadersRow();

                                for (auto& item : cubeItems) {
                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(qualityColor(item.quality), "%s", item.name);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("(%d,%d)", item.gridX, item.gridY);
                                    ImGui::TableNextColumn();
                                    static const char* qualNames[] = {"?","Inferior","Normal","Superior","Magic","Set","Rare","Unique","Crafted"};
                                    int qi = item.quality;
                                    ImGui::TextColored(qualityColor(qi), "%s", (qi >= 0 && qi <= 8) ? qualNames[qi] : "?");
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(cGray6, "%d", item.unitId);
                                }
                                ImGui::EndTable();
                            }
                        } else {
                            ImGui::TextColored(cGray6, "  Empty");
                        }
                    } else {
                        ImGui::TextColored(cGray6, "No inventory data.");
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Stash")) {
                // Read current tab index from PlayerData+0x1B4
                int currentTab = -1;
                if (GameState::IsGameReady()) {
                    UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
                    if (pPlayer && pPlayer->pPlayerData) {
                        DWORD pdataAddr = (DWORD)pPlayer->pPlayerData;
                        if (pdataAddr > 0x10000) {
                            currentTab = *(int*)(pdataAddr + 0x1B4);
                        }
                    }

                    // Tab selector buttons
                    const char* tabLabels[] = {"P", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "M"};
                    for (int t = 0; t < 11; t++) {
                        if (t > 0) ImGui::SameLine();
                        bool isActive = (currentTab == t);
                        if (isActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.3f, 1.0f));
                        if (ImGui::SmallButton(tabLabels[t])) {
                            if (t != currentTab) {
                                static const DWORD tabHandlerRVA[] = {
                                    0x001906c0, 0x00190700, 0x00190740, 0x00190780, 0x001907c0,
                                    0x00190800, 0x00190840, 0x00190880, 0x001908c0, 0x00190900,
                                };
                                HMODULE hPD2 = GetModuleHandle("ProjectDiablo.dll");
                                if (hPD2) {
                                    GameCallQueue::PendingCall call = {};
                                    if (t <= 9) {
                                        call.address = (DWORD)hPD2 + tabHandlerRVA[t];
                                        call.argCount = 0;
                                        call.convention = 1;
                                    } else {
                                        struct MatTabHelper {
                                            static DWORD __cdecl Switch() {
                                                HMODULE pd2 = GetModuleHandle("ProjectDiablo.dll");
                                                if (!pd2) return 0;
                                                DWORD* pSt = (DWORD*)(*(DWORD*)((DWORD)pd2 + 0x00410688));
                                                if (*pSt != 0x0C) return 0;
                                                DWORD* pTab = (DWORD*)((DWORD)pd2 + 0x0040edd4);
                                                if (*pTab == 10) return 0;
                                                DWORD* pPend = (DWORD*)((DWORD)pd2 + 0x0030de48);
                                                if (*pPend != 0) return 0;
                                                DWORD dAddr = (DWORD)pd2 + 0x0023ead0;
                                                WORD pkt = 0x0B55;
                                                __asm {
                                                    lea ecx, pkt
                                                    mov edx, 2
                                                    call dAddr
                                                }
                                                return 1;
                                            }
                                        };
                                        call.address = (DWORD)&MatTabHelper::Switch;
                                        call.argCount = 0;
                                        call.convention = 1;
                                    }
                                    GameCallQueue::CallOnGameThread(call, 3000);
                                    Sleep(300);
                                }
                            }

                            g_stashScanTab = t;
                            g_stashScanItems.clear();

                            if (pPlayer && pPlayer->pInventory) {
                                UnitAny* pItem = D2COMMON_GetItemFromInventory(pPlayer->pInventory);
                                while (pItem) {
                                    if (pItem->pItemData && pItem->pItemData->ItemLocation == 4) {
                                        StashItem si = {};
                                        si.unitId = pItem->dwUnitId;
                                        si.code = pItem->dwTxtFileNo;
                                        si.tab = t;
                                        if (pItem->pPath) {
                                            ItemPath* ip = (ItemPath*)pItem->pPath;
                                            si.gridX = (int)ip->dwPosX;
                                            si.gridY = (int)ip->dwPosY;
                                        }
                                        char itemName[64] = {};
                                        wchar_t* wName = D2CLIENT_GetUnitName(pItem);
                                        if (wName) WideCharToMultiByte(CP_UTF8, 0, wName, -1, itemName, sizeof(itemName)-1, nullptr, nullptr);
                                        strncpy_s(si.name, itemName[0] ? itemName : "?", sizeof(si.name));
                                        strncpy_s(si.category, CategorizeItemName(si.name), sizeof(si.category));
                                        g_stashScanItems.push_back(si);
                                    }
                                    pItem = D2COMMON_GetNextItemFromInventory(pItem);
                                }
                            }
                        }
                        if (isActive) ImGui::PopStyleColor();
                    }

                    ImGui::SameLine();
                    ImGui::TextColored(cGold, " Tab: %d", currentTab);
                    ImGui::SameLine();
                    ImGui::TextColored(cGray6, "(%d items)", (int)g_stashScanItems.size());
                    ImGui::Separator();

                    if (!g_stashScanItems.empty()) {
                        std::map<std::string, std::vector<const StashItem*>> groups;
                        for (auto& si : g_stashScanItems) {
                            groups[si.category].push_back(&si);
                        }

                        if (ImGui::BeginTable("StashItems", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 90 * g_dpiScale);
                            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed, 50 * g_dpiScale);
                            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40 * g_dpiScale);
                            ImGui::TableHeadersRow();

                            for (auto& kv : groups) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::TextColored(cGold, "%s (%d)", kv.first.c_str(), (int)kv.second.size());
                                ImGui::TableNextColumn();
                                ImGui::TableNextColumn();
                                ImGui::TableNextColumn();

                                for (auto* si : kv.second) {
                                    ImGui::TableNextRow();
                                    ImGui::TableNextColumn();
                                    ImGui::TextColored(cGray6, "  ");
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%s", si->name);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("(%d,%d)", si->gridX, si->gridY);
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%d", si->unitId);
                                }
                            }
                            ImGui::EndTable();
                        }
                    } else {
                        ImGui::TextColored(cGray6, "Click a tab button above to load items.");
                        ImGui::TextColored(cGray6, "Stash must be open in-game.");
                    }
                } else {
                    ImGui::TextColored(cGray6, "Not in game.");
                }
                ImGui::EndTabItem();
            }
            // === SKILLS TAB ===
            if (ImGui::BeginTabItem("Skills")) {
                if (!GameState::IsGameReady()) {
                    ImGui::TextColored(cGray6, "Not in game.");
                } else {
                    UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
                    if (pPlayer && pPlayer->pInfo) {
                        // Cache skill data (refreshes each frame)
                        struct SkillDisplay {
                            int id; char name[64]; int base; int total;
                            int page, row, col; bool passive, aura;
                            char tree[32];
                        };
                        static SkillDisplay skills[120];
                        static int skillCount = 0;
                        static SkillDisplay leftSkill = {}, rightSkill = {};
                        static char className[32] = {};

                        {
                            skillCount = 0;
                            static const char* classNames[] = {"Amazon","Sorceress","Necromancer","Paladin","Barbarian","Druid","Assassin"};
                            int cls = pPlayer->dwTxtFileNo;
                            strncpy_s(className, (cls >= 0 && cls <= 6) ? classNames[cls] : "Unknown", sizeof(className));

                            // Read all skills
                            Skill* pSkill = pPlayer->pInfo->pFirstSkill;
                            while (pSkill && skillCount < 120) {
                                if (pSkill->pSkillInfo && pSkill->skillLevel > 0) {
                                    auto& s = skills[skillCount];
                                    s.id = (int)pSkill->pSkillInfo->wSkillId;
                                    s.base = pSkill->skillLevel;
                                    s.total = D2COMMON_GetSkillLevel(pPlayer, pSkill, TRUE);
                                    s.passive = (((BYTE*)pSkill->pSkillInfo)[4] & 0x10) != 0;
                                    s.aura = (((BYTE*)pSkill->pSkillInfo)[4] & 0x20) != 0;
                                    s.name[0] = '\0';
                                    s.page = 0; s.row = 0; s.col = 0;
                                    s.tree[0] = '\0';

                                    {
                                        int sd = pSkill->pSkillInfo->wSkillDesc;
                                        sgptDataTable* pDT = *p_D2COMMON_sgptDataTable;
                                        if (sd > 0 && pDT && pDT->pSkillDescTxt) {
                                            SkillDescTxt* pDesc = &pDT->pSkillDescTxt[sd];
                                            if (pDesc) {
                                                s.page = pDesc->bSkillPage;
                                                s.row = pDesc->bSkillRow;
                                                s.col = pDesc->bSkillColumn;
                                                // Set tree name based on page (Sorc-specific for now)
                                                static const char* pgNames[] = {"General", "Fire", "Lightning", "Cold"};
                                                strncpy_s(s.tree, (s.page >= 0 && s.page <= 3) ? pgNames[s.page] : "Unknown", sizeof(s.tree));
                                                if (pDesc->wStrName > 0) {
                                                    wchar_t* wN = GetTblEntryByIndex(pDesc->wStrName, TBLOFFSET_STRING);
                                                    if (wN) WideCharToMultiByte(CP_UTF8, 0, wN, -1, s.name, sizeof(s.name)-1, nullptr, nullptr);
                                                }
                                            }
                                        }
                                    }
                                    if (!s.name[0]) snprintf(s.name, sizeof(s.name), "Skill %d", s.id);
                                    skillCount++;
                                }
                                pSkill = pSkill->pNextSkill;
                            }

                            // Read assigned L/R skills
                            auto readAssigned = [&](Skill* sk, SkillDisplay& out) {
                                out = {};
                                if (sk && sk->pSkillInfo) {
                                    out.id = (int)sk->pSkillInfo->wSkillId;
                                    out.base = sk->skillLevel;
                                    out.total = D2COMMON_GetSkillLevel(pPlayer, sk, TRUE);
                                    {
                                        int sd = sk->pSkillInfo->wSkillDesc;
                                        sgptDataTable* pDT = *p_D2COMMON_sgptDataTable;
                                        if (sd > 0 && pDT && pDT->pSkillDescTxt) {
                                            SkillDescTxt* pDesc = &pDT->pSkillDescTxt[sd];
                                            if (pDesc && pDesc->wStrName > 0) {
                                                wchar_t* wN = GetTblEntryByIndex(pDesc->wStrName, TBLOFFSET_STRING);
                                                if (wN) WideCharToMultiByte(CP_UTF8, 0, wN, -1, out.name, sizeof(out.name)-1, nullptr, nullptr);
                                            }
                                        }
                                    }
                                    if (!out.name[0]) snprintf(out.name, sizeof(out.name), "Skill %d", out.id);
                                }
                            };
                            readAssigned(pPlayer->pInfo->pLeftSkill, leftSkill);
                            readAssigned(pPlayer->pInfo->pRightSkill, rightSkill);
                        }

                        // -- Assigned Skills Header --
                        ImGui::TextColored(cGold, "%s Skills", className);
                        ImGui::Separator();

                        ImGui::TextColored(ImVec4(0.5f,0.8f,1.0f,1), "Left Click:");
                        ImGui::SameLine();
                        if (leftSkill.id > 0) {
                            ImGui::Text("%s (ID:%d) Lv:%d/%d", leftSkill.name, leftSkill.id, leftSkill.base, leftSkill.total);
                        } else {
                            ImGui::TextColored(cGray6, "None");
                        }

                        ImGui::TextColored(ImVec4(1.0f,0.6f,0.5f,1), "Right Click:");
                        ImGui::SameLine();
                        if (rightSkill.id > 0) {
                            ImGui::Text("%s (ID:%d) Lv:%d/%d", rightSkill.name, rightSkill.id, rightSkill.base, rightSkill.total);
                        } else {
                            ImGui::TextColored(cGray6, "None");
                        }

                        ImGui::Separator();

                        // -- Skills by Tree (separate table per tree) --
                        static const char* treeLabels[] = {"Fire", "Lightning", "Cold"};
                        ImVec4 treeCols[] = {cRed, cYellow, cBlue};

                        for (int pg = 1; pg <= 3; pg++) {
                            // Count skills in this tree
                            int treeCount = 0;
                            int treePoints = 0;
                            for (int i = 0; i < skillCount; i++) {
                                if (skills[i].page == pg) { treeCount++; treePoints += skills[i].base; }
                            }
                            if (treeCount == 0) continue;

                            ImGui::TextColored(treeCols[pg-1], "%s (%d points)", treeLabels[pg-1], treePoints);

                            char tableId[32];
                            snprintf(tableId, sizeof(tableId), "tree_%d", pg);
                            if (ImGui::BeginTable(tableId, 5,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {

                                ImGui::TableSetupColumn("", 0, 0.2f);
                                ImGui::TableSetupColumn("Skill", 0, 2.5f);
                                ImGui::TableSetupColumn("Base", 0, 0.5f);
                                ImGui::TableSetupColumn("Total", 0, 0.8f);
                                ImGui::TableSetupColumn("Type", 0, 0.7f);
                                ImGui::TableHeadersRow();

                                // Sort by row position within tree
                                for (int row = 0; row <= 6; row++) {
                                    for (int col = 0; col <= 3; col++) {
                                        for (int i = 0; i < skillCount; i++) {
                                            auto& s = skills[i];
                                            if (s.page != pg || s.row != row || s.col != col) continue;

                                            bool isL = (s.id == leftSkill.id && leftSkill.id > 0);
                                            bool isR = (s.id == rightSkill.id && rightSkill.id > 0);

                                            ImGui::TableNextRow();

                                            // Assigned
                                            ImGui::TableNextColumn();
                                            if (isL && isR) ImGui::TextColored(ImVec4(0.8f,0.5f,1,1), "LR");
                                            else if (isL) ImGui::TextColored(ImVec4(0.5f,0.8f,1,1), "L");
                                            else if (isR) ImGui::TextColored(ImVec4(1,0.6f,0.5f,1), "R");

                                            // Name
                                            ImGui::TableNextColumn();
                                            ImVec4 nameCol = s.base > 0 ? cGold : cGray6;
                                            if (isL || isR) nameCol = cWhite;
                                            ImGui::TextColored(nameCol, "%s", s.name);

                                            // Base
                                            ImGui::TableNextColumn();
                                            if (s.base > 0) ImGui::Text("%d", s.base);
                                            else ImGui::TextColored(cGray6, "-");

                                            // Total
                                            ImGui::TableNextColumn();
                                            if (s.total > s.base) {
                                                ImGui::Text("%d", s.total);
                                                ImGui::SameLine();
                                                ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "(+%d)", s.total - s.base);
                                            } else if (s.base > 0) {
                                                ImGui::Text("%d", s.total);
                                            } else {
                                                ImGui::TextColored(cGray6, "-");
                                            }

                                            // Type
                                            ImGui::TableNextColumn();
                                            if (s.passive) ImGui::TextColored(ImVec4(0.6f,0.9f,0.6f,1), "Passive");
                                            else if (s.aura) ImGui::TextColored(ImVec4(0.9f,0.9f,0.4f,1), "Aura");
                                            else ImGui::Text("Active");
                                        }
                                    }
                                }
                                ImGui::EndTable();
                            }
                            ImGui::Spacing();
                        }
                    } else {
                        ImGui::TextColored(cGray6, "No player/skill data.");
                    }
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    // Find the monitor where the game window lives
    // and compute a position below the game window on that monitor
    void ComputePanelPosition(int& outX, int& outY, int& outW, int& outH) {
        // Check if we have a saved position from config
        bool hasSavedPos = (App.debugPanel.posX.value != -1 && App.debugPanel.posY.value != -1);

        if (hasSavedPos) {
            outX = App.debugPanel.posX.value;
            outY = App.debugPanel.posY.value;
            outW = App.debugPanel.width.value;
            outH = App.debugPanel.height.value;

            // Get DPI for the saved position
            POINT pt = { outX, outY };
            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            g_dpiAutoScale = GetDpiScaleForMonitor(hMon);
            g_dpiScale = g_dpiAutoScale;

            // Restore DPI preset if saved
            g_dpiPresetIndex = App.debugPanel.dpiPreset.value;
            if (g_dpiPresetIndex > 0 && g_dpiPresetIndex < DPI_PRESET_COUNT) {
                g_dpiScale = DPI_PRESETS[g_dpiPresetIndex];
            }
            return;
        }

        // Auto-position: find game window and position below it
        HWND gameHwnd = nullptr;
        for (int i = 0; i < 30 && !gameHwnd; ++i) {
            gameHwnd = FindWindow(nullptr, "Diablo II");
            if (!gameHwnd) Sleep(200);
        }

        if (gameHwnd) {
            HMONITOR hMon = MonitorFromWindow(gameHwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            GetMonitorInfo(hMon, &mi);

            RECT gameRect;
            GetWindowRect(gameHwnd, &gameRect);

            g_dpiAutoScale = GetDpiScaleForMonitor(hMon);
            g_dpiScale = g_dpiAutoScale;

            outW = (int)(BASE_WINDOW_W * g_dpiScale);
            outH = (int)(BASE_WINDOW_H * g_dpiScale);

            outX = gameRect.left;
            outY = gameRect.bottom + 5;

            int monitorBottom = mi.rcWork.bottom;
            if (outY + outH > monitorBottom) {
                outY = monitorBottom - outH;
                if (outY < mi.rcWork.top) {
                    outY = mi.rcWork.top;
                    outH = monitorBottom - outY;
                }
            }

            int monitorRight = mi.rcWork.right;
            if (outX + outW > monitorRight) {
                outW = monitorRight - outX;
            }
        } else {
            HDC hdc = GetDC(nullptr);
            g_dpiAutoScale = (float)GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
            g_dpiScale = g_dpiAutoScale;
            ReleaseDC(nullptr, hdc);

            outW = (int)(BASE_WINDOW_W * g_dpiScale);
            outH = (int)(BASE_WINDOW_H * g_dpiScale);
            outX = 100;
            outY = 100;
        }
    }

    // Main thread function for the debug panel
    void PanelThread() {
        // Enable per-monitor DPI awareness for this thread
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

        // Register window class
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = WINDOW_CLASS;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassEx(&wc);

        // Compute position below the game window
        int posX, posY, winW, winH;
        ComputePanelPosition(posX, posY, winW, winH);

        g_hwnd = CreateWindowEx(
            0,
            WINDOW_CLASS, WINDOW_TITLE,
            WS_OVERLAPPEDWINDOW,
            posX, posY, winW, winH,
            nullptr, nullptr, wc.hInstance, nullptr);

        if (!g_hwnd || !CreateDeviceD3D(g_hwnd)) {
            CleanupDeviceD3D();
            UnregisterClass(WINDOW_CLASS, wc.hInstance);
            return;
        }

        ShowWindow(g_hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_hwnd);

        // Setup ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX9_Init(g_pd3dDevice);

        // Build fonts at current DPI
        RebuildFonts();

        g_running = true;

        // Main loop
        const ImVec4 clearColor(0.06f, 0.06f, 0.10f, 1.00f);
        MSG msg;
        while (!g_shutdownRequested) {
            while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    g_shutdownRequested = true;
                }
            }
            if (g_shutdownRequested) break;

            // Rebuild fonts if DPI changed (from WM_DPICHANGED)
            if (g_fontRebuildNeeded) {
                RebuildFonts();
                g_fontRebuildNeeded = false;
            }

            // Start ImGui frame
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            DrawDebugUI();

            // Render
            ImGui::EndFrame();
            g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
            g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            D3DCOLOR bg = D3DCOLOR_RGBA(
                (int)(clearColor.x * 255), (int)(clearColor.y * 255),
                (int)(clearColor.z * 255), (int)(clearColor.w * 255));
            g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                bg, 1.0f, 0);

            if (g_pd3dDevice->BeginScene() >= 0) {
                ImGui::Render();
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                g_pd3dDevice->EndScene();
            }

            HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
            if (result == D3DERR_DEVICELOST &&
                g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) {
                ResetDevice();
            }
        }

        // Cleanup
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        CleanupDeviceD3D();
        DestroyWindow(g_hwnd);
        UnregisterClass(WINDOW_CLASS, wc.hInstance);
        g_hwnd = nullptr;
        g_running = false;
    }
}

namespace DebugPanel {
    void Init() {
        if (g_running) return;
        g_shutdownRequested = false;
        g_thread = std::thread(PanelThread);
        g_thread.detach();
    }

    void Shutdown() {
        if (!g_running) return;
        g_shutdownRequested = true;
        if (g_hwnd) {
            PostMessage(g_hwnd, WM_CLOSE, 0, 0);
        }
        for (int i = 0; i < 50 && g_running; ++i) {
            Sleep(100);
        }
    }

    bool IsRunning() {
        return g_running;
    }
}
