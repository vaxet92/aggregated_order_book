#include "md_core/md_core.h"
#include "logger/logger.h"
#include "md_provider/binance/binance_provider.h"
#include "md_provider/bybit/bybit_provider.h"
#include "md_provider/okx/okx_provider.h"
#include "config/venues_config.h"
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include "types/venue.h"

int main() {
    Logger::Log(LogLevel::kInfo, "Hello, World!");

    market_data::CoreConfig config = {
        .venues = {VenueId::BINANCE, VenueId::BYBIT, VenueId::OKX},
        .default_instruments = {MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot)},
    };

    // Null callbacks on purpose: this mock only checks that the three
    // providers connect and feed real data into the venue books. It never
    // publishes, so there is nothing for a BBO or merged-book callback to do.
    market_data::Core core(nullptr, nullptr);
    core.Init(config);
    core.Start();

    auto on_update = [&core](const market_data::BookUpdate& update) { core.ApplyUpdate(update); };

    // Providers must outlive Start() - each Start() spawns its own internal
    // thread and returns immediately, so a provider constructed as a
    // temporary would be destroyed (and Stop()'d) right after Start()
    // returns, before any real data arrives. Keeping them here, in main()'s
    // scope, for the whole run is what avoids that.
    std::vector<std::unique_ptr<market_data::Provider>> providers;

    // Endpoints come from the same venues_config.json the aggregator uses, so
    // this test cannot drift from production by hardcoding a host that has
    // since moved. It must therefore run from a directory containing that file.
    const auto venues_result = market_data::VenuesConfig::LoadFile(std::string(market_data::kVenuesConfigFileName));
    if (!venues_result.Ok()) {
        std::cerr << venues_result.error << "\n";
        return 1;
    }

    const InstrumentKey instrument_key = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    const MarketType market = instrument_key.Market();

    auto make_config = [&](VenueId venue) {
        const market_data::VenueEndpoints& endpoints = *venues_result.config.Find(venue, market);
        return market_data::ProviderConfig{
            .venue_id = venue,
            .instrument = instrument_key,
            .host = endpoints.ws_host,
            .port = endpoints.ws_port,
            .depth_path = endpoints.depth_path,
            .bbo_path = endpoints.bbo_path,
            .rest_host = endpoints.rest_host,
            .rest_port = endpoints.rest_port,
            .rest_depth_path = endpoints.rest_depth_path,
        };
    };

    for (const VenueId venue : VenueIdArray) {
        if (venues_result.config.Find(venue, market) == nullptr) {
            std::cerr << "no endpoints for " << VenueConverter::ToVenueString(venue) << "\n";
            return 1;
        }
    }

    market_data::ProviderConfig binance_config = make_config(VenueId::BINANCE);
    providers.push_back(std::make_unique<market_data::BinanceProvider>(binance_config, on_update));

    market_data::ProviderConfig bybit_config = make_config(VenueId::BYBIT);
    providers.push_back(std::make_unique<market_data::BybitProvider>(bybit_config, on_update));

    market_data::ProviderConfig okx_config = make_config(VenueId::OKX);
    providers.push_back(std::make_unique<market_data::OKXProvider>(okx_config, on_update));

    for (auto& provider : providers) {
        provider->Start();
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    for (auto& provider : providers) {
        provider->Stop();
    }

    return 0;
}
