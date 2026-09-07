#include <gtest/gtest.h>

#include "md_provider/bybit/bybit_parser.h"

using namespace market_data;

namespace {

// Real Bybit v5 public/spot orderbook message shape:
// {"topic","type","ts","data":{"s","b","a","u","seq"},"cts"}
constexpr const char* kSnapshotMessage = R"({
    "topic": "orderbook.50.BTCUSDT",
    "type": "snapshot",
    "ts": 1672304484978,
    "data": {
        "s": "BTCUSDT",
        "b": [["16493.50", "0.006"], ["16493.00", "0.100"]],
        "a": [["16497.00", "0.100"], ["16497.50", "0.023"]],
        "u": 177400507,
        "seq": 7961638724
    },
    "cts": 1672304484976
})";

constexpr const char* kDeltaMessage = R"({
    "topic": "orderbook.50.BTCUSDT",
    "type": "delta",
    "ts": 1672304485100,
    "data": {
        "s": "BTCUSDT",
        "b": [["16493.50", "0"]],
        "a": [],
        "u": 177400508,
        "seq": 7961638725
    },
    "cts": 1672304485098
})";

constexpr const char* kSubscribeAck = R"({"success":true,"ret_msg":"","op":"subscribe","conn_id":"abc"})";

// Bybit sends u == 1 after a service restart. The payload still says
// "type":"delta", but it is fresh snapshot data.
constexpr const char* kRestartMessage = R"({
    "topic": "orderbook.50.BTCUSDT",
    "type": "delta",
    "ts": 1672304485200,
    "data": {
        "s": "BTCUSDT",
        "b": [["16490.00", "1.5"]],
        "a": [["16495.00", "2.5"]],
        "u": 1,
        "seq": 7961638800
    },
    "cts": 1672304485198
})";

}  // namespace

TEST(BybitParserTest, ParsesSnapshotMessage) {
    BybitParser parser(/*venue_depth=*/50);
    auto update = parser.ParseOrderbookMessage(kSnapshotMessage, VenueId::BYBIT,
                                               MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));

    ASSERT_TRUE(update.has_value());
    EXPECT_EQ(update->venue, VenueId::BYBIT);
    EXPECT_EQ(update->instrument, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_TRUE(update->is_snapshot);
    EXPECT_EQ(update->seq, 177400507u);
    EXPECT_EQ(update->exch_ts_ns, 1672304484978LL * kTsNsMultiplier);

    ASSERT_EQ(update->bids.size(), 2u);
    EXPECT_EQ(update->bids[0].price, 1649350000000ull);  // 16493.50 * 1e8
    EXPECT_EQ(update->bids[0].qty, 600000ull);           // 0.006 * 1e8

    ASSERT_EQ(update->asks.size(), 2u);
    EXPECT_EQ(update->asks[0].price, 1649700000000ull);  // 16497.00 * 1e8
}

TEST(BybitParserTest, ParsesDeltaMessageAsNonSnapshot) {
    BybitParser parser(/*venue_depth=*/50);
    auto update =
        parser.ParseOrderbookMessage(kDeltaMessage, VenueId::BYBIT, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));

    ASSERT_TRUE(update.has_value());
    EXPECT_FALSE(update->is_snapshot);
    EXPECT_EQ(update->seq, 177400508u);
    // qty "0" means "remove this level" - still a normal level entry here,
    // FlatOrderBook is what interprets qty==0 as a removal.
    ASSERT_EQ(update->bids.size(), 1u);
    EXPECT_EQ(update->bids[0].qty, 0u);
}

// u == 1 means Bybit restarted its service, so this is fresh snapshot data
// even though "type" says "delta". Normalising it here - rather than in the
// sequencing layer - is what keeps venue quirks out of continuity.h.
// Missing it would merge a brand-new book into a stale one.
TEST(BybitParserTest, UpdateIdOneIsNormalisedToSnapshot) {
    BybitParser parser(/*venue_depth=*/50);
    auto update = parser.ParseOrderbookMessage(kRestartMessage, VenueId::BYBIT,
                                               MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));

    ASSERT_TRUE(update.has_value());
    EXPECT_TRUE(update->is_snapshot) << R"(u == 1 must be treated as a snapshot despite "type":"delta")";
    EXPECT_EQ(update->seq, 1u);
}

TEST(BybitParserTest, IgnoresNonOrderbookMessages) {
    BybitParser parser(/*venue_depth=*/50);
    auto update =
        parser.ParseOrderbookMessage(kSubscribeAck, VenueId::BYBIT, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update.has_value());
}

TEST(BybitParserTest, MalformedJsonReturnsNulloptNotACrash) {
    BybitParser parser(/*venue_depth=*/50);
    auto update = parser.ParseOrderbookMessage("{not valid json", VenueId::BYBIT,
                                               MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot));
    EXPECT_FALSE(update.has_value());
}

// PeekTopic is what routes a message on a combined depth+BBO connection
// (BybitProvider::OnCombinedMessage) before the real parse runs.
TEST(BybitParserTest, PeekTopicReadsTheTopicField) {
    BybitParser parser(/*venue_depth=*/50);
    auto topic = parser.PeekTopic(kSnapshotMessage);
    ASSERT_TRUE(topic.has_value());
    EXPECT_EQ(*topic, "orderbook.50.BTCUSDT");
}

TEST(BybitParserTest, PeekTopicDistinguishesDepthFromBbo) {
    BybitParser parser(/*venue_depth=*/50);

    // Depth topic differs from the BBO topic (orderbook.1.<symbol>) whenever
    // the resolved depth tier is not 1 - the normal case.
    auto depth_topic = parser.PeekTopic(kSnapshotMessage);
    ASSERT_TRUE(depth_topic.has_value());
    EXPECT_NE(*depth_topic, "orderbook.1.BTCUSDT");
}

TEST(BybitParserTest, PeekTopicReturnsNulloptForNonOrderbookMessages) {
    BybitParser parser(/*venue_depth=*/50);
    EXPECT_FALSE(parser.PeekTopic(kSubscribeAck).has_value());
    EXPECT_FALSE(parser.PeekTopic("{not valid json").has_value());
}
