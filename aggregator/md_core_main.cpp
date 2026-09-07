// md_core_app - the consolidation + publish process.
// It owns the Core, the gRPC server, and one ZmqCoreIngress bound on the
// ROUTER. Providers are separate md_provider_app processes that dial the ingress
// over ipc://; nothing in this process opens an exchange connection.
//
// Usage: md_core_app [core_endpoint] --market=spot|futures [--grpc_port=N]
//   core_endpoint  ZeroMQ endpoint to bind; default ipc:///run/md/core.ipc
//   --market=      required; selects server_config_<market>.json
//   --grpc_port=N  overrides the config's grpc_port

#include "aggregator_service.h"
#include "config/config.h"
#include "latency_recorder.h"
#include "logger/logger.h"
#include "md_core/md_core.h"
#include "md_wire/zmq_core_ingress.h"
#include "types/venue.h"
#include "wire_translation.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>  // std::setvbuf - see the top of main()
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace market_data;

namespace {

constexpr std::string_view kDefaultEndpoint = "ipc:///run/md/core.ipc";

std::atomic<bool> g_stop{false};

void OnSignal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char* argv[]) {
    // Line-buffer stdout BEFORE anything logs.
    //
    // stdout is FULLY buffered when it is a pipe rather than a terminal,
    // which is exactly what `docker logs`, systemd and a shell redirect all
    // give it. This process logs comparatively little - startup lines and a
    // [latency] report per 1000 publishes - so it never accumulated enough to
    // flush, and `docker logs md-core-spot` returned ZERO lines while the core
    // was demonstrably serving clients. The instrumentation was working the
    // whole time and simply could not be read, which is the worst version of
    // this bug: the measurement exists and the operator cannot see it.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    // md_core_app [core_endpoint] --market=spot|futures [--grpc_port=N].
    // --market is required (selects the config file); --depth is
    // provider-side, and connections has no CLI flag at all (JSON only, see
    // config.h) - so this does not reuse CliOverrides.
    std::string endpoint(kDefaultEndpoint);
    std::optional<int> grpc_port_override;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.rfind("--grpc_port=", 0) == 0) {
            grpc_port_override = std::stoi(std::string(arg.substr(12)));
        } else if (arg.rfind("--market=", 0) == 0) {
            // Consumed by ParseMarketFlag below.
        } else if (arg.rfind("--", 0) == 0) {
            Logger::Log(LogLevel::kError, "[md_core] unknown flag \"{}\"", arg);
            return 2;
        } else {
            endpoint = std::string(arg);
        }
    }

    const std::optional<MarketType> market_flag = ParseMarketFlag(argc, argv);
    if (!market_flag.has_value()) {
        Logger::Log(LogLevel::kError, "[md_core] --market=spot|futures is required");
        return 2;
    }

    // The per-market session file (which instrument, gRPC port). No
    // venues_config.json: this process dials nothing - venue endpoints are a
    // md_provider_app concern.
    InstrumentRegistry instrument_registry;
    const ConfigLoadResult config_result =
        ServerConfig::LoadFile(ConfigFileForMarket(*market_flag), instrument_registry);
    if (!config_result.Ok()) {
        Logger::Log(LogLevel::kError, "[md_core] {}", config_result.error);
        return 2;
    }
    ServerConfig server_config = config_result.config;
    if (grpc_port_override.has_value()) {
        server_config.grpc_port = *grpc_port_override;
    }

    if (!server_config.Validate()) {
        return 2;
    }

    // Same limit md_provider_app enforces: one InstrumentKey end to end, and
    // the ingress refuses a kHello for any other instrument.
    if (server_config.instruments.size() != 1 || server_config.instruments.front().markets.size() != 1) {
        Logger::Log(LogLevel::kError,
                    "[md_core] exactly one instrument naming exactly one market is supported - "
                    "multi-instrument wiring is not implemented yet");
        return 2;
    }
    const InstrumentEntry& entry = server_config.instruments.front();
    if (entry.markets.front() != *market_flag) {
        Logger::Log(LogLevel::kError, "[md_core] {} names market \"{}\" but --market=\"{}\" was passed",
                    ConfigFileForMarket(*market_flag), ToMarketString(entry.markets.front()),
                    ToMarketString(*market_flag));
        return 2;
    }
    const InstrumentKey instrument_key = MakeKey(entry.id, entry.markets.front());

    Logger::Log(LogLevel::kInfo, "[md_core] starting (instrument={}, grpc_port={}, ingress={})", entry.symbol,
                server_config.grpc_port, endpoint);

    AggregatorServiceImpl service;

    LatencyRecorder publish_latency("book_publish", /*report_every=*/1000, /*warmup=*/200);

    // The VenueWireTable turns Core's per-slot attribution back into a venue on
    // the wire. Venues register on the ingress thread AFTER gRPC is already
    // serving (a provider connecting is the kHello), so the table is rebuilt on
    // demand when the venue count changes rather than once up front.
    //
    // no synchronisation. venue_wire_table_ is read only in PublishBbo /
    // PublishBook, both of which fire on the single consolidator thread, and
    // this refresh runs at the top of those same callbacks - so SetVenueWireTable
    // and every ToWire read are serialised by construction. core.venue_count()
    // and core.VenueName() are the documented single-writer (ingress) /
    // many-reader (consolidator) accesses. venue_count() is monotonic:
    // RemoveVenue keeps the slot, so a simple count compare is enough.
    Core* core_ptr = nullptr;
    std::size_t published_venue_count = 0;
    auto refresh_wire_table = [&] {
        const std::size_t n = core_ptr->venue_count();
        if (n == published_venue_count) {
            return;
        }
        service.SetVenueWireTable(MakeVenueWireTable([&](VenueSlot slot) { return core_ptr->VenueName(slot); }));
        published_venue_count = n;
    };

    // The callback seam between Core and gRPC, with the wire-table refresh in
    // front. A venue may send quotes before depth, so both callbacks refresh.
    Core core(
        [&](InstrumentKey instrument, const consolidated::BBO& bbo) {
            refresh_wire_table();
            service.PublishBbo(instrument, bbo);
        },
        [&](InstrumentKey instrument, const consolidated::Book& book) {
            refresh_wire_table();
            publish_latency.Record(book.source_mono_ns, book.venue_levels);
            service.PublishBook(instrument, book);
        });
    core_ptr = &core;  // set before Start(); callbacks fire only after

    CoreConfig config = {
        .venues = server_config.venues,
        .default_instruments = {instrument_key},
    };
    core.Init(config);

    TimingBreakdown timing_breakdown(/*report_every=*/2000, /*warmup=*/200);
    core.SetInstrumentation(&LatencyRecorder::NowMonotonicNs, [&timing_breakdown](const Core::ApplyTimings& timings) {
        timing_breakdown.Record(timings.lock_wait_ns, timings.book_apply_ns, timings.merge_ns, timings.merged_depth,
                                timings.delta_levels, timings.fast_path_sides, timings.applied_sides);
    });

    // Consolidator first: no venue is registered yet, so it idles until the
    // ingress drains the first VenueRegistration.
    core.Start();

    wire::ZmqCoreIngress ingress(endpoint, core, instrument_key);
    ingress.Start();

    const std::string server_address = fmt::format("0.0.0.0:{}", server_config.grpc_port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    Logger::Log(LogLevel::kInfo, "[md_core] gRPC server listening on {}", server_address);

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    Logger::Log(LogLevel::kInfo, "[md_core] running - SIGINT/SIGTERM to stop");

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Shut down front to back: stop accepting RPCs (no gRPC thread left in
    // service), then stop the ingress (no more Enqueue* into Core), then drain
    // and join the consolidator. ingress.Stop() BEFORE core.Stop() so the final
    // drain is clean (md_wire/zmq_core_ingress.h).
    Logger::Log(LogLevel::kInfo, "[md_core] stopping");
    server->Shutdown();
    ingress.Stop();
    core.Stop();

    return 0;
}
