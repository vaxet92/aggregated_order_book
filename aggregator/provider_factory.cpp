#include "provider_factory.h"

#include "logger/logger.h"
#include "md_provider/binance/binance_provider.h"
#include "md_provider/bybit/bybit_provider.h"
#include "md_provider/okx/okx_provider.h"

namespace market_data {

uint32_t ResolveDepth(VenueId venue, uint32_t desired) {
    uint32_t tier = 0;
    switch (venue) {
        case VenueId::BINANCE:
            tier = SelectDepthTier(kBinanceDepthTiers, desired);
            break;
        case VenueId::BYBIT:
            tier = SelectDepthTier(kBybitDepthTiers, desired);
            break;
        case VenueId::OKX:
            tier = SelectDepthTier(kOkxDepthTiers, desired);
            break;
        case VenueId::COUNT:
            tier = desired;
            break;
    }

    if (tier < desired) {
        Logger::Log(LogLevel::kWarning, "[{}] requested depth {} exceeds this venue's deepest tier - using {}",
                    VenueConverter::ToVenueString(venue), desired, tier);
    } else if (tier > desired) {
        Logger::Log(LogLevel::kInfo, "[{}] depth {} rounded up to venue tier {}", VenueConverter::ToVenueString(venue),
                    desired, tier);
    }
    return tier;
}

std::unique_ptr<Provider> BuildProvider(VenueId venue, InstrumentKey instrument, const VenueEndpoints& endpoints,
                                        const ServerConfig& server_config, Provider::CallBack on_update,
                                        Provider::QuoteCallBack on_quote) {
    ProviderConfig config = {
        .venue_id = venue,
        .instrument = instrument,
        .host = endpoints.ws_host,
        .port = endpoints.ws_port,
        .depth_path = endpoints.depth_path,
        .bbo_path = endpoints.bbo_path,
        .depth = ResolveDepth(venue, server_config.depth),
        // Same count for every venue. Per-venue tuning may eventually be needed -
        // OKX rate-limits connection ATTEMPTS more tightly than the others - but
        // no venue's limits have been verified, so three unverified numbers
        // would be guessing where one is defensible.
        .connections = server_config.connections,
    };

    switch (venue) {
        case VenueId::BINANCE:
            config.rest_host = endpoints.rest_host;
            config.rest_port = endpoints.rest_port;
            config.rest_depth_path = endpoints.rest_depth_path;
            // Binance publishes no keepalive on either stream, so these are
            // placeholders, not derived values - see types/venue.h.
            config.depth_backstop_ns = staleness::kBinanceDepthBackstopNs;
            config.bbo_backstop_ns = staleness::kBinanceBboBackstopNs;
            return std::make_unique<BinanceProvider>(config, std::move(on_update), std::move(on_quote));

        case VenueId::BYBIT:
            config.depth_backstop_ns = staleness::kBybitDepthBackstopNs;
            // The only tight, derived backstop we have: Bybit republishes L1
            // with the same `u` every 3s of no change.
            config.bbo_backstop_ns = staleness::kBybitBboBackstopNs;
            return std::make_unique<BybitProvider>(config, std::move(on_update), std::move(on_quote));

        case VenueId::OKX:
            // Futures only, and only to read the swap's contract size.
            config.rest_host = endpoints.rest_host;
            config.rest_port = endpoints.rest_port;
            config.rest_instruments_path = endpoints.rest_instruments_path;
            // Derived from OKX's ~60s seqId == prevSeqId keepalive.
            config.depth_backstop_ns = staleness::kOkxDepthBackstopNs;
            config.bbo_backstop_ns = staleness::kOkxBboBackstopNs;
            return std::make_unique<OKXProvider>(config, std::move(on_update), std::move(on_quote));

        case VenueId::COUNT:
            break;
    }

    Logger::Log(LogLevel::kError, "[ProviderFactory] BuildProvider called with invalid venue id {}",
                static_cast<int>(venue));
    return nullptr;
}

}  // namespace market_data
