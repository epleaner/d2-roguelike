#include "GameNav.h"
#include "GameState.h"
#include "D2Ptrs.h"
#include "D2Helpers.h"

#include <mutex>

// Control type constants
#define CTRL_TYPE_EDITBOX  1
#define CTRL_TYPE_IMAGE    2
#define CTRL_TYPE_BUTTON   6
#define CTRL_TYPE_LIST     7

namespace {
    std::mutex g_mutex;
    GameNav::NavStatus g_status;
    std::string g_targetChar;
    int g_targetDifficulty = -1;
    DWORD g_lastStepTime = 0;
    DWORD g_stepDelay = 500; // ms between steps
    int g_retryCount = 0;
    bool g_exitRequested = false;
    bool g_quitRequested = false;
    DWORD g_quitStartTime = 0;

    // Count controls by type
    struct ControlCounts {
        int total = 0;
        int buttons = 0;
        int editboxes = 0;
        int lists = 0;
        int images = 0;
    };

    ControlCounts CountControls() {
        ControlCounts cc;
        Control* pCtrl = *p_D2WIN_FirstControl;
        while (pCtrl) {
            cc.total++;
            switch (pCtrl->dwType) {
                case CTRL_TYPE_BUTTON: cc.buttons++; break;
                case CTRL_TYPE_EDITBOX: cc.editboxes++; break;
                case CTRL_TYPE_LIST: cc.lists++; break;
                case CTRL_TYPE_IMAGE: cc.images++; break;
            }
            pCtrl = pCtrl->pNext;
        }
        return cc;
    }

    // Find a button control by approximate position
    Control* FindButton(int minX, int maxX, int minY, int maxY) {
        Control* pCtrl = *p_D2WIN_FirstControl;
        while (pCtrl) {
            if (pCtrl->dwType == CTRL_TYPE_BUTTON && pCtrl->dwState >= 2) {
                int x = (int)pCtrl->dwPosX;
                int y = (int)pCtrl->dwPosY;
                if (x >= minX && x <= maxX && y >= minY && y <= maxY) {
                    return pCtrl;
                }
            }
            pCtrl = pCtrl->pNext;
        }
        return nullptr;
    }

    // Click a control via its OnPress callback
    bool ClickControl(Control* pCtrl) {
        if (!pCtrl || !pCtrl->OnPress) return false;
        // Simulate a click by calling OnPress
        pCtrl->OnPress(pCtrl);
        return true;
    }

    // Identify current OOG screen by control patterns
    // Based on d2_auto.py heuristics (800x600 coordinates)
    enum OogScreen {
        OOG_UNKNOWN,
        OOG_MAIN_MENU,
        OOG_CHAR_SELECT,
        OOG_DIFFICULTY,
        OOG_DIALOG,       // OK/Cancel popup (e.g., "play single player?" warning)
        OOG_IN_GAME,
        OOG_LOADING,
        OOG_OTHER
    };

    OogScreen IdentifyScreen() {
        // If player unit exists, we're in-game
        UnitAny* pPlayer = D2CLIENT_GetPlayerUnit();
        if (pPlayer && pPlayer->pPath) {
            return OOG_IN_GAME;
        }

        Control* pCtrl = *p_D2WIN_FirstControl;
        if (!pCtrl) return OOG_LOADING;

        // Scan all controls once for pattern matching
        bool hasOKButton = false;       // bottom-right button (627, 572)
        int type4Count = 0;             // character slot type
        bool hasDiffButtons = false;    // difficulty buttons at x~264, y~297-383
        int diffButtonCount = 0;
        int buttonCount = 0;

        Control* p = pCtrl;
        while (p) {
            if (p->dwType == CTRL_TYPE_BUTTON) {
                buttonCount++;
                if (p->dwPosX > 600 && p->dwPosY > 550) hasOKButton = true;
                // Difficulty buttons: x around 264, y between 280-400, width ~272
                if (p->dwPosX >= 250 && p->dwPosX <= 280 &&
                    p->dwPosY >= 280 && p->dwPosY <= 400 &&
                    p->dwSizeX >= 200 && p->dwSizeX <= 300) {
                    diffButtonCount++;
                }
            }
            if (p->dwType == 4) type4Count++;
            p = p->pNext;
        }
        hasDiffButtons = (diffButtonCount >= 2);

        // Priority 1: Difficulty select (overlay on char select)
        // Identified by 2-3 difficulty buttons at x~264, y~297-383
        if (hasDiffButtons) {
            return OOG_DIFFICULTY;
        }

        // Priority 2: Character Select — has OK button + character slots
        if (hasOKButton && type4Count >= 4) {
            return OOG_CHAR_SELECT;
        }

        // Priority 3: Main Menu — many buttons (5+), few type4 controls (0-1)
        if (buttonCount >= 5 && type4Count <= 1) {
            return OOG_MAIN_MENU;
        }

        // Priority 4: Dialog popup — 2-3 buttons, few total controls
        // Dialogs like "Are you sure?" or error messages
        // Key signature: small number of buttons (1-3), no char slots (type4 < 8)
        if (buttonCount >= 1 && buttonCount <= 3 && type4Count < 8 && !hasDiffButtons) {
            return OOG_DIALOG;
        }

        // Fallback: few buttons, might be difficulty (pre-overlay)
        if (buttonCount >= 2 && buttonCount <= 4 && type4Count == 0) {
            return OOG_DIFFICULTY;
        }

        return OOG_UNKNOWN;
    }
}

namespace GameNav {

    void EnterGame(const std::string& charName, int difficulty) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_targetChar = charName;
        g_targetDifficulty = difficulty;
        g_status.state = NAV_IN_PROGRESS;
        g_status.step = 0;
        g_status.message = "Starting navigation...";
        g_lastStepTime = 0;
        g_retryCount = 0;
    }

    NavStatus GetStatus() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_status;
    }

    void Cancel() {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.state = NAV_IDLE;
        g_status.message = "Cancelled";
    }

    void Update() {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_status.state != NAV_IN_PROGRESS) return;

        DWORD now = GetTickCount();
        if (now - g_lastStepTime < g_stepDelay) return;
        g_lastStepTime = now;

        OogScreen screen = IdentifyScreen();

        switch (screen) {
            case OOG_IN_GAME:
                g_status.state = NAV_SUCCESS;
                g_status.currentScreen = "In Game";
                g_status.message = "Successfully entered game";
                return;

            case OOG_LOADING:
                g_status.currentScreen = "Loading";
                g_status.message = "Waiting for load...";
                return;

            case OOG_MAIN_MENU: {
                g_status.currentScreen = "Main Menu";
                g_status.message = "Clicking Single Player...";
                // Single Player button is typically around x=264, y=324 (800x600)
                Control* btn = FindButton(250, 380, 310, 340);
                if (btn) {
                    ClickControl(btn);
                    g_status.step = 1;
                } else {
                    g_status.message = "Can't find Single Player button";
                    g_retryCount++;
                    if (g_retryCount > 10) {
                        g_status.state = NAV_FAILED;
                        g_status.message = "Failed: couldn't find Single Player button";
                    }
                }
                return;
            }

            case OOG_CHAR_SELECT: {
                g_status.currentScreen = "Character Select";
                g_status.message = "Clicking OK...";
                // OK button at bottom-right: (627, 572) size 128x35
                Control* btn = FindButton(600, 760, 560, 610);
                if (btn) {
                    ClickControl(btn);
                    g_status.step = 2;
                } else {
                    g_status.message = "Can't find OK button";
                    g_retryCount++;
                    if (g_retryCount > 15) {
                        g_status.state = NAV_FAILED;
                    }
                }
                return;
            }

            case OOG_DIFFICULTY: {
                g_status.currentScreen = "Difficulty Select";
                int diff = g_targetDifficulty;

                // Difficulty buttons stacked vertically
                // Normal ~y=280, Nightmare ~y=330, Hell ~y=380
                Control* btn = nullptr;
                if (diff == 0) {
                    g_status.message = "Selecting Normal...";
                    btn = FindButton(250, 400, 265, 295);
                } else if (diff == 1) {
                    g_status.message = "Selecting Nightmare...";
                    btn = FindButton(250, 400, 315, 345);
                } else if (diff == 2) {
                    g_status.message = "Selecting Hell...";
                    btn = FindButton(250, 400, 365, 395);
                } else {
                    // Default: click the last (highest) enabled button
                    g_status.message = "Selecting highest difficulty...";
                    // Try Hell first, then Nightmare, then Normal
                    btn = FindButton(250, 400, 365, 395);
                    if (!btn) btn = FindButton(250, 400, 315, 345);
                    if (!btn) btn = FindButton(250, 400, 265, 295);
                }

                if (btn) {
                    ClickControl(btn);
                    g_status.step = 3;
                } else {
                    g_status.message = "Can't find difficulty button";
                    g_retryCount++;
                    if (g_retryCount > 10) {
                        g_status.state = NAV_FAILED;
                    }
                }
                return;
            }

            case OOG_DIALOG: {
                g_status.currentScreen = "Dialog/Gateway";
                // PD2 shows a "SELECT GATEWAY" screen with a list of gateways.
                // Strategy: first click the list entry (type 4 with OnPress) to select it,
                // then click the rightmost button (OK) to accept.
                // On even retries: click the list entry
                // On odd retries: click the rightmost button (OK)
                Control* listEntry = nullptr;
                Control* okBtn = nullptr;
                Control* p = *p_D2WIN_FirstControl;
                while (p) {
                    if (p->dwType == 4 && p->OnPress) {
                        listEntry = p; // the gateway list entry
                    }
                    if (p->dwType == CTRL_TYPE_BUTTON && p->OnPress) {
                        if (!okBtn || p->dwPosX > okBtn->dwPosX) {
                            okBtn = p; // rightmost = OK
                        }
                    }
                    p = p->pNext;
                }

                if (g_retryCount % 2 == 0 && listEntry) {
                    g_status.message = "Selecting gateway entry...";
                    ClickControl(listEntry);
                } else if (okBtn) {
                    g_status.message = "Clicking OK on gateway...";
                    ClickControl(okBtn);
                }

                g_retryCount++;
                if (g_retryCount > 30) {
                    g_status.state = NAV_FAILED;
                    g_status.message = "Failed: stuck on gateway";
                }
                return;
            }

            default:
                g_status.currentScreen = "Unknown";
                // After difficulty click (step >= 3), just wait for game to load
                if (g_status.step >= 3) {
                    g_status.message = "Waiting for game to load...";
                    g_retryCount++;
                    if (g_retryCount > 60) { // ~60 seconds
                        g_status.state = NAV_FAILED;
                        g_status.message = "Failed: game load timeout";
                    }
                } else {
                    g_status.message = "Unrecognized screen, trying dialog...";
                    // Try clicking any visible button to dismiss unknown screens
                    Control* anyBtn = *p_D2WIN_FirstControl;
                    while (anyBtn) {
                        if (anyBtn->dwType == CTRL_TYPE_BUTTON && anyBtn->OnPress) {
                            ClickControl(anyBtn);
                            break;
                        }
                        anyBtn = anyBtn->pNext;
                    }
                    g_retryCount++;
                    if (g_retryCount > 40) {
                        g_status.state = NAV_FAILED;
                        g_status.message = "Failed: stuck on unknown screen";
                    }
                }
                return;
        }
    }

    void RequestExitGame() {
        g_exitRequested = true;
    }

    void RequestQuitGame() {
        g_quitRequested = true;
        g_quitStartTime = GetTickCount();
        // If in-game, exit first to save
        if (IsGameReady()) {
            g_exitRequested = true;
        }
    }

    void CheckPendingExit() {
        if (g_exitRequested && !g_quitRequested) {
            g_exitRequested = false;
            if (IsGameReady()) {
                D2CLIENT_ExitGame();
            }
        }

        if (g_quitRequested) {
            DWORD elapsed = GetTickCount() - g_quitStartTime;

            // Step 1: exit game to save (first 500ms)
            if (g_exitRequested && elapsed < 500) {
                g_exitRequested = false;
                if (IsGameReady()) {
                    D2CLIENT_ExitGame();
                }
            }

            // Step 2: after enough time for save, terminate
            if (elapsed >= 3000) {
                g_quitRequested = false;
                TerminateProcess(GetCurrentProcess(), 0);
            }
        }
    }
}
