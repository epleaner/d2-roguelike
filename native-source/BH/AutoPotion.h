#pragma once

// AutoPotion - Automatic potion drinking when HP/MP drops below threshold.
// Configurable via BH.json and MCP tools.

namespace AutoPotion {
    struct Config {
        bool enabled = false;
        int hpThreshold = 50;       // drink HP potion at this % of max HP
        int mpThreshold = 30;       // drink MP potion at this % of max mana
        int rejuvThreshold = 25;    // drink rejuv at this % (overrides HP/MP)
        int cooldownMs = 500;       // minimum ms between potion uses
        bool skipInTown = true;     // don't auto-drink in town
    };

    // Call from game loop (GameLoop / D2Handlers)
    void Update();

    // Get/set config (thread-safe)
    Config GetConfig();
    void SetConfig(const Config& cfg);
}
