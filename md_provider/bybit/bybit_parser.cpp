#include "bybit_parser.h"

#include <simdjson.h>

#include <vector>

#include "decimal.h"

using namespace simdjson;

namespace market_data {

namespace {

constexpr uint64_t kBybitScale = 8;

void AppendLevels(ondemand::array levels, std::vector<PriceLevel>& out) {
    for (auto level : levels) {
        auto pair = level.get_array();
        auto it = pair.begin();
        std::string_view price_sv = (*it).get_string();
        ++it;
        std::string_view qty_sv = (*it).get_string();

        PriceLevel level_out;
        level_out.price = ParseScaledDecimal<kBybitScale>(price_sv);
        level_out.qty = ParseScaledDecimal<kBybitScale>(qty_sv);
        out.push_back(level_out);
    }
}

}  // namespace

BybitParser::BybitParser(uint32_t venue_depth) : Parser(venue_depth) {}

std::optional<BookUpdate> BybitParser::ParseOrderbookMessage(std::string_view message, VenueId venue,
                                                             InstrumentKey instrument) {
    // Contract: never throws. simdjson_error (malformed JSON, unexpected
    // shape) becomes std::nullopt, same as "not an orderbook message" -
    // callers must not need a try/catch of their own.
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        auto topic_result = doc["topic"].get_string();
        if (topic_result.error()) {
            return std::nullopt;  // not an orderbook message (e.g. subscribe ack, pong)
        }

        auto type_result = doc["type"].get_string();
        bool is_snapshot = !type_result.error() && type_result.value() == "snapshot";

        // ts is read before data - it comes first in the real message.
        auto ts_result = doc["ts"].get_int64();
        int64_t exch_ts_ns = ts_result.error() ? 0 : ts_result.value() * kTsNsMultiplier;

        auto data = doc["data"].get_object();
        if (data.error()) {
            return std::nullopt;
        }

        BookUpdate update{venue, instrument, reserve_levels_, is_snapshot};
        update.exch_ts_ns = exch_ts_ns;

        auto seq_result = data["u"].get_uint64();
        update.seq = seq_result.error() ? 0 : seq_result.value();

        // u == 1 means Bybit restarted its service and this is fresh
        // snapshot data even though `type` still says "delta". That is a
        // statement about WHAT this message is, so it is normalised here
        // rather than left for the sequencing layer to remember.
        update.is_snapshot = is_snapshot || update.seq == 1;

        auto bids_result = data["b"].get_array();
        if (!bids_result.error()) {
            AppendLevels(bids_result.value(), update.bids);
        }
        auto asks_result = data["a"].get_array();
        if (!asks_result.error()) {
            AppendLevels(asks_result.value(), update.asks);
        }

        return update;

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<std::string_view> BybitParser::PeekTopic(std::string_view message) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        auto topic_result = doc["topic"].get_string();
        if (topic_result.error()) {
            return std::nullopt;  // not an orderbook message (e.g. subscribe ack, pong)
        }
        return topic_result.value();

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

}  // namespace market_data
