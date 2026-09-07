#include <gtest/gtest.h>

#include "md_provider/binance/binance_parser.h"

using namespace market_data;

namespace {

// Real Binance spot depthUpdate message shape:
// {"e","E","s","U","u","b","a"}
constexpr const char* kDepthMessage = R"({
    "e": "depthUpdate",
    "E": 1672515782136,
    "s": "BTCUSDT",
    "U": 157,
    "u": 160,
    "b": [["0.0024", "10"]],
    "a": [["0.0026", "100"]]
})";

constexpr const char* kNonDepthMessage = R"({"result":null,"id":1})";

// Real Binance bookTicker message shape: {"u","s","b","B","a","A"}
constexpr const char* kBboMessage = R"({
    "u": 400900217,
    "s": "BNBUSDT",
    "b": "25.35190000",
    "B": "31.21000000",
    "a": "25.36520000",
    "A": "40.66000000"
})";

// Real combined-stream envelope shape (/stream?streams=A/B):
// {"stream":"<name>","data":<the real event>}. `data` here is exactly
// kDepthMessage/kBboMessage above, so a round trip through UnwrapCombinedMessage
// then ParseDepthMessage/ParseBboMessage must agree with parsing them directly.
constexpr const char* kCombinedDepthMessage = R"({
    "stream": "btcusdt@depth@100ms",
    "data": {
        "e": "depthUpdate",
        "E": 1672515782136,
        "s": "BTCUSDT",
        "U": 157,
        "u": 160,
        "b": [["0.0024", "10"]],
        "a": [["0.0026", "100"]]
    }
})";

constexpr const char* kCombinedBboMessage = R"({
    "stream": "btcusdt@bookTicker",
    "data": {
        "u": 400900217,
        "s": "BNBUSDT",
        "b": "25.35190000",
        "B": "31.21000000",
        "a": "25.36520000",
        "A": "40.66000000"
    }
})";

}  // namespace

TEST(BinanceParserTest, ParsesDepthUpdateMessage) {
    BinanceParser parser(/*venue_depth=*/20);
    auto update =
        parser.ParseDepthMessage(kDepthMessage, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->venue, VenueId::BINANCE);
    EXPECT_EQ(update->instrument, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update->is_snapshot);
    EXPECT_EQ(update->seq, 160u);
    EXPECT_EQ(update->exch_ts_ns, 1672515782136LL * kTsNsMultiplier);

    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].price, 240000ull);    // 0.0024 * 1e8
    EXPECT_EQ(update->bids[0].qty, 1000000000ull);  // 10 * 1e8

    ASSERT_EQ(update->asks.size(), 1u);
    EXPECT_EQ(update->asks[0].price, 260000ull);     // 0.0026 * 1e8
    EXPECT_EQ(update->asks[0].qty, 10000000000ull);  // 100 * 1e8
}

TEST(BinanceParserTest, IgnoresNonDepthUpdateMessages) {
    BinanceParser parser(/*venue_depth=*/20);
    auto update =
        parser.ParseDepthMessage(kNonDepthMessage, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update.has_value());
}

TEST(BinanceParserTest, DepthMalformedJsonReturnsNulloptNotACrash) {
    BinanceParser parser(/*venue_depth=*/20);
    auto update = parser.ParseDepthMessage("{not valid json", VenueId::BINANCE,
                                           MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update.has_value());
}

TEST(BinanceParserTest, ParsesBookTickerMessage) {
    BinanceParser parser(/*venue_depth=*/20);
    auto quote =
        parser.ParseBboMessage(kBboMessage, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));

    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->venue, VenueId::BINANCE);
    EXPECT_EQ(quote->instrument, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_EQ(quote->seq, 400900217u);           // the `u` field
    EXPECT_EQ(quote->bid_price, 2535190000ull);  // 25.35190000 * 1e8
    EXPECT_EQ(quote->bid_qty, 3121000000ull);    // 31.21000000 * 1e8
    EXPECT_EQ(quote->ask_price, 2536520000ull);  // 25.36520000 * 1e8
    EXPECT_EQ(quote->ask_qty, 4066000000ull);    // 40.66000000 * 1e8
}

TEST(BinanceParserTest, IgnoresNonBookTickerMessages) {
    BinanceParser parser(/*venue_depth=*/20);
    auto quote =
        parser.ParseBboMessage(kNonDepthMessage, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(quote.has_value());
}

// UnwrapCombinedMessage is what routes a message on the combined-stream
// connection (BinanceProvider::OnCombinedMessage) before the real parse runs.
TEST(BinanceParserTest, UnwrapCombinedMessageReadsStreamAndData) {
    BinanceParser parser(/*venue_depth=*/20);
    auto unwrapped = parser.UnwrapCombinedMessage(kCombinedDepthMessage);
    ASSERT_TRUE(unwrapped.has_value());
    EXPECT_EQ(unwrapped->stream, "btcusdt@depth@100ms");

    // The unwrapped `data` must parse identically to the un-enveloped
    // message it came from - this is the whole point of unwrapping.
    auto update =
        parser.ParseDepthMessage(unwrapped->data, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->seq, 160u);
    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].qty, 1000000000ull);  // 10 * 1e8
}

TEST(BinanceParserTest, UnwrapCombinedMessageDistinguishesDepthFromBbo) {
    BinanceParser parser(/*venue_depth=*/20);

    auto depth = parser.UnwrapCombinedMessage(kCombinedDepthMessage);
    ASSERT_TRUE(depth.has_value());
    EXPECT_EQ(depth->stream, "btcusdt@depth@100ms");

    auto bbo = parser.UnwrapCombinedMessage(kCombinedBboMessage);
    ASSERT_TRUE(bbo.has_value());
    EXPECT_EQ(bbo->stream, "btcusdt@bookTicker");

    auto quote = parser.ParseBboMessage(bbo->data, VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    ASSERT_TRUE(quote.has_value());
    EXPECT_EQ(quote->seq, 400900217u);
}

TEST(BinanceParserTest, UnwrapCombinedMessageReturnsNulloptWithoutStreamOrData) {
    BinanceParser parser(/*venue_depth=*/20);
    EXPECT_FALSE(parser.UnwrapCombinedMessage(kNonDepthMessage).has_value());
    EXPECT_FALSE(parser.UnwrapCombinedMessage(R"({"stream":"btcusdt@depth@100ms"})").has_value());
    EXPECT_FALSE(parser.UnwrapCombinedMessage("{not valid json").has_value());
}
