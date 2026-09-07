// Latency micro-benchmark for the Binance JSON parsers.
//
// Not a correctness test - the unit tests own that. This measures how long
// one ParseBinance* call takes, so an optimisation can be judged against a
// baseline number (CLAUDE.md section 7: measure before optimising).
//
// Build is guarded by BUILD_BENCHMARKS (default OFF) so it never blocks the
// normal build or the Docker image.
//
// Usage:  bench_binance_parser [iterations]   (default 2000)

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "md_provider/binance/binance_parser.h"
#include "latency_benchmark.h"

using namespace market_data;

// --- fixtures -------------------------------------------------------------
// Real message shapes. Two depth deltas so we can see how cost scales with
// the number of price levels, plus one bookTicker.

constexpr const char* kDepthSmall =
    R"({"e":"depthUpdate","E":1672515782136,"s":"BTCUSDT","U":157,"u":160,"b":[["27000.10","0.5"]],"a":[["27000.20","1.25"]]})";

constexpr const char* kDepthDeep = R"({"e":"depthUpdate","E":1672515782136,"s":"BTCUSDT","U":157,"u":180,
"b":[["27000.10","0.5"],["27000.09","0.10"],["27000.08","0.20"],["27000.07","0.30"],["27000.06","0.40"],
["27000.05","0.50"],["27000.04","0.60"],["27000.03","0.70"],["27000.02","0.80"],["27000.01","0.90"]],
"a":[["27000.20","1.25"],["27000.21","0.10"],["27000.22","0.20"],["27000.23","0.30"],["27000.24","0.40"],
["27000.25","0.50"],["27000.26","0.60"],["27000.27","0.70"],["27000.28","0.80"],["27000.29","0.90"]]})";

constexpr const char* kBbo =
    R"({"u":400900217,"s":"BTCUSDT","b":"27000.10000000","B":"31.21000000","a":"27000.20000000","A":"40.66000000"})";

int main(int argc, char** argv) {
    std::size_t iterations = 2000;
    if (argc > 1) {
        const long v = std::atol(argv[1]);
        if (v > 0) iterations = static_cast<std::size_t>(v);
    }
    const std::size_t warmup = std::min<std::size_t>(1000, iterations);

    const std::string depth_small = kDepthSmall;
    const std::string depth_deep = kDepthDeep;
    const std::string bbo = kBbo;

    std::printf("Binance parser latency  (iterations=%zu, warmup=%zu)\n", iterations, warmup);

    LatencyBenchmark bench(iterations, warmup);

    // One parser, reused across every iteration - this is what the class
    // change is meant to exploit. A per-call parser would be the old cost.
    BinanceParser parser(/*venue_depth=*/1000);

    bench.Measure("depth_small", [&] {
        auto u =
            parser.ParseDepthMessage(depth_small, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
        return u ? u->seq + u->bids.size() + u->asks.size() : 0;
    });
    bench.Measure("depth_deep", [&] {
        auto u =
            parser.ParseDepthMessage(depth_deep, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
        return u ? u->seq + u->bids.size() + u->asks.size() : 0;
    });
    bench.Measure("bbo", [&] {
        auto q = parser.ParseBboMessage(bbo, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
        return q ? q->seq + q->bid_price + q->ask_price : 0;
    });

    return 0;
}
