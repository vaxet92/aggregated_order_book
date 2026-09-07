#include "client_common.h"

#include <cstdio>  // std::setvbuf - see the top of main()

using namespace market_data;

// The production client: any combination of feeds, chosen on the command
// line. bbo_sub / volume_band_sub / price_band_sub are fixed-subscription
// test binaries covering one feed each; this one is what actually ships.
//
//   ./client_app --market=spot    --bbo
//   ./client_app --market=futures --bbo
//   ./client_app --market=spot    --notional_band=1,100K,1M,50M
//   ./client_app --market=spot    --price_band=50,100,200,500,1000
//   ./client_app --market=spot    --bbo --volume_bands --price_bands
//   ./client_app --server=aggregator:50051 --market=futures --bbo
//
// --market is required. Spot and futures are separate subscriptions, so
// running both markets at once means running two clients.
int main(int argc, char* argv[]) {
    // Line-buffer stdout before anything prints. This binary only APPEARED to
    // be fine under `docker logs` - it emits thousands of lines a minute, so
    // the 4KB pipe buffer flushed constantly and hid the same defect the
    // quieter core and provider processes made obvious. See md_core_main.cpp.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    ClientConfig config = ClientConfig::ParseFromArgs(argc, argv);

    // Checked here rather than left to the server: the server would reject
    // this with INVALID_ARGUMENT after a full connect/subscribe round trip,
    // and a usage message is a better answer than a gRPC status string.
    if (!config.HasAnyFeed() || !config.HasMarket()) {
        PrintUsage(argv[0]);
        return 2;
    }

    // PrintUpdate dispatches on payload_case, so one subscription carrying
    // several feeds interleaves correctly without the caller tracking which
    // is which.
    return RunSubscription(config, "client_app", config.ToRequest(), PrintUpdate);
}
