#pragma once

// AutoPickup - Smart belt refill + scroll pickup.
// On enable, snapshots the current belt layout (item type per column).
// Picks up potions to maintain that layout, preferring same-or-better tier.
// Falls back one tier when on the last potion of that category in belt.

namespace AutoPickup {
    struct Config {
        bool enabled = false;
        int maxDistance = 5;        // max pickup range (game units)
        int cooldownMs = 250;       // minimum ms between pickups
        bool pickTpScrolls = true;  // pick up TP scrolls when tome not full
        bool pickIdScrolls = true;  // pick up ID scrolls when tome not full
    };

    // Belt column snapshot — what type each column prefers
    struct BeltSnapshot {
        bool valid = false;
        int preferredCode[4] = {};  // TxtFileNo for each column (0 = empty/no preference)
    };

    // Call from game loop
    void Update();

    // Get/set config (thread-safe)
    Config GetConfig();
    void SetConfig(const Config& cfg);

    // Get/set belt snapshot
    BeltSnapshot GetSnapshot();
    void SetSnapshot(const BeltSnapshot& snap);

    // Force re-snapshot of current belt layout
    void ResnapBelt();
}
