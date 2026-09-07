// md_provider_app - one provider process for one (venue, market).
// Builds a single Provider via BuildProvider (provider_factory.h), but
// instead of calling Core::Enqueue* in-process it hands every message to a
// ZmqProviderSink, which puts the bytes on a DEALER dialed at md_core's ROUTER
// //
// Usage: md_provider_app <venue> <market> [core_endpoint]
//   venue          binance | bybit | okx   (case-insensitive)
//   market         spot | futures
//   core_endpoint  ZeroMQ endpoint md_core binds; default ipc:///run/md/core.ipc

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>  // std::setvbuf - see the top of main()
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "config/config.h"
#include "config/venues_config.h"
#include "logger/logger.h"
#include "md_provider/md_provider.h"
#include "md_wire/zmq_provider_sink.h"
#include "provider_factory.h"
#include "types/venue.h"

#include <zmq.hpp>

using namespace market_data;

namespace {

constexpr std::string_view kDefaultEndpoint = "ipc:///run/md/core.ipc";

std::atomic<bool> g_stop{false};

void OnSignal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

std::string ToUpper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Line-buffer stdout before anything logs - stdout is FULLY buffered when
    // it is a pipe, so under `docker logs` this process's entire startup
    // narrative (connect, handshake, subscribe, snapshot, sync) sat unflushed
    // and the container looked silent. See md_core_main.cpp for the full note.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (argc < 3 || argc > 4) {
        Logger::Log(LogLevel::kError, "[md_provider] usage: md_provider_app <venue> <market> [core_endpoint]");
        return 2;
    }
    const std::string venue_arg = ToUpper(argv[1]);
    const std::string market_arg = argv[2];
    const std::string endpoint = argc == 4 ? argv[3] : std::string(kDefaultEndpoint);

    const VenueId venue = VenueConverter::ToVenueId(venue_arg);
    if (venue == VenueId::COUNT) {
        Logger::Log(LogLevel::kError, "[md_provider] unknown venue \"{}\" - expected binance, bybit or okx", argv[1]);
        return 2;
    }
    const std::optional<MarketType> market = ToMarketType(market_arg);
    if (!market.has_value()) {
        Logger::Log(LogLevel::kError, "[md_provider] unknown market \"{}\" - expected spot or futures", market_arg);
        return 2;
    }

    // The same two config files md_core_app reads, for the same reasons: the
    // per-market session file (which instrument, how deep, how many redundant
    // connections) picked by the market arg above, and venues_config.json for
    // how a venue is reached.
    InstrumentRegistry instrument_registry;
    const ConfigLoadResult config_result = ServerConfig::LoadFile(ConfigFileForMarket(*market), instrument_registry);
    if (!config_result.Ok()) {
        Logger::Log(LogLevel::kError, "[md_provider] {}", config_result.error);
        return 2;
    }
    const ServerConfig server_config = config_result.config;
    if (!server_config.Validate()) {
        return 2;
    }

    const VenuesConfigLoadResult venues_result = VenuesConfig::LoadFile(std::string(kVenuesConfigFileName));
    if (!venues_result.Ok()) {
        Logger::Log(LogLevel::kError, "[md_provider] {}", venues_result.error);
        return 2;
    }
    const VenuesConfig& venues_config = venues_result.config;

    // Single-instrument, single-market: the same limit md_core_app enforces, for
    // the same reason - ProviderConfig carries one InstrumentKey and nothing
    // downstream is wired for more.
    if (server_config.instruments.size() != 1 || server_config.instruments.front().markets.size() != 1) {
        Logger::Log(LogLevel::kError, "[md_provider] {} must name exactly one instrument and one market",
                    ConfigFileForMarket(*market));
        return 2;
    }
    const InstrumentEntry& entry = server_config.instruments.front();

    if (std::find(server_config.venues.begin(), server_config.venues.end(), venue) == server_config.venues.end()) {
        Logger::Log(LogLevel::kError, "[md_provider] venue \"{}\" is not in the {} venues list", venue_arg,
                    ConfigFileForMarket(*market));
        return 2;
    }
    if (*market != entry.markets.front()) {
        Logger::Log(LogLevel::kError, "[md_provider] {} names market \"{}\" but \"{}\" was passed on the command line",
                    ConfigFileForMarket(*market), ToMarketString(entry.markets.front()), market_arg);
        return 2;
    }
    const VenueEndpoints* endpoints = venues_config.Find(venue, *market);
    if (endpoints == nullptr) {
        Logger::Log(LogLevel::kError, "[md_provider] {} has no endpoints for {} {}", kVenuesConfigFileName, venue_arg,
                    market_arg);
        return 2;
    }

    const InstrumentKey instrument_key = MakeKey(entry.id, *market);

    Logger::Log(LogLevel::kInfo, "[md_provider] starting venue={} instrument={} endpoint={} depth={} connections={}",
                venue_arg, VenueConverter::ToInstrumentString(instrument_key), endpoint, server_config.depth,
                server_config.connections);

    // The sink owns the DEALER; this process owns the context so it can be torn
    // down cleanly at shutdown. connect() queues to the HWM and delivers once
    // md_core binds, so it does not matter whether md_core is up yet.
    zmq::context_t zmq_ctx(1);
    wire::ZmqProviderSink sink(zmq_ctx, endpoint, instrument_key, venue_arg);

    // kHello first: md_core binds this connection's identity to a venue_slot on
    // kHello and cannot attribute anything sent before it. Safe to send
    // from this thread even though the provider's callbacks below fire on its
    // io_context thread - Start() spawns that thread after this point, so this
    // send happens-before any On* call and the socket is never touched
    // concurrently.
    sink.SendHello();

    std::unique_ptr<Provider> provider = BuildProvider(
        venue, instrument_key, *endpoints, server_config,
        [&sink](BookUpdate&& update) { sink.OnUpdate(std::move(update)); },
        [&sink](const BboQuote& quote) { sink.OnQuote(quote); });
    if (provider == nullptr) {
        Logger::Log(LogLevel::kError, "[md_provider] failed to build provider for venue \"{}\"", venue_arg);
        return 2;
    }
    provider->SetHealthCallback([&sink](const VenueHealthEvent& event) { sink.OnHealth(event); });

    // Now that the Provider exists, close the loop the sink could not: a
    // depth/health send that fails at the socket is a broken diff chain, and
    // only the provider can resync it. Provider outlives every hook
    // call - Stop() joins the io_context thread before this scope tears down.
    sink.SetResyncHook([p = provider.get()] { p->RequestResync(); });

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    provider->Start();
    Logger::Log(LogLevel::kInfo, "[md_provider] running - SIGINT/SIGTERM to stop");

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Stop the provider FIRST so its io_context thread is joined and no longer
    // touching the sink, THEN send kGoodbye from this thread - single-threaded
    // socket access again. kGoodbye only distinguishes a clean stop from a crash
    // in md_core's log; ROUTER_NOTIFY is what actually removes the venue.
    Logger::Log(LogLevel::kInfo, "[md_provider] stopping");
    provider->Stop();
    sink.SendGoodbye(wire::GoodbyeReason::kShutdown);

    return 0;
}
