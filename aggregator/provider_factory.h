#pragma once

// BuildProvider: the one place that turns a (venue, market) plus config into
// a fully-constructed Provider - resolving endpoints, depth tier
// (ResolveDepth), redundant-connection count and per-venue staleness
// backstops. Both md_provider_app and the tests build providers through this
// so construction cannot drift between call sites.

#include <cstdint>
#include <memory>

#include "config/config.h"
#include "config/venues_config.h"
#include "md_provider/md_provider.h"
#include "types/venue.h"

namespace market_data {

// Resolves the requested depth to a tier this venue actually publishes, and
// logs when it cannot reach what was asked for - OKX tops out at 400 without
// VIP4, so a request above that is silently under-delivered unless it is
// reported. Exposed (not file-local) because both BuildProvider and any caller
// that wants to log the resolved tier need it.
uint32_t ResolveDepth(VenueId venue, uint32_t desired);

// Builds one fully-configured Provider for `venue` on the market carried by
// `instrument`: WS/REST endpoints from `endpoints`, depth and redundant
// connection count from `server_config`, and the per-venue staleness backstops
// (types/venue.h). One place that turns a (venue, market) into a constructed
// Provider, so md_provider_app - and, before it, the single-process build -
// build providers the same way.
//
// `on_update` / `on_quote` are bound straight into the Provider, exactly as the
// direct construction did. Returns nullptr for VenueId::COUNT.
std::unique_ptr<Provider> BuildProvider(VenueId venue, InstrumentKey instrument, const VenueEndpoints& endpoints,
                                        const ServerConfig& server_config, Provider::CallBack on_update,
                                        Provider::QuoteCallBack on_quote);

}  // namespace market_data
