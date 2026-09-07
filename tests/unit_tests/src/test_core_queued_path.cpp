#include <gtest/gtest.h>

#include "md_core/md_core.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "md_core/consolidated_bbo.h"
#include "md_core/consolidated_book.h"
#include "md_core/provider_message.h"
#include "md_core/types.h"
#include "md_core/venue_health.h"
#include "types/venue.h"
#include "types/venue_registry.h"

using namespace market_data;

namespace {

constexpr InstrumentKey kInstrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
constexpr PriceTicks kBid = 100'000'000'000;
constexpr PriceTicks kAsk = 100'100'000'000;
constexpr QtyUnits kQty = 100'000'000;
constexpr int64_t kMono = 987'654'321'000'000;

BookUpdate MakeSnapshot(VenueId venue, PriceTicks bid, PriceTicks ask, uint64_t seq = 1) {
    BookUpdate update(venue, kInstrument, /*reserve_levels=*/1, /*is_snapshot=*/true, seq);
    update.recv_mono_ns = kMono;
    update.bids.push_back({bid, kQty});
    update.asks.push_back({ask, kQty});
    return update;
}

BboQuote MakeQuote(VenueId venue, PriceTicks bid, PriceTicks ask) {
    BboQuote quote{};
    quote.venue = venue;
    quote.instrument = kInstrument;
    quote.seq = 1;
    quote.recv_mono_ns = kMono;
    quote.bid_price = bid;
    quote.bid_qty = kQty;
    quote.ask_price = ask;
    quote.ask_qty = kQty;
    return quote;
}

VenueHealthEvent MakeHealth(VenueId venue, StreamKind stream, VenueHealth health) {
    return VenueHealthEvent{.venue = venue, .stream = stream, .health = health, .decided_mono_ns = kMono};
}

// Registration order below is BINANCE, BYBIT, OKX, so slot index and venue
// line up - spelled out rather than assumed, since forbids depending
// on slot == VenueId anywhere.
VenueId VenueForIndex(size_t index) {
    switch (index) {
        case 0:
            return VenueId::BINANCE;
        case 1:
            return VenueId::BYBIT;
        default:
            return VenueId::OKX;
    }
}

// Captures what Core published, so both paths can be compared on observable
// output rather than on internals.
struct Recorder {
    consolidated::BBO last_bbo;

    // A COPY, not a reference. Core publishes its live consolidated book and
    // resumes mutating it the moment the callback returns (md_core.h), and this
    // test compares a book captured from the direct path against one captured
    // from the queued path - so both must still exist afterwards. Copying here
    // is the contract working as intended, not a workaround.
    consolidated::Book last_book;
    // Atomic because Core::Start() runs the consolidator on its own thread: the
    // BboSink/BookSink lambdas increment these from that thread while the test
    // body polls them through WaitFor() on the main thread (tests #237, #238).
    // A relaxed int here is the data race TSan flags. The seq_cst RMW also
    // publishes the last_bbo/last_book writes that precede it, so a WaitFor that
    // observes the count can safely read the payload afterwards.
    std::atomic<int> bbo_publishes = 0;
    std::atomic<int> book_publishes = 0;

    Core::BboCallback BboSink() {
        return [this](InstrumentKey, const consolidated::BBO& bbo) {
            last_bbo = bbo;
            ++bbo_publishes;
        };
    }

    Core::BookCallback BookSink() {
        return [this](InstrumentKey, const consolidated::Book& book) {
            last_book = book;
            ++book_publishes;
        };
    }
};

void InitAndRegister(Core& core, const std::vector<VenueId>& venues, std::vector<VenueSlot>& slots_out) {
    const CoreConfig config = {.venues = {}, .default_instruments = {kInstrument}};
    core.Init(config);
    for (VenueId venue : venues) {
        const std::optional<VenueSlot> slot = core.RegisterVenue(VenueConverter::ToVenueString(venue));
        ASSERT_TRUE(slot.has_value());
        slots_out.push_back(*slot);
    }
}

}  // namespace

// --- the equivalence property -----------------------------------------------

// THE test for this step. The synchronous path is the oracle for the queued
// one, the same way the std::map book is the oracle for any faster book: two
// implementations, identical input, identical observable output.
//
// this is what makes "the two paths cannot diverge" a checked claim
// rather than a comment. ProcessUpdate/ProcessQuote/ProcessHealth exist once
// and are reached both ways, so a divergence here would mean the ENTRY
// points differ - wrong slot, wrong order, a dropped message - which is
// exactly the class of bug that would otherwise surface as one venue's
// prices published under another's name.
TEST(CoreQueuedPathTest, QueuedPathMatchesSynchronousPath) {
    Recorder sync_recorder;
    Recorder queued_recorder;
    Core sync_core(sync_recorder.BboSink(), sync_recorder.BookSink());
    Core queued_core(queued_recorder.BboSink(), queued_recorder.BookSink());

    const std::vector<VenueId> venues = {VenueId::BINANCE, VenueId::BYBIT, VenueId::OKX};
    std::vector<VenueSlot> sync_slots;
    std::vector<VenueSlot> queued_slots;
    InitAndRegister(sync_core, venues, sync_slots);
    InitAndRegister(queued_core, venues, queued_slots);

    // Same sequence, same order, both paths. Deliberately mixes all three
    // message kinds so ordering between them is part of what is compared.
    for (size_t i = 0; i < venues.size(); ++i) {
        const VenueId venue = venues[i];
        const PriceTicks bid = kBid + static_cast<PriceTicks>(i) * 1'000'000;

        sync_core.OnVenueHealth(MakeHealth(venue, StreamKind::kDepth, VenueHealth::kLive));
        sync_core.OnVenueHealth(MakeHealth(venue, StreamKind::kBbo, VenueHealth::kLive));
        sync_core.ApplyUpdate(MakeSnapshot(venue, bid, kAsk));
        sync_core.ApplyQuote(MakeQuote(venue, bid, kAsk));

        EXPECT_TRUE(
            queued_core.EnqueueHealth(queued_slots[i], MakeHealth(venue, StreamKind::kDepth, VenueHealth::kLive)));
        EXPECT_TRUE(
            queued_core.EnqueueHealth(queued_slots[i], MakeHealth(venue, StreamKind::kBbo, VenueHealth::kLive)));
        EXPECT_TRUE(queued_core.EnqueueUpdate(queued_slots[i], MakeSnapshot(venue, bid, kAsk)));
        queued_core.EnqueueQuote(queued_slots[i], MakeQuote(venue, bid, kAsk));
    }

    queued_core.DrainOnce();

    ASSERT_GT(sync_recorder.book_publishes, 0) << "nothing was published";
    ASSERT_GT(queued_recorder.book_publishes, 0) << "nothing was published";

    const consolidated::Book& expected = sync_recorder.last_book;
    const consolidated::Book& actual = queued_recorder.last_book;

    ASSERT_EQ(actual.bids.size(), expected.bids.size());
    ASSERT_EQ(actual.asks.size(), expected.asks.size());

    for (size_t level = 0; level < expected.bids.size(); ++level) {
        const consolidated::MergedLevel& want = expected.bids[level];
        const consolidated::MergedLevel& got = actual.bids[level];
        EXPECT_EQ(got.price, want.price) << "bid price differs at level " << level;
        EXPECT_EQ(got.qty, want.qty) << "bid qty differs at level " << level;

        // Attribution is the part that would silently misreport a venue.
        //
        // Read through Venues(), never the raw array: entries at or beyond
        // venue_count are deliberately left uninitialized, so indexing past the
        // count is undefined behaviour rather than a read of zeros
        // (consolidated_book.h).
        const auto got_venues = got.Venues();
        const auto want_venues = want.Venues();
        ASSERT_EQ(got_venues.size(), want_venues.size()) << "bid attribution count differs at level " << level;
        for (size_t v = 0; v < want_venues.size(); ++v) {
            EXPECT_EQ(got_venues[v].slot, want_venues[v].slot) << "bid attribution slot differs at level " << level;
            EXPECT_EQ(got_venues[v].qty, want_venues[v].qty) << "bid attribution qty differs at level " << level;
        }
    }

    for (size_t level = 0; level < expected.asks.size(); ++level) {
        EXPECT_EQ(actual.asks[level].price, expected.asks[level].price) << "ask price differs at level " << level;
        EXPECT_EQ(actual.asks[level].qty, expected.asks[level].qty) << "ask qty differs at level " << level;
    }

    EXPECT_EQ(queued_recorder.last_bbo.best_bid.price, sync_recorder.last_bbo.best_bid.price);
    EXPECT_EQ(queued_recorder.last_bbo.best_ask.price, sync_recorder.last_bbo.best_ask.price);
    EXPECT_EQ(queued_recorder.last_bbo.best_bid.total_qty, sync_recorder.last_bbo.best_bid.total_qty);
}

// --- the path is genuinely asynchronous -------------------------------------

// The reason the methods are called Enqueue* and not Apply*: when this
// returns, nothing has happened yet. A caller that assumed otherwise would
// read a book that has not been updated, with no error anywhere.
TEST(CoreQueuedPathTest, NothingIsAppliedUntilDrain) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);

    ASSERT_TRUE(core.EnqueueHealth(slots[0], MakeHealth(VenueId::BINANCE, StreamKind::kDepth, VenueHealth::kLive)));
    ASSERT_TRUE(core.EnqueueUpdate(slots[0], MakeSnapshot(VenueId::BINANCE, kBid, kAsk)));

    EXPECT_EQ(recorder.book_publishes, 0) << "enqueue must not apply anything by itself";

    EXPECT_EQ(core.DrainOnce(), 2u);
    EXPECT_EQ(recorder.book_publishes, 1);
}

TEST(CoreQueuedPathTest, DrainOnceReturnsMessageCountAndEmptiesQueues) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);

    ASSERT_TRUE(core.EnqueueHealth(slots[0], MakeHealth(VenueId::BINANCE, StreamKind::kDepth, VenueHealth::kLive)));
    ASSERT_TRUE(core.EnqueueUpdate(slots[0], MakeSnapshot(VenueId::BINANCE, kBid, kAsk)));
    core.EnqueueQuote(slots[0], MakeQuote(VenueId::BINANCE, kBid, kAsk));

    EXPECT_EQ(core.DrainOnce(), 3u);
    EXPECT_EQ(core.DrainOnce(), 0u) << "a second drain has nothing left to do";
}

// --- ordering, at Core level rather than queue level ------------------------

// The design property from Core::OnVenueHealth's comment, checked through
// the queue: a venue's verdict must take effect against ITS OWN updates in
// the order the provider produced them. Split into per-kind queues, the
// kStale could be drained before the update that preceded it, and the venue
// would be excluded one update too early - or after, and excluded too late.
TEST(CoreQueuedPathTest, HealthEventOrderedAgainstItsVenuesUpdates) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE, VenueId::BYBIT}, slots);

    for (size_t i = 0; i < slots.size(); ++i) {
        const VenueId venue = i == 0 ? VenueId::BINANCE : VenueId::BYBIT;
        ASSERT_TRUE(core.EnqueueHealth(slots[i], MakeHealth(venue, StreamKind::kDepth, VenueHealth::kLive)));
        ASSERT_TRUE(core.EnqueueUpdate(slots[i], MakeSnapshot(venue, kBid, kAsk)));
    }
    core.DrainOnce();
    ASSERT_GT(recorder.book_publishes, 0) << "nothing was published";
    ASSERT_FALSE(recorder.last_book.bids.empty());
    const uint8_t venues_while_both_live = recorder.last_book.bids.front().venue_count;
    ASSERT_EQ(venues_while_both_live, 2) << "both venues should contribute before either goes stale";

    // BINANCE goes stale, then sends one more update. After the drain the
    // stale verdict must already be in force, so its update contributes
    // nothing to the merge.
    ASSERT_TRUE(core.EnqueueHealth(slots[0], MakeHealth(VenueId::BINANCE, StreamKind::kDepth, VenueHealth::kStale)));
    ASSERT_TRUE(core.EnqueueUpdate(slots[0], MakeSnapshot(VenueId::BINANCE, kBid, kAsk, /*seq=*/2)));
    core.DrainOnce();

    ASSERT_FALSE(recorder.last_book.bids.empty());
    EXPECT_EQ(recorder.last_book.bids.front().venue_count, 1) << "a venue marked stale must leave the merge";
    EXPECT_EQ(recorder.last_book.bids.front().venues[0].slot, slots[1]) << "the surviving venue must be BYBIT";
}

TEST(CoreQueuedPathTest, MultipleVenuesAllDrainInOnePass) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE, VenueId::BYBIT, VenueId::OKX}, slots);

    for (size_t i = 0; i < slots.size(); ++i) {
        const VenueId venue = VenueForIndex(i);
        ASSERT_TRUE(core.EnqueueHealth(slots[i], MakeHealth(venue, StreamKind::kDepth, VenueHealth::kLive)));
        ASSERT_TRUE(core.EnqueueUpdate(slots[i], MakeSnapshot(venue, kBid, kAsk)));
    }

    EXPECT_EQ(core.DrainOnce(), 6u) << "one pass must drain every venue, not just the first";
    ASSERT_GT(recorder.book_publishes, 0) << "nothing was published";
    EXPECT_EQ(recorder.last_book.bids.front().venue_count, 3);
}

// --- overflow policy --------------------------------------------------------

// Depth overflow is NOT silently absorbed: the caller is told, because only
// the provider can resync. Nothing drains here, so the queue fills.
TEST(CoreQueuedPathTest, DepthOverflowReportsFailureAndCounts) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);

    size_t accepted = 0;
    bool rejected = false;
    for (size_t i = 0; i < kProviderQueueCapacity + 8; ++i) {
        if (core.EnqueueUpdate(slots[0], MakeSnapshot(VenueId::BINANCE, kBid, kAsk, /*seq=*/i + 1))) {
            ++accepted;
        } else {
            rejected = true;
            break;
        }
    }

    EXPECT_TRUE(rejected) << "a queue with nothing draining it must eventually refuse";
    EXPECT_EQ(accepted, kProviderQueueCapacity);
    EXPECT_GT(core.OverflowCount(slots[0]), 0u) << "overflow must be observable, not just returned";
    EXPECT_EQ(core.QuoteDropCount(slots[0]), 0u) << "no quotes were sent, so none can be dropped";
}

// Quote overflow IS absorbed - a quote is a complete snapshot, so the next
// one supersedes it. The caller is not told, because there is nothing it
// should do; the counter is the record.
TEST(CoreQueuedPathTest, QuoteOverflowDropsAndCountsWithoutFailing) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);

    for (size_t i = 0; i < kProviderQueueCapacity + 4; ++i) {
        core.EnqueueQuote(slots[0], MakeQuote(VenueId::BINANCE, kBid, kAsk));
    }

    EXPECT_EQ(core.QuoteDropCount(slots[0]), 4u) << "every quote past capacity is conflated away";
    EXPECT_EQ(core.OverflowCount(slots[0]), 0u) << "a dropped quote is not an overflow requiring resync";
}

// --- the consolidator thread ------------------------------------------------

namespace {

// Polls until `predicate` holds or the deadline passes. Returns false on
// timeout so the test fails with its own message rather than hanging.
template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

}  // namespace

// The point of Start(): a producer enqueues and the work happens on the
// consolidator thread, with no drain call from anyone.
TEST(CoreQueuedPathTest, StartedConsolidatorAppliesWithoutAnExplicitDrain) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);

    core.Start();

    ASSERT_TRUE(core.EnqueueHealth(slots[0], MakeHealth(VenueId::BINANCE, StreamKind::kDepth, VenueHealth::kLive)));
    ASSERT_TRUE(core.EnqueueUpdate(slots[0], MakeSnapshot(VenueId::BINANCE, kBid, kAsk)));

    EXPECT_TRUE(WaitFor([&recorder] { return recorder.book_publishes > 0; }))
        << "the consolidator thread must apply queued messages on its own";

    core.Stop();
    ASSERT_GT(recorder.book_publishes, 0) << "nothing was published";
    EXPECT_EQ(recorder.last_book.bids.front().price, kBid);
}

// The doorbell has to work repeatedly, not just once - a lost wakeup after
// the first sleep would leave every later message stranded until the 50 ms
// backstop, or forever if the backstop were removed.
TEST(CoreQueuedPathTest, ConsolidatorWakesRepeatedlyAcrossIdlePeriods) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);
    core.Start();

    ASSERT_TRUE(core.EnqueueHealth(slots[0], MakeHealth(VenueId::BINANCE, StreamKind::kDepth, VenueHealth::kLive)));

    for (int round = 1; round <= 5; ++round) {
        // Long enough that the consolidator has certainly gone back to sleep
        // between rounds, so each iteration tests a fresh wakeup.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ASSERT_TRUE(core.EnqueueUpdate(
            slots[0], MakeSnapshot(VenueId::BINANCE, kBid + round * 1'000'000, kAsk, /*seq=*/round + 1)));

        EXPECT_TRUE(WaitFor([&recorder, round] { return recorder.book_publishes >= round; }))
            << "consolidator did not wake for round " << round;
    }

    core.Stop();
}

// Shutdown must not silently discard what the providers already handed over.
TEST(CoreQueuedPathTest, StopDrainsWhatIsStillQueued) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);

    // Enqueued BEFORE Start(), so nothing has drained them yet.
    ASSERT_TRUE(core.EnqueueHealth(slots[0], MakeHealth(VenueId::BINANCE, StreamKind::kDepth, VenueHealth::kLive)));
    ASSERT_TRUE(core.EnqueueUpdate(slots[0], MakeSnapshot(VenueId::BINANCE, kBid, kAsk)));

    core.Start();
    core.Stop();  // may run before the loop ever drains - the final drain covers it

    EXPECT_GT(recorder.book_publishes, 0) << "Stop() must drain, not discard";
}

TEST(CoreQueuedPathTest, StartAndStopAreIdempotentAndSafeInAnyOrder) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);

    core.Stop();  // never started
    core.Start();
    core.Start();  // must not spawn a second consolidator
    core.Stop();
    core.Stop();
    SUCCEED();
}

// A joinable std::thread whose owner is destroyed calls std::terminate. The
// destructor has to join, and it has to work without an explicit Stop().
TEST(CoreQueuedPathTest, DestructorJoinsARunningConsolidator) {
    Recorder recorder;
    {
        Core core(recorder.BboSink(), recorder.BookSink());
        std::vector<VenueSlot> slots;
        InitAndRegister(core, {VenueId::BINANCE}, slots);
        core.Start();
        ASSERT_TRUE(core.EnqueueHealth(slots[0], MakeHealth(VenueId::BINANCE, StreamKind::kDepth, VenueHealth::kLive)));
        ASSERT_TRUE(core.EnqueueUpdate(slots[0], MakeSnapshot(VenueId::BINANCE, kBid, kAsk)));
        // No Stop() - the destructor must handle it.
    }
    SUCCEED();
}

TEST(CoreQueuedPathTest, EnqueueToOutOfRangeSlotIsRefusedNotCrashed) {
    Recorder recorder;
    Core core(recorder.BboSink(), recorder.BookSink());
    std::vector<VenueSlot> slots;
    InitAndRegister(core, {VenueId::BINANCE}, slots);

    // Fabricated - the registry never issues a slot at or past kMaxVenues.
    const VenueSlot bogus = static_cast<VenueSlot>(kMaxVenues + 1);

    EXPECT_FALSE(core.EnqueueUpdate(bogus, MakeSnapshot(VenueId::BINANCE, kBid, kAsk)));
    EXPECT_FALSE(core.EnqueueHealth(bogus, MakeHealth(VenueId::BINANCE, StreamKind::kDepth, VenueHealth::kLive)));
    core.EnqueueQuote(bogus, MakeQuote(VenueId::BINANCE, kBid, kAsk));

    // The real slot must be untouched by any of that.
    EXPECT_EQ(core.DrainOnce(), 0u);
    EXPECT_EQ(core.OverflowCount(slots[0]), 0u);
    EXPECT_EQ(core.QuoteDropCount(slots[0]), 0u);
}
