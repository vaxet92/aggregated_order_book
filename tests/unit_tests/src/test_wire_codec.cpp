#include <gtest/gtest.h>

#include "md_wire/wire_codec.h"

using namespace market_data;
using namespace market_data::wire;

namespace {

constexpr InstrumentKey kInstrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);

}  // namespace

// --- kHello ------------------------------------------------------------

TEST(WireCodecTest, HelloRoundTrips) {
    std::vector<std::byte> buf;
    EncodeHello(kInstrument, "BINANCE", buf);

    InstrumentKey decoded_instrument{};
    std::string decoded_venue;
    ASSERT_TRUE(DecodeHello(buf, decoded_instrument, decoded_venue));
    EXPECT_EQ(decoded_instrument, kInstrument);
    EXPECT_EQ(decoded_venue, "BINANCE");
}

TEST(WireCodecTest, HelloTruncatesOverlongVenueNameRatherThanOverflow) {
    std::vector<std::byte> buf;
    // 20 chars - longer than kVenueNameCapacity - 1 (15).
    EncodeHello(kInstrument, "A_VERY_LONG_VENUE_NAME", buf);

    InstrumentKey decoded_instrument{};
    std::string decoded_venue;
    ASSERT_TRUE(DecodeHello(buf, decoded_instrument, decoded_venue));
    EXPECT_EQ(decoded_venue.size(), kVenueNameCapacity - 1);
}

TEST(WireCodecTest, DecodeHelloRejectsWrongMsgType) {
    std::vector<std::byte> buf;
    EncodeGoodbye(GoodbyeReason::kShutdown, buf);

    InstrumentKey decoded_instrument{};
    std::string decoded_venue;
    EXPECT_FALSE(DecodeHello(buf, decoded_instrument, decoded_venue));
}

// --- kGoodbye ------------------------------------------------------------

TEST(WireCodecTest, GoodbyeRoundTrips) {
    std::vector<std::byte> buf;
    EncodeGoodbye(GoodbyeReason::kError, buf);

    GoodbyeReason decoded{};
    ASSERT_TRUE(DecodeGoodbye(buf, decoded));
    EXPECT_EQ(decoded, GoodbyeReason::kError);
}

// --- kBookUpdate ---------------------------------------------------------

TEST(WireCodecTest, BookUpdateRoundTripsWithLevels) {
    BookUpdate update(VenueId::OKX, kInstrument, /*reserve_levels=*/4, /*is_snapshot=*/true, /*seq=*/42);
    update.prev_seq = -1;
    update.recv_ts_ns = 1000;
    update.exch_ts_ns = 2000;
    update.recv_mono_ns = 3000;
    update.bids = {{100, 1}, {99, 2}};
    update.asks = {{101, 3}};

    std::vector<std::byte> buf;
    EncodeBookUpdate(update, buf);

    BookUpdate decoded;
    ASSERT_TRUE(DecodeBookUpdate(buf, decoded));

    EXPECT_EQ(decoded.instrument, kInstrument);
    EXPECT_EQ(decoded.seq, 42u);
    EXPECT_EQ(decoded.prev_seq, -1);
    EXPECT_EQ(decoded.recv_ts_ns, 1000);
    EXPECT_EQ(decoded.exch_ts_ns, 2000);
    EXPECT_EQ(decoded.recv_mono_ns, 3000);  // decoded from wire's provider_mono_ns
    EXPECT_TRUE(decoded.is_snapshot);
    ASSERT_EQ(decoded.bids.size(), 2u);
    EXPECT_EQ(decoded.bids[0].price, 100u);
    EXPECT_EQ(decoded.bids[0].qty, 1u);
    EXPECT_EQ(decoded.bids[1].price, 99u);
    EXPECT_EQ(decoded.bids[1].qty, 2u);
    ASSERT_EQ(decoded.asks.size(), 1u);
    EXPECT_EQ(decoded.asks[0].price, 101u);
    EXPECT_EQ(decoded.asks[0].qty, 3u);
}

TEST(WireCodecTest, BookUpdateRoundTripsWithEmptySides) {
    BookUpdate update(VenueId::BINANCE, kInstrument, /*reserve_levels=*/0, /*is_snapshot=*/false, /*seq=*/7);

    std::vector<std::byte> buf;
    EncodeBookUpdate(update, buf);

    BookUpdate decoded;
    ASSERT_TRUE(DecodeBookUpdate(buf, decoded));
    EXPECT_TRUE(decoded.bids.empty());
    EXPECT_TRUE(decoded.asks.empty());
    EXPECT_FALSE(decoded.is_snapshot);
}

TEST(WireCodecTest, BookUpdateVenueIsNotCarriedOnTheWire) {
    // identity comes from the transport, not the payload. Decode must
    //  not touch `venue` - the caller stamps it from the connection's bound
    //  slot, which this test simulates by pre-seeding a sentinel value and
    //  checking it survives decode untouched.
    BookUpdate update(VenueId::OKX, kInstrument, /*reserve_levels=*/0);
    std::vector<std::byte> buf;
    EncodeBookUpdate(update, buf);

    BookUpdate decoded;
    decoded.venue = VenueId::BYBIT;  // sentinel: must NOT become OKX after decode
    ASSERT_TRUE(DecodeBookUpdate(buf, decoded));
    EXPECT_EQ(decoded.venue, VenueId::BYBIT);
}

TEST(WireCodecTest, DecodeBookUpdateReusesCapacity) {
    BookUpdate update(VenueId::BINANCE, kInstrument, /*reserve_levels=*/0);
    update.bids = {{1, 1}, {2, 2}, {3, 3}};

    std::vector<std::byte> buf;
    EncodeBookUpdate(update, buf);

    BookUpdate decoded;
    ASSERT_TRUE(DecodeBookUpdate(buf, decoded));
    ASSERT_EQ(decoded.bids.size(), 3u);
    const std::size_t cap_after_first = decoded.bids.capacity();

    // A second, smaller update into the same `decoded` must not need to grow
    // the vector - this is the "reused BookUpdate" decode path
    // describes as allocating nothing in steady state.
    BookUpdate smaller(VenueId::BINANCE, kInstrument, /*reserve_levels=*/0);
    smaller.bids = {{9, 9}};
    std::vector<std::byte> buf2;
    EncodeBookUpdate(smaller, buf2);
    ASSERT_TRUE(DecodeBookUpdate(buf2, decoded));
    EXPECT_EQ(decoded.bids.size(), 1u);
    EXPECT_EQ(decoded.bids.capacity(), cap_after_first);
}

TEST(WireCodecTest, DecodeBookUpdateRejectsTruncatedLevelRun) {
    BookUpdate update(VenueId::BINANCE, kInstrument, /*reserve_levels=*/0);
    update.bids = {{1, 1}, {2, 2}};

    std::vector<std::byte> buf;
    EncodeBookUpdate(update, buf);
    buf.resize(buf.size() - 1);  // truncate mid-level-run

    BookUpdate decoded;
    EXPECT_FALSE(DecodeBookUpdate(buf, decoded));
}

// --- kBboQuote -------------------------------------------------------------

TEST(WireCodecTest, BboQuoteRoundTrips) {
    BboQuote quote{};
    quote.venue = VenueId::BYBIT;
    quote.instrument = kInstrument;
    quote.seq = 99;
    quote.recv_ts_ns = 10;
    quote.exch_ts_ns = 20;
    quote.recv_mono_ns = 30;
    quote.bid_price = 100;
    quote.bid_qty = 1;
    quote.ask_price = 101;
    quote.ask_qty = 2;

    std::vector<std::byte> buf;
    EncodeBboQuote(quote, buf);

    BboQuote decoded{};
    decoded.venue = VenueId::OKX;  // sentinel: must survive untouched
    ASSERT_TRUE(DecodeBboQuote(buf, decoded));

    EXPECT_EQ(decoded.venue, VenueId::OKX);
    EXPECT_EQ(decoded.instrument, kInstrument);
    EXPECT_EQ(decoded.seq, 99u);
    EXPECT_EQ(decoded.recv_ts_ns, 10);
    EXPECT_EQ(decoded.exch_ts_ns, 20);
    EXPECT_EQ(decoded.recv_mono_ns, 30);
    EXPECT_EQ(decoded.bid_price, 100u);
    EXPECT_EQ(decoded.bid_qty, 1u);
    EXPECT_EQ(decoded.ask_price, 101u);
    EXPECT_EQ(decoded.ask_qty, 2u);
}

// --- kHealth -----------------------------------------------------------

TEST(WireCodecTest, HealthRoundTrips) {
    VenueHealthEvent event{};
    event.venue = VenueId::OKX;
    event.stream = StreamKind::kBbo;
    event.health = VenueHealth::kStale;
    event.decided_mono_ns = 555;

    std::vector<std::byte> buf;
    EncodeHealth(event, buf);

    VenueHealthEvent decoded{};
    decoded.venue = VenueId::BINANCE;  // sentinel: must survive untouched
    ASSERT_TRUE(DecodeHealth(buf, decoded));

    EXPECT_EQ(decoded.venue, VenueId::BINANCE);
    EXPECT_EQ(decoded.stream, StreamKind::kBbo);
    EXPECT_EQ(decoded.health, VenueHealth::kStale);
    EXPECT_EQ(decoded.decided_mono_ns, 555);
}

// --- header dispatch ---------------------------------------------------

TEST(WireCodecTest, DecodeHeaderReadsMsgTypeAndVersion) {
    std::vector<std::byte> buf;
    EncodeBboQuote(BboQuote{}, buf);

    WireHeader header{};
    ASSERT_TRUE(DecodeHeader(buf, header));
    EXPECT_EQ(header.msg_type, static_cast<uint16_t>(WireMsgType::kBboQuote));
    EXPECT_EQ(header.wire_version, kWireVersion);
}

TEST(WireCodecTest, DecodeHeaderRejectsEmptyBuffer) {
    WireHeader header{};
    EXPECT_FALSE(DecodeHeader({}, header));
}
