#pragma once

// Translation between md_core's domain types and the client-facing gRPC
// protobuf types (a different boundary from md_wire's internal provider<->core
// wire): ToWire/FromWire for VenueId/MarketType, and VenueWireTable - the
// slot -> wire::Venue lookup built once per publish, because md_core
// attributes levels by slot and knows no venue names.

#include <array>
#include <functional>
#include <optional>
#include <string_view>

#include "md_core/consolidated_bbo.h"
#include "md_core/consolidated_book.h"
#include "types/venue_registry.h"
#include "aggregator.pb.h"

namespace market_data {

// VenueId (global scope, types/venue.h) has no UNSPECIFIED value; wire::Venue
// does (proto3 enums must have a zero value). VENUE_UNSPECIFIED should never
// actually be produced here - every VenueId case is handled explicitly.
wire::Venue ToWire(VenueId venue);

// Domain MarketType (types/venue.h) has no UNSPECIFIED value; wire::MarketType
// does, for the same proto3 reason as Venue. Every domain value maps to a real
// wire value, so this direction cannot fail.
wire::MarketType ToWire(MarketType market);

// The wire -> domain direction CAN fail, and that is exactly the point.
// MARKET_UNSPECIFIED means the client named no market; an unknown positive
// value means a newer client is talking to an older server. Neither of them
// names a book we hold.
//
// nullopt rather than a defaulted MarketType - the same choice
// ToMarketType(string_view) already makes for config. Spot and futures are
// separate subscriptions, so quietly defaulting a confused client to spot
// would hand it the wrong book with no way to notice.
std::optional<MarketType> FromWire(wire::MarketType market);

// Slot -> wire venue, resolved ONCE per published message rather than per
// level.
//
// md_core attributes levels by SLOT and knows no venue names; the wire
// carries a venue enum. Something has to bridge the two, and the cheap place
// is here: a merged book has up to 1000 levels with several contributors each,
// which is thousands of lookups to produce at most kMaxVenues distinct
// answers. Build the table once from Core::VenueName, then index it.
//
// KNOWN LIMIT: wire::Venue is itself a fixed proto enum (BINANCE/BYBIT/OKX),
// so a fourth venue would resolve to VENUE_UNSPECIFIED here even though
// md_core handles it correctly. Removing that needs a proto change - carrying
// a slot plus a per-message slot->name dictionary (the B3) - which changes
// the published contract and every client, and is NOT done.
using VenueWireTable = std::array<wire::Venue, kMaxVenues>;

// Builds the table from a slot -> name resolver, typically Core::VenueName.
// An unregistered slot maps to VENUE_UNSPECIFIED.
VenueWireTable MakeVenueWireTable(const std::function<std::string_view(VenueSlot)>& venue_name);

wire::ConsolidatedPriceLevel ToWire(const consolidated::ConsolidatedPriceLevel& level, const VenueWireTable& venues);

wire::Bbo ToWire(const consolidated::BBO& bbo, const VenueWireTable& venues);

// The band results don't carry their own threshold - the caller holds the
// request's threshold list and zips it positionally with the results, which
// FillToNotionalBands/FillToBpsBands guarantee are in the same order as the
// targets passed in.
wire::VolumeBandResult ToWire(const consolidated::NotionalFill& fill, uint64_t notional_threshold);

wire::PriceBandResult ToWire(const consolidated::BpsFill& fill, uint32_t bps_threshold);

}  // namespace market_data
