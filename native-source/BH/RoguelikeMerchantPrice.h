#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace RoguelikeMerchantPrice {
constexpr std::uint32_t SpecialQualityFloor(std::uint32_t itemQuality,
    std::uint32_t nativePrice, std::uint32_t rarePrice) noexcept {
    if (itemQuality != 5 && itemQuality != 7) return nativePrice;
    const std::uint64_t basePrice =
        (std::max)(nativePrice, rarePrice);
    const std::uint64_t premium = (basePrice + 9) / 10;
    const std::uint64_t quote = basePrice +
        (std::max<std::uint64_t>)(1, premium);
    return static_cast<std::uint32_t>(
        (std::min<std::uint64_t>)(quote,
            (std::numeric_limits<std::uint32_t>::max)()));
}
} // namespace RoguelikeMerchantPrice
