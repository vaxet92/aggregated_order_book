#include <gtest/gtest.h>

#include "aggregator/wire_translation.h"

using namespace market_data;

namespace {

// The slot -> wire mapping the aggregator builds once per publish from
// Core::VenueName. Spelled out here rather than reusing
// production wiring, so a test asserts against a layout it chose: OKX in slot
// 0, BINANCE in slot 1 - deliberately NOT enum order, because the whole point
// is that the two are independent.
constexpr VenueSlot kOkxSlot = static_cast<VenueSlot>(0);
constexpr VenueSlot kBinanceSlot = static_cast<VenueSlot>(1);

VenueWireTable OutOfOrderTable() {
    return MakeVenueWireTable([](VenueSlot slot) -> std::string_view {
        if (slot == kOkxSlot) return "OKX";
        if (slot == kBinanceSlot) return "BINANCE";
        return {};
    });
}

}  // namespace

TEST(WireTranslationTest, VenueWireTableResolvesSlotsNotEnumValues) {
    const VenueWireTable table = OutOfOrderTable();

    // Slot 0 is OKX here, even though VenueId::OKX is 2. Indexing this table
    // by VenueId would publish OKX's levels as BINANCE.
    EXPECT_EQ(table[VenueSlotIndex(kOkxSlot)], wire::OKX);
    EXPECT_EQ(table[VenueSlotIndex(kBinanceSlot)], wire::BINANCE);
}

TEST(WireTranslationTest, UnregisteredSlotsAreUnspecified) {
    const VenueWireTable table = OutOfOrderTable();

    // Nothing registered above slot 1. Unattributed is the honest answer;
    // defaulting to a real venue would name the wrong exchange.
    for (size_t i = 2; i < kMaxVenues; ++i) {
        EXPECT_EQ(table[i], wire::VENUE_UNSPECIFIED) << "slot " << i;
    }
}

TEST(WireTranslationTest, VenueIdMapsToCorrectWireVenue) {
    EXPECT_EQ(ToWire(VenueId::BINANCE), wire::BINANCE);
    EXPECT_EQ(ToWire(VenueId::BYBIT), wire::BYBIT);
    EXPECT_EQ(ToWire(VenueId::OKX), wire::OKX);
}

TEST(WireTranslationTest, CountVenueMapsToUnspecified) {
    // Not a real venue - should never actually be passed in practice, but
    // must not silently pick a real venue if it ever is.
    EXPECT_EQ(ToWire(VenueId::COUNT), wire::VENUE_UNSPECIFIED);
}

TEST(WireTranslationTest, PriceLevelFieldsCarryOverExactly) {
    consolidated::ConsolidatedPriceLevel level;
    level.price = 7831010000000ull;
    level.total_qty = 9531387ull;
    level.venues.push_back({kBinanceSlot, 9531387ull});

    auto wire_level = ToWire(level, OutOfOrderTable());

    EXPECT_EQ(wire_level.price(), 7831010000000LL);
    EXPECT_EQ(wire_level.total_qty(), 9531387LL);
    ASSERT_EQ(wire_level.venues_size(), 1);
    EXPECT_EQ(wire_level.venues(0).venue(), wire::BINANCE);
    EXPECT_EQ(wire_level.venues(0).qty(), 9531387LL);
}

TEST(WireTranslationTest, BboCarriesCrossedFlagAndBothSides) {
    consolidated::BBO bbo;
    bbo.best_bid.price = 100;
    bbo.best_bid.total_qty = 5;
    bbo.best_ask.price = 99;
    bbo.best_ask.total_qty = 3;
    bbo.crossed = true;

    auto wire_bbo = ToWire(bbo, OutOfOrderTable());

    EXPECT_EQ(wire_bbo.best_bid().price(), 100LL);
    EXPECT_EQ(wire_bbo.best_ask().price(), 99LL);
    EXPECT_TRUE(wire_bbo.crossed());
}

// --------------------------------------------------------------- market ---
//
// Spot and futures are separate subscriptions, so the market is half of the
// key that selects a book. These tests pin the ASYMMETRY between the two
// directions: domain -> wire always succeeds, wire -> domain can refuse.

TEST(WireTranslationTest, MarketRoundTripsInBothDirections) {
    EXPECT_EQ(ToWire(MarketType::kSpot), wire::SPOT);
    EXPECT_EQ(ToWire(MarketType::kFutures), wire::FUTURES);

    EXPECT_EQ(FromWire(wire::SPOT), MarketType::kSpot);
    EXPECT_EQ(FromWire(wire::FUTURES), MarketType::kFutures);
}

// The contract test for the whole design decision. A proto3 enum field has no
// presence, so a client that never set `market` sends MARKET_UNSPECIFIED -
// byte-for-byte identical to a client that deliberately sent zero. If this
// ever returned kSpot, that client would be silently subscribed to the spot
// book and would have no way to discover it.
TEST(WireTranslationTest, UnspecifiedMarketIsRejectedNotDefaultedToSpot) {
    EXPECT_EQ(FromWire(wire::MARKET_UNSPECIFIED), std::nullopt);
}

// proto3 enums are OPEN: a value outside the generated enumerators is legal on
// the wire and arrives intact, which is how a NEWER client talking to an OLDER
// server shows up. It names no book this build holds, so it is refused exactly
// like UNSPECIFIED rather than being coerced into one of the known values.
TEST(WireTranslationTest, UnknownMarketValueFromANewerClientIsRejected) {
    EXPECT_EQ(FromWire(static_cast<wire::MarketType>(99)), std::nullopt);
}
