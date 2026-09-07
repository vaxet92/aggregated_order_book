#include <gtest/gtest.h>

#include "md_provider/okx/okx_parser.h"

using namespace market_data;

namespace {

// Real OKX "books" channel message shape:
// {"arg","action","data":[{"asks","bids","ts","checksum","seqId"}]}
constexpr const char* kSnapshotMessage = R"({
    "arg": {"channel": "books", "instId": "BTC-USDT"},
    "action": "snapshot",
    "data": [
        {
            "asks": [["8476.98", "415", "0", "13"]],
            "bids": [["8476.97", "256", "0", "12"]],
            "ts": "1597026383085",
            "checksum": -855196043,
            "seqId": 123456
        }
    ]
})";

constexpr const char* kUpdateMessage = R"({
    "arg": {"channel": "books", "instId": "BTC-USDT"},
    "action": "update",
    "data": [
        {
            "asks": [],
            "bids": [["8476.97", "0", "0", "0"]],
            "ts": "1597026383200",
            "checksum": 123456789,
            "seqId": 123457
        }
    ]
})";

constexpr const char* kSubscribeAck = R"({"event":"subscribe","arg":{"channel":"books","instId":"BTC-USDT"}})";

// Real bbo-tbt shape: no "action" and no "checksum", unlike the books channel.
constexpr const char* kBboMessage = R"({
    "arg": {"channel": "bbo-tbt", "instId": "BTC-USDT"},
    "data": [
        {
            "asks": [["8506.96", "100", "0", "2"]],
            "bids": [["8446.00", "95", "0", "3"]],
            "ts": "1597026383085",
            "seqId": 363996337
        }
    ]
})";

}  // namespace

TEST(OkxParserTest, ParsesSnapshotMessage) {
    OkxParser parser(/*venue_depth=*/400);
    auto update =
        parser.ParseBooksMessage(kSnapshotMessage, VenueId::OKX, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->venue, VenueId::OKX);
    EXPECT_EQ(update->instrument, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_TRUE(update->is_snapshot);
    EXPECT_EQ(update->seq, 123456u);
    EXPECT_EQ(update->exch_ts_ns, 1597026383085LL * kTsNsMultiplier);

    // 4-element levels [price, qty, deprecated, numOrders] - only first two matter.
    ASSERT_EQ(update->asks.size(), 1u);
    EXPECT_EQ(update->asks[0].price, 847698000000ull);  // 8476.98 * 1e8
    EXPECT_EQ(update->asks[0].qty, 41500000000ull);     // 415 * 1e8

    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].price, 847697000000ull);  // 8476.97 * 1e8
    EXPECT_EQ(update->bids[0].qty, 25600000000ull);     // 256 * 1e8
}

TEST(OkxParserTest, ParsesUpdateMessageAsNonSnapshot) {
    OkxParser parser(/*venue_depth=*/400);
    auto update =
        parser.ParseBooksMessage(kUpdateMessage, VenueId::OKX, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));

    ASSERT_TRUE(update.has_value());
    EXPECT_FALSE(update->is_snapshot);
    EXPECT_EQ(update->seq, 123457u);
    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].qty, 0u);  // qty "0" means "remove this level"
}

TEST(OkxParserTest, IgnoresNonBooksMessages) {
    OkxParser parser(/*venue_depth=*/400);
    auto update =
        parser.ParseBooksMessage(kSubscribeAck, VenueId::OKX, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update.has_value());
}

TEST(OkxParserTest, MalformedJsonReturnsNulloptNotACrash) {
    OkxParser parser(/*venue_depth=*/400);
    auto update =
        parser.ParseBooksMessage("{not valid json", VenueId::OKX, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update.has_value());
}

TEST(OkxParserTest, ParsesBboTbtMessage) {
    OkxParser parser(/*venue_depth=*/400);
    auto update = parser.ParseBboMessage(kBboMessage, VenueId::OKX, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->venue, VenueId::OKX);
    EXPECT_TRUE(update->is_snapshot);  // bbo-tbt is always a full replacement
    EXPECT_EQ(update->seq, 363996337u);
    EXPECT_EQ(update->exch_ts_ns, 1597026383085LL * kTsNsMultiplier);

    ASSERT_EQ(update->asks.size(), 1u);
    EXPECT_EQ(update->asks[0].price, 850696000000ull);  // 8506.96 * 1e8
    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].price, 844600000000ull);  // 8446.00 * 1e8
}

TEST(OkxParserTest, BboIgnoresSubscribeAck) {
    OkxParser parser(/*venue_depth=*/400);
    auto update =
        parser.ParseBboMessage(kSubscribeAck, VenueId::OKX, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update.has_value());
}

// The regression this parser split exists for: a shared parser probing for
// an absent `action` field left simdjson's lazy iterator at a broken depth,
// and the next lookup asserted instead of returning an error.
TEST(OkxParserTest, BboMalformedJsonReturnsNulloptNotACrash) {
    OkxParser parser(/*venue_depth=*/400);
    auto update =
        parser.ParseBboMessage("{not valid json", VenueId::OKX, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update.has_value());
}

// PeekChannel is what routes a message on a combined depth+BBO connection
// (OKXProvider::OnCombinedMessage) before the real parse runs.
TEST(OkxParserTest, PeekChannelDistinguishesBooksFromBboTbt) {
    OkxParser parser(/*venue_depth=*/400);

    auto books_channel = parser.PeekChannel(kSnapshotMessage);
    ASSERT_TRUE(books_channel.has_value());
    EXPECT_EQ(*books_channel, "books");

    auto bbo_channel = parser.PeekChannel(kBboMessage);
    ASSERT_TRUE(bbo_channel.has_value());
    EXPECT_EQ(*bbo_channel, "bbo-tbt");
}

// A subscribe ack for OKX echoes `arg.channel` back, so PeekChannel reading
// only that field cannot tell an ack from a real push - the same way
// ParseBooksMessage/ParseBboMessage rely on `action`/`data` for that, not on
// `arg`. This is deliberate: it is why OnCombinedMessage's routing is a
// "which real handler runs" decision, not a "is this a real message" one -
// the real handlers still make that call.
TEST(OkxParserTest, PeekChannelAlsoReadsASubscribeAcksEchoedArg) {
    OkxParser parser(/*venue_depth=*/400);
    auto channel = parser.PeekChannel(kSubscribeAck);
    ASSERT_TRUE(channel.has_value());
    EXPECT_EQ(*channel, "books");
}

TEST(OkxParserTest, PeekChannelReturnsNulloptForMessagesWithNoArgField) {
    OkxParser parser(/*venue_depth=*/400);
    EXPECT_FALSE(parser.PeekChannel(R"({"event":"pong"})").has_value());
    EXPECT_FALSE(parser.PeekChannel("{not valid json").has_value());
}
