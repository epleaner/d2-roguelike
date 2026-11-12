#pragma once

namespace RoguelikeAura {
struct Config {
    char itemCode[4];
    int skillId;
    int serverFunction;
    int skillLevel;
    int durationSeconds;
};

inline constexpr Config kElixirsOfAuras[] = {
    {{'r','a','1',' '}, 98, 65, 30, 30},
    {{'r','p','r',' '}, 99, 65, 30, 30},
    {{'r','a','2',' '}, 100, 65, 30, 30},
    {{'r','a','3',' '}, 102, 66, 30, 30},
    {{'r','a','4',' '}, 103, 65, 30, 30},
    {{'r','a','5',' '}, 104, 65, 30, 30},
    {{'r','a','6',' '}, 105, 65, 30, 30},
    {{'r','a','7',' '}, 108, 65, 30, 30},
    {{'r','a','8',' '}, 109, 65, 30, 30},
    {{'r','a','9',' '}, 110, 65, 30, 30},
    {{'r','b','1',' '}, 113, 65, 30, 30},
    {{'r','b','2',' '}, 114, 81, 30, 30},
    {{'r','b','3',' '}, 115, 65, 30, 30},
    {{'r','b','4',' '}, 118, 66, 30, 30},
    {{'r','b','5',' '}, 119, 66, 30, 30},
    {{'r','b','6',' '}, 120, 65, 30, 30},
    {{'r','b','7',' '}, 122, 65, 30, 30},
    {{'r','b','8',' '}, 123, 66, 30, 30},
    {{'r','b','9',' '}, 124, 82, 30, 30},
    {{'r','c','1',' '}, 125, 65, 30, 30},
};
constexpr bool ValidCatalog() {
    constexpr int count = sizeof(kElixirsOfAuras) / sizeof(kElixirsOfAuras[0]);
    for (int index = 0; index < count; ++index) {
        const Config& aura = kElixirsOfAuras[index];
        if (aura.skillId <= 0 || aura.skillLevel != 30 ||
            aura.durationSeconds != 30 || aura.itemCode[3] != ' ' ||
            (aura.serverFunction != 65 && aura.serverFunction != 66 &&
             aura.serverFunction != 81 && aura.serverFunction != 82))
            return false;
        for (int previous = 0; previous < index; ++previous) {
            const Config& other = kElixirsOfAuras[previous];
            if (aura.skillId == other.skillId ||
                (aura.itemCode[0] == other.itemCode[0] &&
                 aura.itemCode[1] == other.itemCode[1] &&
                 aura.itemCode[2] == other.itemCode[2]))
                return false;
        }
    }
    return count == 20;
}
static_assert(ValidCatalog(), "Aura elixirs must be unique level-30, 30-second configurations");


inline const Config* FindByCode(const char code[4]) {
    if (!code || (code[3] != ' ' && code[3] != '\0')) return nullptr;
    for (const Config& aura : kElixirsOfAuras)
        if (aura.itemCode[0] == code[0] && aura.itemCode[1] == code[1] &&
            aura.itemCode[2] == code[2])
            return &aura;
    return nullptr;
}

inline const Config* FindBySkillId(int skillId) {
    for (const Config& aura : kElixirsOfAuras)
        if (aura.skillId == skillId) return &aura;
    return nullptr;
}

constexpr bool SupportedServerFunction(int function) noexcept {
    return function == 65 || function == 66 || function == 81 || function == 82;
}

// Confirm the native aura state, not the handler's incidental return value.
// Prayer also verifies its custom passive; Redemption must establish its aura state.
constexpr bool ActivationEstablished(int serverFunction, int skillId,
    bool statePresent, bool prayerStatVerified) noexcept {
    if (!SupportedServerFunction(serverFunction)) return false;
    if (skillId == 99) return statePresent && prayerStatVerified;
    return statePresent;
}
} // namespace RoguelikeAura
