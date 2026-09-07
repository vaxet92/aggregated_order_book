#include "wire_translation.h"

namespace market_data {

wire::Venue ToWire(VenueId venue) {
    switch (venue) {
        case VenueId::BINANCE:
            return wire::BINANCE;
        case VenueId::BYBIT:
            return wire::BYBIT;
        case VenueId::OKX:
            return wire::OKX;
        case VenueId::COUNT:
            break;  // not a real venue - falls through to VENUE_UNSPECIFIED
    }
    return wire::VENUE_UNSPECIFIED;
}

wire::MarketType ToWire(MarketType market) {
    switch (market) {
        case MarketType::kSpot:
            return wire::SPOT;
        case MarketType::kFutures:
            return wire::FUTURES;
    }
    return wire::MARKET_UNSPECIFIED;  // unreachable - the enum has no other value
}

std::optional<MarketType> FromWire(wire::MarketType market) {
    switch (market) {
        case wire::SPOT:
            return MarketType::kSpot;
        case wire::FUTURES:
            return MarketType::kFutures;
        default:
            // MARKET_UNSPECIFIED, and anything a newer client might send that
            // this build does not know. Both are rejected by the caller.
            //
            // A `default` rather than an exhaustive list: proto3 enums are
            // OPEN, so an unknown value really can arrive over the wire and
            // must not be a compile-time-only concern.
            return std::nullopt;
    }
}

VenueWireTable MakeVenueWireTable(const std::function<std::string_view(VenueSlot)>& venue_name) {
    VenueWireTable table{};  // VENUE_UNSPECIFIED everywhere by default
    for (size_t i = 0; i < kMaxVenues; ++i) {
        const std::string_view name = venue_name(static_cast<VenueSlot>(i));
        if (name.empty()) {
            continue;  // nothing registered in this slot
        }
        // ToVenueId returns COUNT for a name the enum does not know, and
        // ToWire maps that to VENUE_UNSPECIFIED - which is the honest answer
        // until the proto can carry a name (see the header's KNOWN LIMIT).
        table[i] = ToWire(VenueConverter::ToVenueId(std::string(name)));
    }
    return table;
}

wire::ConsolidatedPriceLevel ToWire(const consolidated::ConsolidatedPriceLevel& level, const VenueWireTable& venues) {
    wire::ConsolidatedPriceLevel wire_level;
    // No casts: PriceTicks/QtyUnits are uint64_t and the proto fields are
    // uint64 too, so these are exact, same-width assignments.
    wire_level.set_price(level.price);
    wire_level.set_total_qty(level.total_qty);
    for (const auto& venue_quote : level.venues) {
        wire::VenueQuote* wire_quote = wire_level.add_venues();
        wire_quote->set_venue(venues[VenueSlotIndex(venue_quote.slot)]);
        wire_quote->set_qty(venue_quote.qty);
    }
    return wire_level;
}

wire::Bbo ToWire(const consolidated::BBO& bbo, const VenueWireTable& venues) {
    wire::Bbo wire_bbo;
    *wire_bbo.mutable_best_bid() = ToWire(bbo.best_bid, venues);
    *wire_bbo.mutable_best_ask() = ToWire(bbo.best_ask, venues);
    wire_bbo.set_crossed(bbo.crossed);
    return wire_bbo;
}

wire::VolumeBandResult ToWire(const consolidated::NotionalFill& fill, uint64_t notional_threshold) {
    wire::VolumeBandResult result;
    result.set_notional_threshold(notional_threshold);
    result.set_vwap(fill.vwap);
    result.set_worst_price(fill.worst_price);
    result.set_filled_notional(fill.filled_notional);
    result.set_filled_qty(fill.filled_qty);
    result.set_insufficient_depth(fill.insufficient_depth);
    result.set_level_count(fill.level_count);
    return result;
}

wire::PriceBandResult ToWire(const consolidated::BpsFill& fill, uint32_t bps_threshold) {
    wire::PriceBandResult result;
    result.set_bps_threshold(bps_threshold);
    result.set_vwap(fill.vwap);
    result.set_limit_price(fill.limit_price);
    result.set_cum_qty(fill.cum_qty);
    result.set_cum_notional(fill.cum_notional);
    result.set_level_count(fill.level_count);
    result.set_insufficient_depth(fill.insufficient_depth);
    return result;
}

}  // namespace market_data
