#include "binance_parser.h"

#include <simdjson.h>

#include <vector>

#include "decimal.h"

using namespace simdjson;

namespace market_data {

namespace {

void AppendLevels(ondemand::array levels, std::vector<PriceLevel>& out) {
    for (auto level : levels) {
        auto pair = level.get_array();
        auto it = pair.begin();
        std::string_view price_sv = (*it).get_string();
        ++it;
        std::string_view qty_sv = (*it).get_string();

        PriceLevel level_out;
        level_out.price = ParseScaledDecimal<kBinanceScale>(price_sv);
        level_out.qty = ParseScaledDecimal<kBinanceScale>(qty_sv);
        out.push_back(level_out);
    }
}

}  // namespace

BinanceParser::BinanceParser(uint32_t venue_depth) : Parser(venue_depth) {}

std::optional<BookUpdate> BinanceParser::ParseDepthMessage(std::string_view message, VenueId venue,
                                                           InstrumentKey instrument) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        auto event_type_result = doc["e"].get_string();
        if (event_type_result.error() || event_type_result.value() != "depthUpdate") {
            return std::nullopt;  // not a depth update (e.g. a control/ack message)
        }

        BookUpdate update(venue, instrument, reserve_levels_);

        auto event_ts_result = doc["E"].get_int64();
        update.exch_ts_ns = event_ts_result.error() ? 0 : event_ts_result.value() * kTsNsMultiplier;

        // Wire order is e, E, s, U, u, b, a - so U must be read before u to
        // stay forward-only.
        auto first_id_result = doc["U"].get_uint64();
        update.prev_seq = first_id_result.error() ? 0 : static_cast<int64_t>(first_id_result.value());

        auto final_id_result = doc["u"].get_uint64();
        update.seq = final_id_result.error() ? 0 : final_id_result.value();

        // FUTURES only: "pu" sits right after "u" in wire order, so reading
        // it here stays forward-only. Absent on spot -> error -> nullopt,
        // which is exactly "this message has no chain signal to give".
        // Always assigned (never left untouched), so a stale value from a
        // previous futures call can never survive into this one's result.
        auto chain_seq_result = doc["pu"].get_uint64();
        last_chain_seq_ = chain_seq_result.error() ? std::nullopt : std::optional<uint64_t>(chain_seq_result.value());

        auto bids_result = doc["b"].get_array();
        if (!bids_result.error()) {
            AppendLevels(bids_result.value(), update.bids);
        }
        auto asks_result = doc["a"].get_array();
        if (!asks_result.error()) {
            AppendLevels(asks_result.value(), update.asks);
        }

        return update;

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<BookUpdate> BinanceParser::ParseDepthSnapshot(std::string_view body, VenueId venue,
                                                            InstrumentKey instrument) {
    try {
        ondemand::document doc = parser_.iterate(Load(body));

        // Wire order is lastUpdateId, bids, asks. lastUpdateId is also the
        // gate: an error response ({"code":..,"msg":..}) has no such field.
        auto last_update_id_result = doc["lastUpdateId"].get_uint64();
        if (last_update_id_result.error()) {
            return std::nullopt;
        }

        // The ctor reserves bids/asks and sets is_snapshot + seq.
        // A REST snapshot has no predecessor and no exchange timestamp.
        BookUpdate update{venue, instrument, reserve_levels_, true, last_update_id_result.value()};

        auto bids_result = doc["bids"].get_array();
        if (!bids_result.error()) {
            AppendLevels(bids_result.value(), update.bids);
        }
        auto asks_result = doc["asks"].get_array();
        if (!asks_result.error()) {
            AppendLevels(asks_result.value(), update.asks);
        }

        return update;

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<BboQuote> BinanceParser::ParseBboMessage(std::string_view message, VenueId venue,
                                                       InstrumentKey instrument) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        // Read in wire order (u, s, b, B, a, A) - simdjson on-demand scans
        // forward, so matching the document's own order is cheapest.
        auto update_id_result = doc["u"].get_uint64();
        if (update_id_result.error()) {
            return std::nullopt;  // not a bookTicker payload (e.g. a subscribe ack)
        }

        auto bid_price_result = doc["b"].get_string();
        auto bid_qty_result = doc["B"].get_string();
        auto ask_price_result = doc["a"].get_string();
        auto ask_qty_result = doc["A"].get_string();

        if (bid_price_result.error() || bid_qty_result.error() || ask_price_result.error() || ask_qty_result.error()) {
            return std::nullopt;
        }

        BboQuote quote;
        quote.venue = venue;
        quote.instrument = instrument;
        quote.seq = update_id_result.value();
        // bookTicker carries no exchange timestamp - exch_ts_ns stays 0.
        quote.bid_price = ParseScaledDecimal<kBinanceScale>(bid_price_result.value());
        quote.bid_qty = ParseScaledDecimal<kBinanceScale>(bid_qty_result.value());
        quote.ask_price = ParseScaledDecimal<kBinanceScale>(ask_price_result.value());
        quote.ask_qty = ParseScaledDecimal<kBinanceScale>(ask_qty_result.value());
        return quote;

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<BinanceParser::CombinedStreamMessage> BinanceParser::UnwrapCombinedMessage(std::string_view message) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        // Wire order: "stream" comes first, "data" second - simdjson
        // on-demand is forward-only, so reading them out of order asserts.
        auto stream_result = doc["stream"].get_string();
        if (stream_result.error()) {
            return std::nullopt;
        }

        // raw_json() consumes and returns the "data" object's exact bytes
        // from the input buffer - not a re-serialization, so it costs no
        // more than skipping the object already would.
        auto data_result = doc["data"].raw_json();
        if (data_result.error()) {
            return std::nullopt;
        }

        // Copied out now: data_result's view points into input_ (this call's
        // Load() buffer), which ParseDepthMessage/ParseBboMessage's own
        // Load() - called on the unwrapped data next - would overwrite.
        // data_buffer_ only grows, never reallocated smaller, so this is not
        // a fresh heap allocation once warmed up. stream_result's view is
        // left as-is: nothing here touches input_ again before the caller
        // uses it.
        data_buffer_.assign(data_result.value());

        return CombinedStreamMessage{stream_result.value(), data_buffer_};

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

}  // namespace market_data
