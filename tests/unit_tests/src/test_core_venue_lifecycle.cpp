#include <gtest/gtest.h>

#include "md_core/md_core.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "md_core/consolidated_bbo.h"
#include "md_core/consolidated_book.h"
#include "md_core/types.h"
#include "md_core/venue_health.h"
#include "types/venue.h"
#include "types/venue_registry.h"

using namespace market_data;

namespace {

constexpr InstrumentKey kInstrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);

// Prices are x1e8, so these are $1000 / $1001 etc. The absolute values do not
// matter; only their ORDER does, since every assertion below is about which
// venue wins the merge.
constexpr PriceTicks kBid = 100'000'000'000;
constexpr PriceTicks kAsk = 100'100'000'000;
constexpr QtyUnits kQty = 100'000'000;

// steady_clock's epoch is arbitrary, so a realistic stamp is a large number
// with no meaning of its own (same convention as test_venue_health.cpp).
constexpr int64_t kMono = 987'654'321'000'000;

// Drives Core through its PUBLIC surface only - Init, RegisterVenue,
// RemoveVenue, ApplyUpdate, ApplyQuote, OnVenueHealth - and observes the
// results through the two publish callbacks. Nothing here reaches into
// venue_books_ or the registry directly, so these tests hold regardless of how
// the internals are stored, which is the point while those internals are being
// migrated slot by slot.
class CoreVenueLifecycleTest : public ::testing::Test {
   protected:
    CoreVenueLifecycleTest()
        : core_(
              [this](InstrumentKey, const consolidated::BBO& bbo) {
                  last_bbo_ = bbo;
                  ++bbo_publishes_;
              },
              [this](InstrumentKey, const consolidated::Book& book) {
                  last_book_ = book;
                  ++book_publishes_;
              }) {}

    void InitWithInstrument() {
        const CoreConfig config = {
            .venues = {},  // Core registers no venues from config
            .default_instruments = {kInstrument},
        };
        core_.Init(config);
    }

    VenueSlot Register(VenueId venue) {
        const std::optional<VenueSlot> slot = core_.RegisterVenue(VenueConverter::ToVenueString(venue));
        EXPECT_TRUE(slot.has_value());
        return slot.value_or(static_cast<VenueSlot>(0));
    }

    // A venue contributes nothing until a provider affirms its feed is alive -
    // depth_health_ starts at kNoData and kNoData is not admissible. That is
    // the fail-safe default, so every test that expects data in the merge has
    // to say so explicitly, exactly as a real provider would.
    void MarkLive(VenueId venue, StreamKind stream) {
        core_.OnVenueHealth({.venue = venue, .stream = stream, .health = VenueHealth::kLive, .decided_mono_ns = kMono});
    }

    void ApplySnapshot(VenueId venue, PriceTicks bid, PriceTicks ask) {
        BookUpdate update(venue, kInstrument, /*reserve_levels=*/1, /*is_snapshot=*/true, /*seq=*/1);
        update.recv_mono_ns = kMono;
        update.bids.push_back({bid, kQty});
        update.asks.push_back({ask, kQty});
        core_.ApplyUpdate(update);
    }

    void ApplyQuote(VenueId venue, PriceTicks bid, PriceTicks ask) {
        BboQuote quote{};
        quote.venue = venue;
        quote.instrument = kInstrument;
        quote.seq = 1;
        quote.recv_mono_ns = kMono;
        quote.bid_price = bid;
        quote.bid_qty = kQty;
        quote.ask_price = ask;
        quote.ask_qty = kQty;
        core_.ApplyQuote(quote);
    }

    // Depth of that venue's OWN book, not what it contributed to the merge.
    // venue_levels is filled from venue_book->bids().size() regardless of
    // admission (md_core.cpp), so a STALE venue still reports its levels here
    // - staleness is reversible and the book is deliberately kept. Use this to
    // check a book exists at all; use TopBidSlots() to check what actually
    // entered the merge.
    uint32_t BookDepthAt(VenueSlot slot) const { return last_book_.venue_levels[VenueSlotIndex(slot)]; }

    // Which SLOTS the last published book attributes its best bid to.
    // Attribution carries a slot, not a VenueId; the name is
    // resolved at the wire boundary. Getting this wrong publishes one
    // exchange's price under another's name, so it is asserted directly.
    std::vector<VenueSlot> TopBidSlots() const {
        std::vector<VenueSlot> venues;
        if (!last_book_.bids.empty()) {
            const consolidated::MergedLevel& level = last_book_.bids.front();
            for (uint8_t i = 0; i < level.venue_count; ++i) {
                venues.push_back(level.venues[i].slot);
            }
        }
        return venues;
    }

    consolidated::BBO last_bbo_;

    // A COPY: Core publishes a reference to its live book and keeps mutating
    // it after the callback returns, so anything read later must be captured
    // here rather than pointed at (md_core.h).
    consolidated::Book last_book_;
    int bbo_publishes_ = 0;
    int book_publishes_ = 0;
    Core core_;
};

}  // namespace

// --- registration -----------------------------------------------------------

TEST_F(CoreVenueLifecycleTest, InitRegistersNoVenues) {
    InitWithInstrument();

    // a venue exists because a provider exists, not
    // because config named one. Init allocates capacity and nothing else.
    EXPECT_EQ(core_.venue_count(), 0u);
}

TEST_F(CoreVenueLifecycleTest, RegisterAssignsSlotsInOrder) {
    InitWithInstrument();

    EXPECT_EQ(VenueSlotIndex(Register(VenueId::BINANCE)), 0u);
    EXPECT_EQ(VenueSlotIndex(Register(VenueId::BYBIT)), 1u);
    EXPECT_EQ(VenueSlotIndex(Register(VenueId::OKX)), 2u);
    EXPECT_EQ(core_.venue_count(), 3u);
}

TEST_F(CoreVenueLifecycleTest, RegisterIsIdempotent) {
    InitWithInstrument();

    const VenueSlot first = Register(VenueId::BINANCE);
    const VenueSlot second = Register(VenueId::BINANCE);

    EXPECT_EQ(first, second);
    EXPECT_EQ(core_.venue_count(), 1u);
}

TEST_F(CoreVenueLifecycleTest, UnknownVenueNameIsRefused) {
    InitWithInstrument();

    // Refused rather than assigned a slot: the name arrives from the wiring
    // layer today and from a remote process later (kHello). Accepting
    // an unrecognised one would burn a slot on a venue nothing can ever feed.
    EXPECT_FALSE(core_.RegisterVenue("NOT_A_VENUE").has_value());
    EXPECT_EQ(core_.venue_count(), 0u);
}

// --- the two sync directions ------------------------------------------------

TEST_F(CoreVenueLifecycleTest, VenueRegisteredBeforeInitContributesToTheMerge) {
    const VenueSlot binance = Register(VenueId::BINANCE);
    InitWithInstrument();
    MarkLive(VenueId::BINANCE, StreamKind::kDepth);

    ApplySnapshot(VenueId::BINANCE, kBid, kAsk);

    ASSERT_EQ(book_publishes_, 1);
    EXPECT_EQ(BookDepthAt(binance), 1u);
}

TEST_F(CoreVenueLifecycleTest, VenueRegisteredAfterInitGetsBooksForExistingInstruments) {
    InitWithInstrument();

    // The second sync direction (see AddInstrument in md_core.h): the
    // instrument already exists, so RegisterVenue has to create this venue's
    // book for it. Without that, every update from a late-joining venue is
    // rejected as "unconfigured venue" - silently, with the process healthy.
    const VenueSlot bybit = Register(VenueId::BYBIT);
    MarkLive(VenueId::BYBIT, StreamKind::kDepth);

    ApplySnapshot(VenueId::BYBIT, kBid, kAsk);

    ASSERT_EQ(book_publishes_, 1);
    EXPECT_EQ(BookDepthAt(bybit), 1u);
}

// --- registration order is free ---------------------------------------------
//
// These are the tests the venue_id_to_slot_ table exists for. Before it, Core
// indexed every per-venue array by static_cast<size_t>(VenueId), so slot N had
// to equal VenueId N and RegisterVenue refused any other order. "Add a venue
// at runtime" then really meant "add the next venue in enum order", which is
// not a property anyone would want to defend.

TEST_F(CoreVenueLifecycleTest, SlotsFollowArrivalOrderNotEnumOrder) {
    InitWithInstrument();

    // OKX is VenueId 2 but registers first, so it takes slot 0.
    EXPECT_EQ(VenueSlotIndex(Register(VenueId::OKX)), 0u);
    EXPECT_EQ(VenueSlotIndex(Register(VenueId::BINANCE)), 1u);
    EXPECT_EQ(core_.venue_count(), 2u);
}

TEST_F(CoreVenueLifecycleTest, OutOfOrderRegistrationAttributesLevelsToTheRightVenue) {
    InitWithInstrument();
    const VenueSlot okx = Register(VenueId::OKX);          // slot 0
    const VenueSlot binance = Register(VenueId::BINANCE);  // slot 1
    MarkLive(VenueId::OKX, StreamKind::kDepth);
    MarkLive(VenueId::BINANCE, StreamKind::kDepth);

    // OKX alone at the best bid, BINANCE lower.
    ApplySnapshot(VenueId::BINANCE, kBid - 1'000'000, kAsk);
    ApplySnapshot(VenueId::OKX, kBid, kAsk);

    EXPECT_EQ(BookDepthAt(okx), 1u);
    EXPECT_EQ(BookDepthAt(binance), 1u);

    // The assertion that matters: OKX owns the top level, and it is attributed
    // to OKX's slot (0) rather than to whatever slot its VenueId would suggest
    // (2). The wire boundary resolves slot 0 back to "OKX" via the same
    // registry that assigned it, so the name a client sees follows from this.
    ASSERT_EQ(TopBidSlots().size(), 1u);
    EXPECT_EQ(TopBidSlots().front(), okx);
}

TEST_F(CoreVenueLifecycleTest, OutOfOrderRegistrationRoutesHealthToTheRightSlot) {
    InitWithInstrument();
    const VenueSlot okx = Register(VenueId::OKX);          // slot 0
    const VenueSlot binance = Register(VenueId::BINANCE);  // slot 1
    MarkLive(VenueId::OKX, StreamKind::kDepth);
    MarkLive(VenueId::BINANCE, StreamKind::kDepth);

    // OKX owns the best bid; BINANCE sits one tick below it.
    ApplySnapshot(VenueId::BINANCE, kBid - 1'000'000, kAsk);
    ApplySnapshot(VenueId::OKX, kBid, kAsk);
    ASSERT_EQ(TopBidSlots().size(), 1u);
    ASSERT_EQ(TopBidSlots().front(), okx);

    // Mark OKX stale. Indexing the health array by VenueId would write slot 2
    // - nobody's verdict - leaving OKX admitted and a stale venue owning the
    // top of book, which is the failure exists to prevent.
    core_.OnVenueHealth(
        {.venue = VenueId::OKX, .stream = StreamKind::kDepth, .health = VenueHealth::kStale, .decided_mono_ns = kMono});
    ApplySnapshot(VenueId::BINANCE, kBid - 1'000'000, kAsk);

    ASSERT_EQ(TopBidSlots().size(), 1u);
    EXPECT_EQ(TopBidSlots().front(), binance) << "a stale venue must leave the merge";

    // Its BOOK is deliberately kept - staleness is reversible, and freeing it
    // would force a full REST resync when the venue recovers. Only removal
    // frees a book.
    EXPECT_EQ(BookDepthAt(okx), 1u);
    EXPECT_EQ(BookDepthAt(binance), 1u);
}

TEST_F(CoreVenueLifecycleTest, OutOfOrderRegistrationAttributesTheBboToTheRightVenue) {
    InitWithInstrument();
    const VenueSlot okx = Register(VenueId::OKX);  // slot 0
    Register(VenueId::BINANCE);                    // slot 1
    MarkLive(VenueId::OKX, StreamKind::kBbo);
    MarkLive(VenueId::BINANCE, StreamKind::kBbo);

    ApplyQuote(VenueId::BINANCE, kBid - 1'000'000, kAsk);
    ApplyQuote(VenueId::OKX, kBid, kAsk);

    ASSERT_EQ(last_bbo_.best_bid.price, kBid);
    ASSERT_EQ(last_bbo_.best_bid.venues.size(), 1u);
    EXPECT_EQ(last_bbo_.best_bid.venues.front().slot, okx);
}

TEST_F(CoreVenueLifecycleTest, UpdateFromAnUnregisteredVenueIsDropped) {
    InitWithInstrument();
    const VenueSlot okx = Register(VenueId::OKX);
    MarkLive(VenueId::OKX, StreamKind::kDepth);
    ApplySnapshot(VenueId::OKX, kBid, kAsk);
    const int publishes_before = book_publishes_;

    // BYBIT never registered, so it has no slot. Before the translation table
    // this would have indexed slot 1 - which OKX may well occupy - and written
    // one venue's book with another venue's prices.
    ApplySnapshot(VenueId::BYBIT, kBid + 1'000'000, kAsk);
    ApplyQuote(VenueId::BYBIT, kBid + 1'000'000, kAsk);

    EXPECT_EQ(book_publishes_, publishes_before) << "a dropped update must not publish";
    EXPECT_EQ(core_.venue_count(), 1u);
    EXPECT_EQ(BookDepthAt(okx), 1u);
}

// --- removal ----------------------------------------------------------------

TEST_F(CoreVenueLifecycleTest, RemoveVenueStopsItContributingToTheMergedBook) {
    const VenueSlot binance = Register(VenueId::BINANCE);
    const VenueSlot bybit = Register(VenueId::BYBIT);
    InitWithInstrument();
    MarkLive(VenueId::BINANCE, StreamKind::kDepth);
    MarkLive(VenueId::BYBIT, StreamKind::kDepth);

    ApplySnapshot(VenueId::BINANCE, kBid, kAsk);
    ApplySnapshot(VenueId::BYBIT, kBid, kAsk);
    ASSERT_EQ(BookDepthAt(bybit), 1u);

    core_.RemoveVenue(bybit);

    // Republish from the surviving venue. The merge must now be one venue
    // thinner - which is already the correct behaviour for a venue that has
    // gone away, and the same thing the health path does.
    ApplySnapshot(VenueId::BINANCE, kBid, kAsk);
    EXPECT_EQ(BookDepthAt(binance), 1u);
    EXPECT_EQ(BookDepthAt(bybit), 0u);
}

TEST_F(CoreVenueLifecycleTest, RemoveVenueDoesNotDecrementVenueCount) {
    const VenueSlot binance = Register(VenueId::BINANCE);
    Register(VenueId::BYBIT);
    const VenueSlot okx = Register(VenueId::OKX);
    InitWithInstrument();
    ASSERT_EQ(core_.venue_count(), 3u);

    // Removing the MIDDLE slot is the case that matters. venue_count() is a
    // high-water mark: decrementing here would make every per-venue loop stop
    // at 1 and silently drop OKX, in slot 2, from the merge - while the
    // published book still looked perfectly well-formed.
    core_.RemoveVenue(static_cast<VenueSlot>(1));
    EXPECT_EQ(core_.venue_count(), 3u);

    MarkLive(VenueId::BINANCE, StreamKind::kDepth);
    MarkLive(VenueId::OKX, StreamKind::kDepth);
    ApplySnapshot(VenueId::BINANCE, kBid, kAsk);
    ApplySnapshot(VenueId::OKX, kBid, kAsk);

    EXPECT_EQ(BookDepthAt(binance), 1u);
    EXPECT_EQ(BookDepthAt(okx), 1u) << "slot 2 must survive the removal of slot 1";
}

TEST_F(CoreVenueLifecycleTest, RemoveVenueClearsItsPriceFromTheRunningBbo) {
    Register(VenueId::BINANCE);
    const VenueSlot bybit = Register(VenueId::BYBIT);
    InitWithInstrument();
    MarkLive(VenueId::BINANCE, StreamKind::kBbo);
    MarkLive(VenueId::BYBIT, StreamKind::kBbo);

    constexpr PriceTicks kBetterBid = kBid + 1'000'000;

    ApplyQuote(VenueId::BINANCE, kBid, kAsk);
    ApplyQuote(VenueId::BYBIT, kBetterBid, kAsk);
    ASSERT_EQ(last_bbo_.best_bid.price, kBetterBid) << "Bybit should own the best bid before removal";

    core_.RemoveVenue(bybit);

    // this is the assertion the removal path exists for. The consolidated
    // BBO is maintained INCREMENTALLY, so Bybit's price is already inside it
    // and Bybit will never send another quote to displace it. Removing the
    // venue has to clear the stored quote AND force the next quote onto the
    // full-rescan path; an incremental update alone cannot remove a price that
    // is already there.
    ApplyQuote(VenueId::BINANCE, kBid, kAsk);
    EXPECT_EQ(last_bbo_.best_bid.price, kBid) << "removed venue's price survived in the running BBO";
    for (const consolidated::VenueQuote& quote : last_bbo_.best_bid.venues) {
        EXPECT_NE(quote.slot, bybit);
    }
}

TEST_F(CoreVenueLifecycleTest, RemovedVenueUpdatesAreDroppedNotResurrected) {
    const VenueSlot binance = Register(VenueId::BINANCE);
    const VenueSlot bybit = Register(VenueId::BYBIT);
    InitWithInstrument();
    MarkLive(VenueId::BINANCE, StreamKind::kDepth);
    MarkLive(VenueId::BYBIT, StreamKind::kDepth);
    ApplySnapshot(VenueId::BINANCE, kBid, kAsk);

    core_.RemoveVenue(bybit);

    // Core never creates state from a data message. An update still in
    // flight when the venue was removed must be DROPPED - recreating the book
    // would leave a venue nobody registered and nobody will ever remove.
    const int publishes_before = book_publishes_;
    ApplySnapshot(VenueId::BYBIT, kBid, kAsk);
    EXPECT_EQ(book_publishes_, publishes_before) << "a dropped update must not publish";
    EXPECT_EQ(core_.venue_count(), 2u);

    ApplySnapshot(VenueId::BINANCE, kBid, kAsk);
    EXPECT_EQ(BookDepthAt(bybit), 0u);
    EXPECT_EQ(BookDepthAt(binance), 1u);
}

TEST_F(CoreVenueLifecycleTest, ReRegisteringAfterRemovalReusesTheSameSlot) {
    Register(VenueId::BINANCE);
    const VenueSlot bybit = Register(VenueId::BYBIT);
    InitWithInstrument();

    core_.RemoveVenue(bybit);

    // A crashed provider's replacement dials back in. It must land on
    // its original slot: the slot is what books, health and attribution are
    // keyed on, so a different one would strand the old book. Retaining the
    // name -> slot mapping through removal is what makes this hold.
    const VenueSlot again = Register(VenueId::BYBIT);
    EXPECT_EQ(again, bybit);
    EXPECT_EQ(core_.venue_count(), 2u);

    MarkLive(VenueId::BYBIT, StreamKind::kDepth);
    ApplySnapshot(VenueId::BYBIT, kBid, kAsk);
    EXPECT_EQ(BookDepthAt(bybit), 1u) << "re-registered venue should have a book again";
}

TEST_F(CoreVenueLifecycleTest, RemovingAnInactiveSlotIsIgnored) {
    Register(VenueId::BINANCE);
    InitWithInstrument();

    // Removal is driven by a socket closing, and sockets close more
    // than once - a crash followed by the supervisor's own teardown. A second
    // remove has to be a no-op rather than corrupting the slot that is there.
    core_.RemoveVenue(static_cast<VenueSlot>(0));
    core_.RemoveVenue(static_cast<VenueSlot>(0));
    core_.RemoveVenue(static_cast<VenueSlot>(kMaxVenues - 1));  // never registered
    EXPECT_EQ(core_.venue_count(), 1u);

    const VenueSlot binance = Register(VenueId::BINANCE);
    MarkLive(VenueId::BINANCE, StreamKind::kDepth);
    ApplySnapshot(VenueId::BINANCE, kBid, kAsk);
    EXPECT_EQ(BookDepthAt(binance), 1u);
}
