#pragma once

// ParseScaledDecimal: parses a venue's decimal-string price/quantity (e.g.
// "0.0024") directly into a scaled uint64_t, with no floating-point
// intermediate - what keeps venue-reported numbers exact all the way into
// PriceTicks/QtyUnits.

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace market_data {

// 10^n for n in [0, 8]. `scale` must stay in this range.
inline constexpr uint64_t kMultipliers[] = {1ULL,       10ULL,        100ULL,        1'000ULL,      10'000ULL,
                                            100'000ULL, 1'000'000ULL, 10'000'000ULL, 100'000'000ULL};

// Parses a decimal string like "0.0024" into a scaled uint64_t (default 1e8,
// matching PriceTicks/QtyUnits). No floating point: venue prices/quantities
// arrive as JSON strings so they can be parsed exactly; a double would
// reintroduce rounding error. Extra fractional digits
// beyond `scale` are truncated. `scale` must be 0..8.
template <uint64_t Scale = 8>
inline uint64_t ParseScaledDecimal(std::string_view sv) noexcept {
    static_assert(Scale <= 8, "Scale must be 0..8 (index into kMultipliers)");

    uint64_t value = 0;
    size_t dot_pos = 0;
    for (; dot_pos < sv.size(); ++dot_pos) {
        const char c = sv[dot_pos];
        if (c == '.') {
            break;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }

    if (dot_pos == sv.size()) {
        return value * kMultipliers[Scale];  // integer string, no fractional part
    }

    const size_t fractional_digits = sv.size() - dot_pos - 1;
    const size_t digits_to_parse = std::min<size_t>(fractional_digits, Scale);
    for (size_t i = 0; i < digits_to_parse; ++i) {
        value = value * 10 + static_cast<uint64_t>(sv[dot_pos + 1 + i] - '0');
    }
    value *= kMultipliers[Scale - digits_to_parse];
    return value;
}

}  // namespace market_data
