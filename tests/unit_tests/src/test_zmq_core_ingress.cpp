#include <gtest/gtest.h>

#include <zmq.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "md_core/md_core.h"
#include "md_core/types.h"
#include "md_wire/wire_codec.h"
#include "md_wire/zmq_core_ingress.h"
#include "md_wire/zmq_provider_sink.h"

using namespace market_data;
using namespace market_data::wire;
using namespace std::chrono_literals;

namespace {

constexpr InstrumentKey kSpotBtc = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
constexpr InstrumentKey kFuturesBtc = MakeKey(InstrumentId::BTCUSDT, MarketType::kFutures);

std::string TestEndpoint(std::string_view name) {
    return "ipc:///tmp/zmq_core_ingress_test_" + std::string(name) + ".ipc";
}

std::string PathOf(const std::string& endpoint) {
    return endpoint.substr(std::string_view("ipc://").size());
}

// Spins a predicate for up to `timeout`. Tests here cross a real socket and a
// real consolidator thread, so the observable state lands asynchronously - a
// deadline poll is the deterministic way to wait for it without a fixed sleep.
template <typename Fn>
bool PollUntil(Fn predicate, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

// A Core plus the last book it published, captured under a lock because the
// callback fires on the consolidator thread.
struct CoreHarness {
    std::mutex mu;

    // A COPY, taken INSIDE the callback and under the mutex. Core hands out a
    // reference to its live book, and this harness reads it from the test
    // thread while the consolidator keeps running - so holding the reference
    // would be a data race on levels being mutated. Copying inside the callback
    // is what makes the cross-thread read safe (md_core.h).
    consolidated::Book last_book;
    consolidated::BBO last_bbo;
    std::atomic<int> book_publishes{0};
    std::atomic<int> bbo_publishes{0};

    Core core;

    CoreHarness()
        : core(
              [this](InstrumentKey, const consolidated::BBO& bbo) {
                  {
                      std::lock_guard<std::mutex> lock(mu);
                      last_bbo = bbo;
                  }
                  bbo_publishes.fetch_add(1, std::memory_order_relaxed);
              },
              [this](InstrumentKey, const consolidated::Book& book) {
                  {
                      std::lock_guard<std::mutex> lock(mu);
                      last_book = book;
                  }
                  book_publishes.fetch_add(1, std::memory_order_relaxed);
              }) {
        CoreConfig config;
        config.default_instruments = {kSpotBtc};
        core.Init(config);
        core.Start();
    }

    ~CoreHarness() { core.Stop(); }

    consolidated::Book Book() {
        std::lock_guard<std::mutex> lock(mu);
        return last_book;
    }

    consolidated::BBO Bbo() {
        std::lock_guard<std::mutex> lock(mu);
        return last_bbo;
    }
};

// Minimal provider-side peer: a raw DEALER that speaks the wire directly, so
// these tests exercise the ingress against the wire format rather than against
// ZmqProviderSink.
class DealerPeer {
   public:
    DealerPeer(zmq::context_t& ctx, const std::string& endpoint) : dealer_(ctx, zmq::socket_type::dealer) {
        dealer_.set(zmq::sockopt::linger, 0);
        dealer_.connect(endpoint);
    }

    void SendHello(InstrumentKey instrument, std::string_view venue) {
        EncodeHello(instrument, venue, buf_);
        Send();
    }
    void SendUpdate(const BookUpdate& update) {
        EncodeBookUpdate(update, buf_);
        Send();
    }
    // A real provider promotes its stream out of kNoData on its first message.
    // Without this the merge admits nothing (only kLive venues contribute).
    void SendDepthLive() {
        VenueHealthEvent event{};
        event.venue = VenueId::BINANCE;  // not carried on the wire
        event.stream = StreamKind::kDepth;
        event.health = VenueHealth::kLive;
        event.decided_mono_ns = 0;
        EncodeHealth(event, buf_);
        Send();
    }
    void Disconnect() { dealer_.close(); }

   private:
    void Send() {
        zmq::message_t msg(buf_.data(), buf_.size());
        dealer_.send(msg, zmq::send_flags::none);
    }

    zmq::socket_t dealer_;
    std::vector<std::byte> buf_;
};

BookUpdate SnapshotWithBid(std::string_view /*venue label, ignored on wire*/, PriceTicks price, QtyUnits qty) {
    BookUpdate update(VenueId::BINANCE, kSpotBtc, /*reserve_levels=*/1, /*is_snapshot=*/true, /*seq=*/1);
    update.bids = {{price, qty}};
    return update;
}

// A venue's stream is kNoData until its first health verdict, and only kLive
// venues contribute (IsAdmissible). Depth for the merge, kBbo for the
// published BBO - they are gated independently.
VenueHealthEvent LiveHealth(StreamKind stream) {
    VenueHealthEvent event{};
    event.venue = VenueId::BINANCE;  // not carried on the wire
    event.stream = stream;
    event.health = VenueHealth::kLive;
    event.decided_mono_ns = 0;
    return event;
}

}  // namespace

TEST(ZmqCoreIngressTest, HelloRegistersVenueAndUpdateReachesTheBook) {
    const std::string endpoint = TestEndpoint("hello_update");
    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();

    zmq::context_t peer_ctx(1);
    DealerPeer peer(peer_ctx, endpoint);
    peer.SendHello(kSpotBtc, "BINANCE");

    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 1; }));
    EXPECT_EQ(h.core.VenueName(static_cast<VenueSlot>(0)), "BINANCE");

    peer.SendDepthLive();
    peer.SendUpdate(SnapshotWithBid("BINANCE", /*price=*/100, /*qty=*/5));

    ASSERT_TRUE(PollUntil([&] {
        const consolidated::Book book = h.Book();
        return !book.bids.empty();
    }));
    EXPECT_EQ(h.Book().bids[0].price, 100);

    ingress.Stop();
}

TEST(ZmqCoreIngressTest, UpdateBeforeHelloIsDroppedAndCounted) {
    const std::string endpoint = TestEndpoint("no_hello");
    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();

    zmq::context_t peer_ctx(1);
    DealerPeer peer(peer_ctx, endpoint);
    peer.SendUpdate(SnapshotWithBid("BINANCE", 100, 5));

    ASSERT_TRUE(PollUntil([&] { return ingress.DataBeforeHelloCount() >= 1; }));
    EXPECT_EQ(h.core.venue_count(), 0u);
    EXPECT_EQ(h.book_publishes.load(), 0);

    ingress.Stop();
}

TEST(ZmqCoreIngressTest, WrongMarketTypeIsRefused) {
    const std::string endpoint = TestEndpoint("wrong_market");
    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();

    zmq::context_t peer_ctx(1);
    DealerPeer peer(peer_ctx, endpoint);
    peer.SendHello(kFuturesBtc, "BINANCE");  // futures BTCUSDT - different book

    // Give the ingress time to process and reject it.
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(h.core.venue_count(), 0u);

    ingress.Stop();
}

TEST(ZmqCoreIngressTest, DistinctIdentitiesGetDistinctSlots) {
    const std::string endpoint = TestEndpoint("two_venues");
    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();

    zmq::context_t peer_ctx(1);
    DealerPeer binance(peer_ctx, endpoint);
    DealerPeer bybit(peer_ctx, endpoint);
    binance.SendHello(kSpotBtc, "BINANCE");
    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 1; }));
    bybit.SendHello(kSpotBtc, "BYBIT");
    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 2; }));

    EXPECT_EQ(h.core.VenueName(static_cast<VenueSlot>(0)), "BINANCE");
    EXPECT_EQ(h.core.VenueName(static_cast<VenueSlot>(1)), "BYBIT");

    ingress.Stop();
}

TEST(ZmqCoreIngressTest, DisconnectRemovesVenueFromTheMerge) {
    const std::string endpoint = TestEndpoint("disconnect");
    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();

    zmq::context_t peer_ctx(1);
    auto binance = std::make_unique<DealerPeer>(peer_ctx, endpoint);
    DealerPeer bybit(peer_ctx, endpoint);

    binance->SendHello(kSpotBtc, "BINANCE");
    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 1; }));
    bybit.SendHello(kSpotBtc, "BYBIT");
    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 2; }));

    binance->SendDepthLive();
    bybit.SendDepthLive();
    {
        BookUpdate u(VenueId::BINANCE, kSpotBtc, 1, true, 1);
        u.bids = {{100, 5}};
        binance->SendUpdate(u);
    }
    {
        BookUpdate u(VenueId::BINANCE, kSpotBtc, 1, true, 1);
        u.bids = {{90, 5}};
        bybit.SendUpdate(u);
    }
    ASSERT_TRUE(PollUntil([&] {
        auto book = h.Book();
        return book.bids.size() == 2;  // both venues' prices present
    }));

    // Kill BINANCE's connection - ROUTER_NOTIFY disconnect -> EnqueueDisconnect
    // -> RemoveVenue on the consolidator thread.
    binance->Disconnect();
    binance.reset();

    // RemoveVenue does not republish, so the removal only shows up on the NEXT
    // merge. Keep feeding BYBIT updates until one is merged after the removal
    // has landed and BINANCE's 100 bid is gone.
    uint64_t seq = 2;
    ASSERT_TRUE(PollUntil(
        [&] {
            BookUpdate u(VenueId::BINANCE, kSpotBtc, 1, true, seq++);
            u.bids = {{90, 7}};
            bybit.SendUpdate(u);
            auto book = h.Book();
            return book.bids.size() == 1 && book.bids[0].price == 90;
        },
        3000ms));

    ingress.Stop();
}

TEST(ZmqCoreIngressTest, UnlinksStaleSocketFileBeforeBind) {
    const std::string endpoint = TestEndpoint("stale_file");
    // Leave a regular file where the socket wants to be - bind() would fail
    // EADDRINUSE without the unlink() in Start().
    {
        std::ofstream stale(PathOf(endpoint));
        stale << "not a socket";
    }

    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();  // must not throw

    zmq::context_t peer_ctx(1);
    DealerPeer peer(peer_ctx, endpoint);
    peer.SendHello(kSpotBtc, "BINANCE");
    EXPECT_TRUE(PollUntil([&] { return h.core.venue_count() >= 1; }));

    ingress.Stop();
}

// ---------------------------------------------------------------------------
// The same ingress, now driven by the real ZmqProviderSink instead of the
// hand-rolled DealerPeer - so the sink's encode path and the ingress decode
// path are checked against each other over a real ipc:// socket, with a real
// Core and consolidator thread behind it. The sink runs on its own
// zmq::context_t, which is what a separate md_provider_app process looks like.
// ---------------------------------------------------------------------------

TEST(ZmqCoreIngressTest, RealSinkHelloAndSnapshotReachTheBook) {
    const std::string endpoint = TestEndpoint("real_sink_snapshot");
    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();

    zmq::context_t sink_ctx(1);
    ZmqProviderSink sink(sink_ctx, endpoint, kSpotBtc, "BINANCE");
    sink.SendHello();

    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 1; }));
    EXPECT_EQ(h.core.VenueName(static_cast<VenueSlot>(0)), "BINANCE");

    sink.OnHealth(LiveHealth(StreamKind::kDepth));
    sink.OnUpdate(SnapshotWithBid("BINANCE", /*price=*/100, /*qty=*/5));

    ASSERT_TRUE(PollUntil([&] {
        const consolidated::Book book = h.Book();
        return !book.bids.empty();
    }));
    EXPECT_EQ(h.Book().bids[0].price, 100);

    ingress.Stop();
}

TEST(ZmqCoreIngressTest, RealSinkQuoteReachesTheConsolidatedBbo) {
    const std::string endpoint = TestEndpoint("real_sink_quote");
    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();

    zmq::context_t sink_ctx(1);
    ZmqProviderSink sink(sink_ctx, endpoint, kSpotBtc, "BINANCE");
    sink.SendHello();
    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 1; }));

    // kBbo health gates the published BBO independently of the depth book.
    sink.OnHealth(LiveHealth(StreamKind::kBbo));

    BboQuote quote{};
    quote.venue = VenueId::BINANCE;
    quote.instrument = kSpotBtc;
    quote.seq = 1;
    quote.bid_price = 100;
    quote.bid_qty = 5;
    quote.ask_price = 101;
    quote.ask_qty = 7;
    sink.OnQuote(quote);

    ASSERT_TRUE(PollUntil([&] {
        auto bbo = h.Bbo();
        return bbo.best_bid.price == 100 && bbo.best_ask.price == 101;
    }));

    ingress.Stop();
}

TEST(ZmqCoreIngressTest, RealSinkGoodbyeDoesNotRemoveButDisconnectDoes) {
    const std::string endpoint = TestEndpoint("real_sink_goodbye");
    CoreHarness h;
    ZmqCoreIngress ingress(endpoint, h.core, kSpotBtc);
    ingress.Start();

    auto binance_ctx = std::make_unique<zmq::context_t>(1);
    auto binance = std::make_unique<ZmqProviderSink>(*binance_ctx, endpoint, kSpotBtc, "BINANCE");
    zmq::context_t bybit_ctx(1);
    ZmqProviderSink bybit(bybit_ctx, endpoint, kSpotBtc, "BYBIT");

    binance->SendHello();
    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 1; }));
    bybit.SendHello();
    ASSERT_TRUE(PollUntil([&] { return h.core.venue_count() >= 2; }));

    binance->OnHealth(LiveHealth(StreamKind::kDepth));
    bybit.OnHealth(LiveHealth(StreamKind::kDepth));
    binance->OnUpdate(SnapshotWithBid("BINANCE", /*price=*/100, /*qty=*/5));
    {
        BookUpdate u(VenueId::BINANCE, kSpotBtc, 1, true, 1);  // venue label ignored on wire
        u.bids = {{90, 5}};
        bybit.OnUpdate(std::move(u));
    }
    ASSERT_TRUE(PollUntil([&] {
        auto book = h.Book();
        return book.bids.size() == 2;
    }));

    // kGoodbye is a log line only: removal is the ROUTER_NOTIFY
    // disconnect, never the goodbye. The venue must still be in the merge.
    binance->SendGoodbye(GoodbyeReason::kShutdown);
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(h.core.venue_count(), 2u);
    ASSERT_TRUE(h.Book().bids.size() == 2);

    // Now actually drop the connection - close the socket, tear down its
    // context - and the disconnect notify removes the venue on the next merge.
    binance.reset();
    binance_ctx.reset();

    uint64_t seq = 2;
    ASSERT_TRUE(PollUntil(
        [&] {
            BookUpdate u(VenueId::BINANCE, kSpotBtc, 1, true, seq++);
            u.bids = {{90, 7}};
            bybit.OnUpdate(std::move(u));
            auto book = h.Book();
            return book.bids.size() == 1 && book.bids[0].price == 90;
        },
        3000ms));

    ingress.Stop();
}
