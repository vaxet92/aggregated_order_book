#include <gtest/gtest.h>

#include <zmq.hpp>
#include <zmq_addon.hpp>

#include "md_wire/wire_codec.h"
#include "md_wire/zmq_provider_sink.h"

using namespace market_data;
using namespace market_data::wire;

namespace {

constexpr InstrumentKey kInstrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);

// unique per-test endpoint so parallel/repeated runs never collide on one
// leftover socket file.
std::string TestEndpoint(std::string_view test_name) {
    return "ipc:///tmp/zmq_provider_sink_test_" + std::string(test_name) + ".ipc";
}

// Reads one multipart message off a bound ROUTER and returns just the
// payload frame (frame 1) - frame 0 is the identity, added/stripped by
// ROUTER, and irrelevant to what these tests check.
std::vector<std::byte> RecvPayload(zmq::socket_t& router) {
    zmq::multipart_t msg;
    EXPECT_TRUE(msg.recv(router));
    EXPECT_GE(msg.size(), 2u);
    zmq::message_t& payload = msg.at(1);
    const auto* bytes = static_cast<const std::byte*>(payload.data());
    return std::vector<std::byte>(bytes, bytes + payload.size());
}

}  // namespace

TEST(ZmqProviderSinkTest, HelloArrivesAndDecodes) {
    zmq::context_t ctx(1);
    std::string endpoint = TestEndpoint("hello");
    zmq::socket_t router(ctx, zmq::socket_type::router);
    router.bind(endpoint);

    ZmqProviderSink sink(ctx, endpoint, kInstrument, "OKX");
    sink.SendHello();

    InstrumentKey decoded_instrument{};
    std::string decoded_venue;
    ASSERT_TRUE(DecodeHello(RecvPayload(router), decoded_instrument, decoded_venue));
    EXPECT_EQ(decoded_instrument, kInstrument);
    EXPECT_EQ(decoded_venue, "OKX");
}

TEST(ZmqProviderSinkTest, GoodbyeArrivesAndDecodes) {
    zmq::context_t ctx(1);
    std::string endpoint = TestEndpoint("goodbye");
    zmq::socket_t router(ctx, zmq::socket_type::router);
    router.bind(endpoint);

    ZmqProviderSink sink(ctx, endpoint, kInstrument, "BYBIT");
    sink.SendGoodbye(GoodbyeReason::kShutdown);

    GoodbyeReason decoded{};
    ASSERT_TRUE(DecodeGoodbye(RecvPayload(router), decoded));
    EXPECT_EQ(decoded, GoodbyeReason::kShutdown);
}

TEST(ZmqProviderSinkTest, UpdateArrivesWithoutVenueOnTheWire) {
    zmq::context_t ctx(1);
    std::string endpoint = TestEndpoint("update");
    zmq::socket_t router(ctx, zmq::socket_type::router);
    router.bind(endpoint);

    ZmqProviderSink sink(ctx, endpoint, kInstrument, "BINANCE");
    BookUpdate update(VenueId::BINANCE, kInstrument, /*reserve_levels=*/2, /*is_snapshot=*/true, /*seq=*/5);
    update.bids = {{100, 1}};
    update.asks = {{101, 2}};
    ASSERT_TRUE(sink.SendUpdate(update));

    BookUpdate decoded;
    ASSERT_TRUE(DecodeBookUpdate(RecvPayload(router), decoded));
    EXPECT_EQ(decoded.instrument, kInstrument);
    EXPECT_EQ(decoded.seq, 5u);
    ASSERT_EQ(decoded.bids.size(), 1u);
    EXPECT_EQ(decoded.bids[0].price, 100u);
    ASSERT_EQ(decoded.asks.size(), 1u);
    EXPECT_EQ(decoded.asks[0].price, 101u);
}

TEST(ZmqProviderSinkTest, QuoteArrivesAndDecodes) {
    zmq::context_t ctx(1);
    std::string endpoint = TestEndpoint("quote");
    zmq::socket_t router(ctx, zmq::socket_type::router);
    router.bind(endpoint);

    ZmqProviderSink sink(ctx, endpoint, kInstrument, "OKX");
    BboQuote quote{};
    quote.instrument = kInstrument;
    quote.bid_price = 10;
    quote.ask_price = 11;
    sink.OnQuote(quote);  // void - drop+count policy, no return to check

    BboQuote decoded{};
    ASSERT_TRUE(DecodeBboQuote(RecvPayload(router), decoded));
    EXPECT_EQ(decoded.bid_price, 10u);
    EXPECT_EQ(decoded.ask_price, 11u);
    EXPECT_EQ(sink.QuoteDropCount(), 0u);
}

TEST(ZmqProviderSinkTest, HealthArrivesAndDecodes) {
    zmq::context_t ctx(1);
    std::string endpoint = TestEndpoint("health");
    zmq::socket_t router(ctx, zmq::socket_type::router);
    router.bind(endpoint);

    ZmqProviderSink sink(ctx, endpoint, kInstrument, "BYBIT");
    VenueHealthEvent event{};
    event.stream = StreamKind::kDepth;
    event.health = VenueHealth::kStale;
    event.decided_mono_ns = 123;
    ASSERT_TRUE(sink.SendHealth(event));

    VenueHealthEvent decoded{};
    ASSERT_TRUE(DecodeHealth(RecvPayload(router), decoded));
    EXPECT_EQ(decoded.stream, StreamKind::kDepth);
    EXPECT_EQ(decoded.health, VenueHealth::kStale);
    EXPECT_EQ(decoded.decided_mono_ns, 123);
}

// No bound ROUTER at all: DEALER still queues sends locally up to its HWM
// (the "startup ordering" - ordering between process starts does not
// matter), so this proves the OVERFLOW path fires once that queue fills,
// without needing a live peer to create backpressure.
TEST(ZmqProviderSinkTest, SendFailsAndCountsOverflowOnceQueueSaturates) {
    zmq::context_t ctx(1);
    // A path nothing binds - the sink connects, and every send queues
    // locally rather than being delivered.
    std::string endpoint = "ipc:///tmp/zmq_provider_sink_test_no_peer.ipc";

    ZmqProviderSink sink(ctx, endpoint, kInstrument, "BINANCE");
    BookUpdate update(VenueId::BINANCE, kInstrument, /*reserve_levels=*/0);

    bool saw_failure = false;
    // kSendHwm is 256 (zmq_provider_sink.cpp); comfortably overrun it.
    for (int i = 0; i < 2000 && !saw_failure; ++i) {
        if (!sink.SendUpdate(update)) {
            saw_failure = true;
        }
    }
    EXPECT_TRUE(saw_failure);
    EXPECT_GT(sink.OverflowCount(), 0u);
}

// A depth delta or health transition that cannot be sent is a broken diff
// chain - OnUpdate/OnHealth must escalate to the resync hook. A dropped quote
// must not: the next quote supersedes it whole.
TEST(ZmqProviderSinkTest, ResyncHookFiresForLostUpdateAndHealthButNotQuote) {
    zmq::context_t ctx(1);
    std::string endpoint = "ipc:///tmp/zmq_provider_sink_test_resync_hook.ipc";  // nothing binds it

    ZmqProviderSink sink(ctx, endpoint, kInstrument, "BINANCE");
    int resync_calls = 0;
    sink.SetResyncHook([&resync_calls] { ++resync_calls; });

    // Saturate the DEALER's local queue so every subsequent send fails.
    BookUpdate update(VenueId::BINANCE, kInstrument, /*reserve_levels=*/0);
    for (int i = 0; i < 2000 && sink.SendUpdate(update); ++i) {
    }
    ASSERT_GT(sink.OverflowCount(), 0u);

    resync_calls = 0;
    sink.OnUpdate(BookUpdate(VenueId::BINANCE, kInstrument, /*reserve_levels=*/0));
    EXPECT_EQ(resync_calls, 1);

    VenueHealthEvent event{};
    event.stream = StreamKind::kDepth;
    event.health = VenueHealth::kStale;
    sink.OnHealth(event);
    EXPECT_EQ(resync_calls, 2);

    BboQuote quote{};
    quote.instrument = kInstrument;
    sink.OnQuote(quote);
    EXPECT_EQ(resync_calls, 2);  // unchanged - a dropped quote is not escalated
    EXPECT_GT(sink.QuoteDropCount(), 0u);
}
