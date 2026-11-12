#pragma once

#include <string_view>

namespace RoguelikeBosses {
constexpr unsigned kUberStartDepth = 85;
constexpr unsigned kMapBossStartDepth = 80;

constexpr bool IsUberName(std::string_view name) noexcept {
    return name.find("Uber") != std::string_view::npos ||
        name.find("Lilith") != std::string_view::npos ||
        name.find("Diablo Clone") != std::string_view::npos;
}

constexpr bool IsUberClass(int classId) noexcept {
    // Pinned v17 S13 MonStats indices: Diablo Clone, Ubers, Uber Diablo, and
    // Uber Ancients. Their localized names are not consistently identifiable.
    return classId == 333 || classId == 789 ||
        (classId >= 704 && classId <= 709) ||
        (classId >= 989 && classId <= 991);
}

constexpr bool EligibleAtDepth(int nativeNormalLevel, unsigned zoneDepth,
    bool uber) noexcept {
    if (nativeNormalLevel < 1 || zoneDepth < 1 || zoneDepth > 99) return false;
    return uber ? zoneDepth >= kUberStartDepth :
        zoneDepth >= kMapBossStartDepth &&
        nativeNormalLevel <= static_cast<int>(zoneDepth);
}
constexpr bool EligibleCandidateAtDepth(int nativeNormalLevel, unsigned zoneDepth,
    int classId, std::string_view name) noexcept {
    const bool uber = IsUberClass(classId) || IsUberName(name);
    return EligibleAtDepth(nativeNormalLevel, zoneDepth, uber);
}
} // namespace RoguelikeBosses
