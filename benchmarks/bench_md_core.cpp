// Latency micro-benchmark for the order book hot path: per-venue apply, the
// k-way merge, and the BBO.
//
// Not a correctness test - the unit tests own that. This produced the BEFORE
// numbers that CLAUDE.md section 7 requires before the DESIGN.md section 14.2
// optimisations (flat-vector book, incremental merge) could be justified. The
// std::map comparison arm that motivated the flat book has been removed with
// MapOrderBook itself; the map-vs-flat numbers were recorded under the
// pre-cleanup baseline and are not kept in-tree.
//
// a nanosecond figure here is NOT a decision on its own. It only becomes
// one when multiplied by the real message rate. At 60 updates/sec a 50us
// merge is 0.3% of a core and there is nothing to optimise; at 50k/sec the
// same number saturates it. The rate is measured separately - do not draw a
// conclusion from this file alone.
//
// What this still isolates:
//
//   merge_flat_3venue - merge_flat_1venue    what the k-way SELECTION costs
//   inc_publish_N vs full_publish_N           incremental merge vs full rebuild
//
// Build guarded by BUILD_BENCHMARKS (default OFF).
//
// Usage:  bench_md_core [iterations]   (default 2000)

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <vector>

#include "latency_benchmark.h"
#include "md_core/consolidated_bbo.h"
#include "md_core/consolidated_book.h"
#include "md_core/consolidated_state.h"
#include "md_core/flat_order_book.h"

using namespace market_data;
using namespace market_data::consolidated;

namespace {

// --- fixture scale ---------------------------------------------------------
//
// Production scale, not toy numbers. Band accumulation runs in __int128
// precisely because price x qty at this scale overflows uint64, so a fixture
// with single-digit prices would exercise a different code path than the one
// we are trying to measure.

constexpr PriceTicks kMidPrice = 50'000 * kScaleFactor;  // 50k USDT
constexpr PriceTicks kTick = kScaleFactor / 100;         // 0.01 USDT

// What the aggregator ACTUALLY subscribes to at the default --depth=500,
// after SelectDepthTier rounds each venue to a tier it publishes:
//
//   Binance  500   exact tier
//   Bybit   1000   500 rounds UP to the next tier {1, 50, 200, 1000}
//   OKX      400   500 exceeds its deepest tier, so it is capped and warned
//
// Confirmed against a live run: "orderbook.1000.BTCUSDT" on the wire.
constexpr size_t kBinanceLevels = 1000;
constexpr size_t kBybitLevels = 1000;
constexpr size_t kOkxLevels = 400;

// this is NOT 1900 output levels. Every venue sits on the same tick grid
// anchored at the same mid, so they OVERLAP - the merged depth is the DEEPEST
// venue's depth, not the sum. Here that is Bybit's 1000, with three venues
// contributing to levels 1-400, two to 401-500, and one beyond that.
//
// That is the realistic shape and it also exercises the tie path heavily,
// which is the expensive one: a second scan across all venues per output
// level. A real book has gaps in its price grid, so contiguous ticks are
// slightly pessimistic on tie frequency - the right direction for a
// before-number.
constexpr size_t kRealisticMergedDepth = kBybitLevels;

// For the selection-isolation runs below, every venue gets the SAME depth, so
// the output level count is constant no matter how many venues take part and
// the only variable is how many venues the merge must choose between.
constexpr size_t kIsolationDepth = 1000;

// venues must OVERLAP in price. They quote the same asset, so near the
// top of book they land on the same ticks constantly, and the merge's
// tie-handling path - a second scan across all venues per output level - runs
// on almost every level. Disjoint fixtures would skip that path entirely and
// report a merge that is cheaper than the real one.
//
// Every venue is anchored at the same mid and walks the same tick grid, so
// overlap is total where their depths overlap. That is the pessimistic end of
// realistic, which is the right end for a before-number.

BookUpdate MakeSnapshot(VenueId venue, size_t levels) {
    BookUpdate update{};
    update.venue = venue;
    update.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    update.seq = 1;
    update.is_snapshot = true;
    update.bids.reserve(levels);
    update.asks.reserve(levels);

    // Deterministic quantities - a fixed seed, so two runs are comparable.
    // Varying them matters: identical quantities would let the compiler and
    // the branch predictor do better here than they can in production.
    std::mt19937_64 rng(static_cast<uint64_t>(venue) * 7919 + 42);
    std::uniform_int_distribution<uint64_t> qty_dist(kScaleFactor / 100, 5 * kScaleFactor);

    for (size_t i = 0; i < levels; ++i) {
        const PriceTicks offset = static_cast<PriceTicks>(i + 1) * kTick;
        update.bids.push_back({kMidPrice - offset, qty_dist(rng)});
        update.asks.push_back({kMidPrice + offset, qty_dist(rng)});
    }
    return update;
}

// Templated on the book type - kept generic from when a std::map arm ran
// alongside the flat one from the identical fixture. Only FlatOrderBook is
// instantiated now; a second implementation would slot
// in here unchanged.
template <typename Book>
std::unique_ptr<Book> MakeBook(VenueId venue, size_t levels) {
    auto book = std::make_unique<Book>(venue, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    book->ApplyUpdate(MakeSnapshot(venue, levels));
    return book;
}

// Median / p99 / max of a sample set, sorted in place.
//
// this summarises BYTES MOVED, and that column exists only for the flat
// book. std::map moves no bytes at all - it allocates and frees nodes - so an
// empty cell there is not a zero, and the two implementations are simply not
// comparable on this metric. They are compared on ns/call instead.
//
// Bytes moved is reported because it is the figure that TRANSFERS: nanoseconds
// describe this laptop, while bytes moved describe the algorithm and predict it
// on any machine. The p99 is the point of it - once the in-place version lands
// the mean should sit near zero, and the only interesting case left is a new
// price at the top of a deep book, which a median hides completely.
struct MovedStats {
    double mean = 0.0;
    uint64_t median = 0;
    uint64_t p99 = 0;
    uint64_t max = 0;
};

MovedStats SummariseMoved(std::vector<uint64_t>& samples) {
    MovedStats stats;
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    long double total = 0.0L;
    for (uint64_t value : samples) {
        total += static_cast<long double>(value);
    }
    stats.mean = static_cast<double>(total / static_cast<long double>(samples.size()));
    stats.median = samples[samples.size() / 2];
    stats.p99 = samples[static_cast<size_t>(static_cast<double>(samples.size()) * 0.99)];
    stats.max = samples.back();
    return stats;
}

void PrintMoved(const char* name, MovedStats stats) {
    std::printf("  %-20s  mean %10.1f  median %8llu  p99 %8llu  max %8llu  bytes/diff\n", name, stats.mean,
                static_cast<unsigned long long>(stats.median), static_cast<unsigned long long>(stats.p99),
                static_cast<unsigned long long>(stats.max));
}

// Runtime-settable, because the right values are an empirical question and
// the defaults above are read off the code rather than off a live book. Run
// with different depths rather than arguing about the constants:
//
//   bench_md_core 2000 --binance=20 --bybit=1000 --okx=400
//
// This matters more than it looks. The whole point of the exercise is to find
// out whether std::map is the bottleneck, and that answer depends heavily on
// how many levels are in the book - a 20-level map fits in cache and a
// 1000-level one does not.
struct Fixture {
    size_t binance = kBinanceLevels;
    size_t bybit = kBybitLevels;
    size_t okx = kOkxLevels;
    size_t isolation = kIsolationDepth;
};

// Gives `venue_count` venues the SAME depth, so every variant produces the
// same number of OUTPUT levels and the only thing that changes is how many
// venues the merge compares per level.
//
// holding the OUTPUT constant is the correct control, not the total
// input. Because the venues share one tick grid they overlap, so three venues
// of 333 levels produce 333 output levels while one venue of 1000 produces
// 1000 - splitting a fixed total would change the output size and the
// comparison would measure that instead of the selection cost.
template <typename BookArray>
void FillIsolation(BookArray& books, size_t venue_count, size_t depth) {
    using Book = typename BookArray::value_type::element_type;
    for (auto& book : books) {
        book.reset();
    }
    static constexpr VenueId kOrder[] = {VenueId::BINANCE, VenueId::OKX, VenueId::BYBIT};
    for (size_t i = 0; i < venue_count; ++i) {
        books[static_cast<size_t>(kOrder[i])] = MakeBook<Book>(kOrder[i], depth);
    }
}

// The realistic shape: each venue at the depth it is actually subscribed to.
// Merged output is the DEEPEST venue's depth, not the sum, because they
// overlap - with the defaults that is Bybit's 1000, where levels 1-400 have
// three contributors, 401-500 have two, and the rest have one.
template <typename BookArray>
void FillRealistic(BookArray& books, const Fixture& fixture) {
    using Book = typename BookArray::value_type::element_type;
    for (auto& book : books) {
        book.reset();
    }
    books[static_cast<size_t>(VenueId::BINANCE)] = MakeBook<Book>(VenueId::BINANCE, fixture.binance);
    books[static_cast<size_t>(VenueId::OKX)] = MakeBook<Book>(VenueId::OKX, fixture.okx);
    books[static_cast<size_t>(VenueId::BYBIT)] = MakeBook<Book>(VenueId::BYBIT, fixture.bybit);
}

// Minimal --key=value parsing. Not worth a config library in a benchmark.
bool ParseSizeArg(const char* arg, const char* key, size_t& out) {
    const size_t key_len = std::strlen(key);
    if (std::strncmp(arg, key, key_len) != 0) {
        return false;
    }
    const long value = std::atol(arg + key_len);
    if (value > 0) {
        out = static_cast<size_t>(value);
    }
    return true;
}

// --- delta fixtures --------------------------------------------------------

// Rewrites quantities at prices that ALREADY exist. std::map does a lookup
// and an assignment: no node allocation, no rebalancing.
BookUpdate MakeQtyUpdate(size_t levels) {
    BookUpdate update{};
    update.venue = VenueId::BINANCE;
    update.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    update.seq = 2;
    update.is_snapshot = false;
    for (size_t i = 0; i < levels; ++i) {
        const PriceTicks offset = static_cast<PriceTicks>(i + 1) * kTick;
        update.bids.push_back({kMidPrice - offset, 3 * kScaleFactor});
        update.asks.push_back({kMidPrice + offset, 3 * kScaleFactor});
    }
    return update;
}

// Same shape as MakeQtyUpdate, but with a chosen quantity so two of them can be
// ALTERNATED.
//
// MakeQtyUpdate re-sends the same quantity every iteration, and
// FlatOrderBook now suppresses a repeated quantity as a no-op - no store, and
// no LevelChange. A loop over one such update therefore measures the no-op path
// and says nothing about what applying or reporting a real change costs.
// Alternating between two quantities is what makes the measurement mean what
// its name says.
BookUpdate MakeQtyUpdateOf(size_t levels, uint64_t qty_multiple) {
    BookUpdate update{};
    update.venue = VenueId::BINANCE;
    update.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    update.seq = 2;
    update.is_snapshot = false;
    for (size_t i = 0; i < levels; ++i) {
        const PriceTicks offset = static_cast<PriceTicks>(i + 1) * kTick;
        update.bids.push_back({kMidPrice - offset, qty_multiple * kScaleFactor});
        update.asks.push_back({kMidPrice + offset, qty_multiple * kScaleFactor});
    }
    return update;
}

// the one that matters. Removes levels (qty == 0 -> erase) and adds new
// prices that were not in the book (-> insert, which allocates a node and
// rebalances the tree). std::map's real cost is node allocation, and a
// benchmark that only rewrote existing quantities would miss it entirely and
// make the map look better than it is.
//
// Erase and insert are paired in ONE call so the book returns to its starting
// size and the measurement is repeatable across iterations.
struct ChurnDeltas {
    BookUpdate remove;
    BookUpdate add;
};

// `start_offset` is in ticks from the mid. 500 puts the churn deep in the book,
// away from the top, so it does not also change the best price; 1 puts it AT
// the best price and the 19 levels behind it (MakeSnapshot lays levels at
// offsets 1..N, so offset 1 is the top of book).
//
// the pair exists to separate two cases the flat book treats very
// differently. Its cost is set by how DEEP the delta reaches - the best price
// lives at back(), so a top-of-book structural change rewrites a handful of
// levels while a deep one rewrites everything above it. std::map has no such
// asymmetry: a node is a node wherever it sits. Reporting only the deep arm
// measures the flat book at its worst; reporting only the shallow one flatters
// it. Both, or neither.
ChurnDeltas MakeChurn(size_t levels, size_t start_offset) {
    ChurnDeltas churn;
    for (BookUpdate* update : {&churn.remove, &churn.add}) {
        update->venue = VenueId::BINANCE;
        update->instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
        update->seq = 3;
        update->is_snapshot = false;
    }
    for (size_t i = 0; i < levels; ++i) {
        const PriceTicks offset = static_cast<PriceTicks>(start_offset + i) * kTick;
        churn.remove.bids.push_back({kMidPrice - offset, 0});
        churn.remove.asks.push_back({kMidPrice + offset, 0});
        churn.add.bids.push_back({kMidPrice - offset, 2 * kScaleFactor});
        churn.add.asks.push_back({kMidPrice + offset, 2 * kScaleFactor});
    }
    return churn;
}

// --- quote fixtures --------------------------------------------------------

BboQuote MakeQuote(VenueId venue, PriceTicks bid, PriceTicks ask) {
    BboQuote quote;
    quote.venue = venue;
    quote.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    quote.bid_price = bid;
    quote.bid_qty = kScaleFactor;
    quote.ask_price = ask;
    quote.ask_qty = kScaleFactor;
    return quote;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 2000;
    Fixture fixture;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (ParseSizeArg(arg, "--binance=", fixture.binance)) continue;
        if (ParseSizeArg(arg, "--bybit=", fixture.bybit)) continue;
        if (ParseSizeArg(arg, "--okx=", fixture.okx)) continue;
        if (ParseSizeArg(arg, "--isolation=", fixture.isolation)) continue;
        const long v = std::atol(arg);
        if (v > 0) iterations = static_cast<std::size_t>(v);
    }
    const std::size_t warmup = std::min<std::size_t>(1000, iterations);

    // Overlapping venues means merged depth is the DEEPEST venue, not the sum.
    const size_t merged_depth = std::max({fixture.binance, fixture.bybit, fixture.okx});

    std::printf("md_core order book latency  (iterations=%zu, warmup=%zu)\n", iterations, warmup);
    std::printf("fixture: binance=%zu bybit=%zu okx=%zu  ->  %zu merged levels (venues share one tick grid)\n",
                fixture.binance, fixture.bybit, fixture.okx, merged_depth);
    std::printf("prices x1e8 around %llu, tick %llu\n\n", static_cast<unsigned long long>(kMidPrice),
                static_cast<unsigned long long>(kTick));

    LatencyBenchmark bench(iterations, warmup);

    const BookUpdate qty_small = MakeQtyUpdate(5);
    const BookUpdate qty_deep = MakeQtyUpdate(50);
    // Erase then re-insert, so the book size is unchanged per call. The
    // reported cost is for BOTH halves together - 20 erases plus 20 inserts -
    // not for one of them.
    const ChurnDeltas churn = MakeChurn(20, 500);
    const ChurnDeltas churn_top = MakeChurn(20, 1);

    // ---- 1. per-venue apply, flat book -----------------------------------
    std::printf("FlatOrderBook::ApplyUpdate  (sorted vector)\n");
    std::vector<uint64_t> moved_qty_deep;
    std::vector<uint64_t> moved_churn;
    std::vector<uint64_t> moved_churn_top;
    {
        auto book = MakeBook<FlatOrderBook>(VenueId::BINANCE, kBinanceLevels);
        moved_qty_deep.reserve(iterations);
        moved_churn.reserve(iterations);
        moved_churn_top.reserve(iterations);

        bench.Measure("flat_qty_update_5", [&] {
            book->ApplyUpdate(qty_small);
            return book->last_seq();
        });
        bench.Measure("flat_qty_update_50", [&] {
            book->ApplyUpdate(qty_deep);
            moved_qty_deep.push_back(book->last_bytes_moved());
            return book->last_seq();
        });
        bench.Measure("flat_churn_20x2", [&] {
            book->ApplyUpdate(churn.remove);
            const uint64_t removed = book->last_bytes_moved();
            book->ApplyUpdate(churn.add);
            moved_churn.push_back(removed + book->last_bytes_moved());
            return book->bids().size();
        });
        bench.Measure("flat_churn_top_20x2", [&] {
            book->ApplyUpdate(churn_top.remove);
            const uint64_t removed = book->last_bytes_moved();
            book->ApplyUpdate(churn_top.add);
            moved_churn_top.push_back(removed + book->last_bytes_moved());
            return book->bids().size();
        });
    }

    // ---- 2. the merge, isolating the selection cost -------------------------
    //
    // Same total level count in all three, so the ONLY variable is how many
    // venues the merge has to choose between per output level.
    std::printf("\nMergeBooks - venue count at constant %zu OUTPUT levels\n", fixture.isolation);
    {
        Book merged;
        FlatBookArray books{};

        FillIsolation(books, 1, fixture.isolation);
        bench.Measure("merge_flat_1venue", [&] {
            MergeBooks(books, kVenueCount, merged);
            return merged.bids.size();
        });

        FillIsolation(books, 2, fixture.isolation);
        bench.Measure("merge_flat_2venue", [&] {
            MergeBooks(books, kVenueCount, merged);
            return merged.bids.size();
        });

        FillIsolation(books, 3, fixture.isolation);
        bench.Measure("merge_flat_3venue", [&] {
            MergeBooks(books, kVenueCount, merged);
            return merged.bids.size();
        });
    }

    // ---- 3. the realistic shape, and how cost scales with output depth ------
    std::printf("\nMergeBooks - realistic depths (Binance %zu / OKX %zu / Bybit %zu), by max_depth\n", fixture.binance,
                fixture.okx, fixture.bybit);
    {
        Book merged;
        FlatBookArray books{};
        FillRealistic(books, fixture);

        bench.Measure("merge_flat_depth_50", [&] {
            MergeBooks(books, kVenueCount, merged, 50);
            return merged.bids.size();
        });
        bench.Measure("merge_flat_depth_400", [&] {
            MergeBooks(books, kVenueCount, merged, 400);
            return merged.bids.size();
        });
        bench.Measure("merge_flat_full", [&] {
            MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth);
            return merged.bids.size();
        });
    }

    // ---- 3b. incremental consolidated merge vs full rebuild -----------------
    //
    // THE comparison this optimisation lives or dies by. Both arms do the same
    // unit of work end to end - apply one venue's delta, then produce
    // everything a client actually receives:
    //
    //   full_publish_N   ApplyUpdate + MergeBooks       + bands   <- before
    //   inc_publish_N    ApplyUpdate + ApplyDepthDelta  + bands   <- after
    //   inc_apply_N      ApplyUpdate + ApplyDepthDelta           (no bands)
    //
    // there is no snapshot copy in either arm any more, because Core now
    // publishes its live consolidated book BY REFERENCE. An earlier version of
    // this section measured the copy as ~8.2 us - 98% of what publishing cost
    // once the merge had fallen to 167 ns - which is what motivated removing
    // it. The band walk is what publishing costs now, so it belongs in the
    // measurement rather than outside it.
    //
    // inc_apply is kept separate because the two halves are attacked by
    // different work: inc_apply is the consolidation, and the difference is the
    // band walk, which is O(depth) however cheap the merge becomes.
    //
    // Deltas alternate between two quantities so every iteration is a real
    // change - see MakeQtyUpdateOf.
    std::printf("\nIncremental consolidated merge vs full rebuild (both including the band walk)\n");
    {
        // The assignment's default bands, sorted ascending as
        // the single-walk band functions require.
        const std::vector<uint64_t> notional_bands = {1'000'000ULL * kScaleFactor, 5'000'000ULL * kScaleFactor,
                                                      10'000'000ULL * kScaleFactor, 25'000'000ULL * kScaleFactor,
                                                      50'000'000ULL * kScaleFactor};
        const std::vector<uint32_t> bps_bands = {50, 100, 200, 500, 1000};

        std::vector<NotionalFill> notional_out;
        std::vector<BpsFill> bps_out;

        // Exactly what AggregatorServiceImpl::PublishBook does: cap the side at
        // the publication depth, then walk it once per band family.
        auto walk_bands = [&](const Book& book) {
            const auto capped = [](const std::vector<MergedLevel>& side) {
                return std::span<const MergedLevel>(side).first(std::min(side.size(), kDefaultMaxDepth));
            };
            FillToNotionalBands(capped(book.bids), notional_bands, notional_out);
            FillToNotionalBands(capped(book.asks), notional_bands, notional_out);
            FillToBpsBands(capped(book.bids), bps_bands, /*is_bid=*/true, bps_out);
            FillToBpsBands(capped(book.asks), bps_bands, /*is_bid=*/false, bps_out);
            return book.bids.size();
        };

        for (const size_t levels : {size_t{5}, size_t{50}}) {
            const BookUpdate qty_a = MakeQtyUpdateOf(levels, 3);
            const BookUpdate qty_b = MakeQtyUpdateOf(levels, 4);
            char name[64];

            {
                FlatBookArray books{};
                FillRealistic(books, fixture);
                FlatOrderBook& binance = *books[static_cast<size_t>(VenueId::BINANCE)];
                Book merged;
                bool flip = false;

                std::snprintf(name, sizeof(name), "full_publish_%zu", levels);
                bench.Measure(name, [&] {
                    binance.ApplyUpdate(flip ? qty_a : qty_b);
                    flip = !flip;
                    MergeBooks(books, kVenueCount, merged, kDefaultMaxDepth);
                    return walk_bands(merged);
                });
            }
            {
                FlatBookArray books{};
                FillRealistic(books, fixture);
                FlatOrderBook& binance = *books[static_cast<size_t>(VenueId::BINANCE)];
                State state;
                state.FullRebuild(books, kVenueCount);
                bool flip = false;

                std::snprintf(name, sizeof(name), "inc_publish_%zu", levels);
                bench.Measure(name, [&] {
                    binance.ApplyUpdate(flip ? qty_a : qty_b);
                    flip = !flip;
                    state.ApplyDepthDelta(static_cast<VenueSlot>(VenueId::BINANCE), binance.last_bid_changes(),
                                          binance.last_ask_changes());
                    return walk_bands(state.book());
                });
            }
            {
                FlatBookArray books{};
                FillRealistic(books, fixture);
                FlatOrderBook& binance = *books[static_cast<size_t>(VenueId::BINANCE)];
                State state;
                state.FullRebuild(books, kVenueCount);
                bool flip = false;

                std::snprintf(name, sizeof(name), "inc_apply_%zu", levels);
                bench.Measure(name, [&] {
                    binance.ApplyUpdate(flip ? qty_a : qty_b);
                    flip = !flip;
                    state.ApplyDepthDelta(static_cast<VenueSlot>(VenueId::BINANCE), binance.last_bid_changes(),
                                          binance.last_ask_changes());
                    return state.book().bids.size();
                });
            }
        }
    }

    // ---- 4. traversal alone ----------------------------------------------
    //
    // the measurement that decided the flat book. It walks every level of
    // all three books with no merge logic - just the iteration and an add. When
    // the books were std::maps this came in at or above merge_full, meaning the
    // merge was ~100% pointer-chasing red-black tree nodes; that is why the flat
    // book exists. The map arm is gone with MapOrderBook, and what remains is
    // the flat baseline: the same
    // walk over contiguous memory, which a prefetcher follows.
    std::printf("\nTraversal alone (no merge logic)\n");
    {
        FlatBookArray books{};
        FillRealistic(books, fixture);

        bench.Measure("iterate_flat_only", [&] {
            uint64_t sum = 0;
            for (const auto& book : books) {
                if (!book) continue;
                for (const auto& [price, qty] : book->bids()) {
                    sum += price + qty;
                }
                for (const auto& [price, qty] : book->asks()) {
                    sum += price + qty;
                }
            }
            return sum;
        });
    }

    // ---- 5. the BBO, expected to be negligible ------------------------------
    //
    // Included to CONFIRM that rather than assume it. The quote stream can be
    // faster than the depth stream, so "it is only three venues" is not by
    // itself an argument that it does not matter.
    std::printf("\nBBO\n");
    {
        VenueQuoteArray quotes{};
        quotes[static_cast<size_t>(VenueId::BINANCE)] =
            MakeQuote(VenueId::BINANCE, kMidPrice - kTick, kMidPrice + kTick);
        quotes[static_cast<size_t>(VenueId::OKX)] = MakeQuote(VenueId::OKX, kMidPrice - 2 * kTick, kMidPrice + kTick);
        quotes[static_cast<size_t>(VenueId::BYBIT)] =
            MakeQuote(VenueId::BYBIT, kMidPrice - kTick, kMidPrice + 2 * kTick);

        BBO bbo = ComputeBBOFromQuotes(quotes);
        bbo.best_bid.venues.reserve(kVenueCount);
        bbo.best_ask.venues.reserve(kVenueCount);

        // Alternates the incoming price so the update does not settle into one
        // branch - a quote that never changes the best level would only ever
        // exercise UpdateEqualPrice.
        uint64_t tick = 0;
        bench.Measure("bbo_incremental", [&] {
            const PriceTicks bid = kMidPrice - kTick - static_cast<PriceTicks>(tick++ % 3) * kTick;
            BboQuote quote = MakeQuote(VenueId::BINANCE, bid, kMidPrice + kTick);
            // slot == VenueId here: the fixture fills the quote array by index
            // rather than going through Core::RegisterVenue, so it picks that
            // layout deliberately.
            constexpr VenueSlot kBinanceSlot = static_cast<VenueSlot>(static_cast<size_t>(VenueId::BINANCE));
            quotes[VenueSlotIndex(kBinanceSlot)] = quote;
            UpdateBBOWithQuote(bbo, quote, kBinanceSlot, quotes);
            return bbo.best_bid.price;
        });

        bench.Measure("bbo_fullscan", [&] {
            BBO scanned = ComputeBBOFromQuotes(quotes);
            return scanned.best_bid.price + scanned.best_ask.price;
        });
    }

    // ---- 6. bytes moved, flat book only ------------------------------------
    std::printf("\nBytes memmoved per diff message  (FlatOrderBook only)\n");
    {
        const MovedStats qty_stats = SummariseMoved(moved_qty_deep);
        const MovedStats churn_stats = SummariseMoved(moved_churn);
        const MovedStats churn_top_stats = SummariseMoved(moved_churn_top);
        PrintMoved("flat_qty_update_50", qty_stats);
        PrintMoved("flat_churn_20x2", churn_stats);
        PrintMoved("flat_churn_top_20x2", churn_top_stats);
    }
    std::printf(
        "  The two churn rows are the same 20 erases + 20 inserts at different\n"
        "  DEPTHS: offset 500 vs offset 1. The flat book rewrites everything\n"
        "  above the deepest level a delta touches, so depth is what sets its\n"
        "  cost. Both are reported so neither the worst case nor the best case\n"
        "  stands alone.\n"
        "  Counted bytes are the WRITE-BACK only - staging costs an equal pass\n"
        "  again, so true traffic is twice these numbers.\n");

    std::printf(
        "\nNOTE: a sequential scan over the flat book's contiguous memory is\n"
        "prefetchable whether it starts cold or hot, so the merge/traversal numbers\n"
        "here are close to the production floor. Multiply by the measured message\n"
        "rate before drawing any conclusion. The historical std::map comparison -\n"
        "which showed the map ~11x slower on top-of-book churn and understated even\n"
        "that because the loop keeps tree nodes hot - was recorded during development.\n");

    return 0;
}
