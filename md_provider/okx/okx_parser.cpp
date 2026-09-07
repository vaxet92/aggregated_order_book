#include "okx_parser.h"

#include <simdjson.h>

#include <vector>

#include "decimal.h"

using namespace simdjson;

namespace market_data {

// Price/qty are decimal strings ("8506.96", "256") - scale 8 like every
// other venue. `ts` is integer milliseconds - parsed at scale 0, then
// multiplied to nanoseconds.
constexpr uint64_t kOkxScale = 8;
constexpr uint64_t kOkxTsScale = 0;

namespace {

// OKX levels are 4-element arrays [price, qty, deprecated, numOrders] -
// only the first two matter here.
//
// `contract_size` converts a SWAP's size in CONTRACTS to the base currency
// (see OkxParser::SetContractSize). kScaleFactor means 1.0 - the spot case -
// and is checked for explicitly so spot pays nothing for a conversion it does
// not need. This runs up to 400 times per message.
void AppendLevels(ondemand::array levels, std::vector<PriceLevel>& out, QtyUnits contract_size) {
    const bool convert = contract_size != kScaleFactor;
    for (auto level : levels) {
        auto tuple = level.get_array();
        auto it = tuple.begin();
        std::string_view price_sv = (*it).get_string();
        ++it;
        std::string_view qty_sv = (*it).get_string();

        PriceLevel level_out;
        level_out.price = ParseScaledDecimal<kOkxScale>(price_sv);
        const QtyUnits raw_qty = ParseScaledDecimal<kOkxScale>(qty_sv);
        // widened to 128 bits before multiplying. Both operands are
        // already x1e8, so their product is x1e16 - 1e6 contracts would reach
        // 1e14 x 1e8 == 1e22 and overflow a uint64 (max ~1.8e19) long before
        // any real size did. Notional is unsigned __int128, so the product is
        // exact and the divide brings it back to x1e8.
        level_out.qty =
            convert ? static_cast<QtyUnits>(static_cast<Notional>(raw_qty) * contract_size / kScaleFactor) : raw_qty;
        out.push_back(level_out);
    }
}

}  // namespace

OkxParser::OkxParser(uint32_t venue_depth) : Parser(venue_depth) {}

std::optional<BookUpdate> OkxParser::ParseBooksMessage(std::string_view message, VenueId venue,
                                                       InstrumentKey instrument) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        // Hard gate, deliberately: returning on the first error means a
        // malformed document is never touched again. simdjson's on-demand
        // iterate() is lazy, so THIS is where bad JSON is actually detected -
        // continuing past it leaves the iterator at a broken depth and the
        // next lookup asserts. bbo-tbt (no `action`) has its own method.
        auto action_result = doc["action"].get_string();
        if (action_result.error()) {
            return std::nullopt;  // not a books update (e.g. subscribe ack, pong, malformed)
        }
        bool is_snapshot = action_result.value() == "snapshot";

        auto data_array = doc["data"].get_array();
        if (data_array.error()) {
            return std::nullopt;
        }

        // Exactly one entry expected per instId subscribed.
        for (auto entry : data_array.value()) {
            BookUpdate update{venue, instrument, reserve_levels_, is_snapshot};

            // Read in real document order: asks, bids, ts, checksum
            // (skipped), prevSeqId, seqId. prevSeqId MUST be read before
            // seqId - simdjson on-demand is forward-only.
            auto asks_result = entry["asks"].get_array();
            if (!asks_result.error()) {
                AppendLevels(asks_result.value(), update.asks, contract_size_);
            }
            auto bids_result = entry["bids"].get_array();
            if (!bids_result.error()) {
                AppendLevels(bids_result.value(), update.bids, contract_size_);
            }

            auto ts_result = entry["ts"].get_string();  // OKX sends ts as a string
            update.exch_ts_ns =
                ts_result.error() ? 0 : ParseScaledDecimal<kOkxTsScale>(ts_result.value()) * kTsNsMultiplier;

            // -1 on a snapshot; on an update it is the seqId this message
            // follows, which is what the continuity chain is checked against.
            auto prev_seq_result = entry["prevSeqId"].get_int64();
            update.prev_seq = prev_seq_result.error() ? 0 : prev_seq_result.value();

            auto seq_result = entry["seqId"].get_int64();
            update.seq = seq_result.error() ? 0 : static_cast<uint64_t>(seq_result.value());

            return update;
        }

        return std::nullopt;  // empty data array

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<BookUpdate> OkxParser::ParseBboMessage(std::string_view message, VenueId venue,
                                                     InstrumentKey instrument) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        // `data` is the gate here - always present on bbo-tbt, absent on
        // subscribe acks/pongs, and the first thing to fail on malformed
        // JSON. Nothing is read before it, so a failure here is returned
        // immediately and the iterator is never reused.
        auto data_array = doc["data"].get_array();
        if (data_array.error()) {
            return std::nullopt;
        }

        // Exactly one entry expected per instId subscribed.
        for (auto entry : data_array.value()) {
            BookUpdate update{venue, instrument, reserve_levels_, true};

            // Read in wire order: asks, bids, ts, seqId (no checksum here,
            // unlike the books channel).
            auto asks_result = entry["asks"].get_array();
            if (!asks_result.error()) {
                AppendLevels(asks_result.value(), update.asks, contract_size_);
            }
            auto bids_result = entry["bids"].get_array();
            if (!bids_result.error()) {
                AppendLevels(bids_result.value(), update.bids, contract_size_);
            }

            auto ts_result = entry["ts"].get_string();  // OKX sends ts as a string
            update.exch_ts_ns =
                ts_result.error() ? 0 : ParseScaledDecimal<kOkxTsScale>(ts_result.value()) * kTsNsMultiplier;

            auto seq_result = entry["seqId"].get_int64();
            update.seq = seq_result.error() ? 0 : static_cast<uint64_t>(seq_result.value());

            return update;
        }

        return std::nullopt;  // empty data array

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<std::string_view> OkxParser::PeekChannel(std::string_view message) {
    try {
        ondemand::document doc = parser_.iterate(Load(message));

        auto channel_result = doc["arg"]["channel"].get_string();
        if (channel_result.error()) {
            return std::nullopt;  // not a channel push (e.g. subscribe ack, pong, malformed)
        }
        return channel_result.value();

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

std::optional<QtyUnits> ParseOkxContractSize(std::string_view body, std::string_view inst_id) {
    try {
        // Its own parser and padded buffer: this runs on the worker thread at
        // startup and must not touch OkxParser's, which belongs to the
        // io_context thread.
        ondemand::parser parser;
        padded_string padded(body);
        ondemand::document doc = parser.iterate(padded);

        // OKX reports failures in the body with HTTP 200, so a non-zero `code`
        // is the real error channel - "0" is success.
        auto code_result = doc["code"].get_string();
        if (code_result.error() || code_result.value() != "0") {
            return std::nullopt;
        }

        auto data_array = doc["data"].get_array();
        if (data_array.error()) {
            return std::nullopt;
        }

        for (auto entry : data_array.value()) {
            auto inst_result = entry["instId"].get_string();
            if (inst_result.error() || inst_result.value() != inst_id) {
                continue;  // instruments[] can carry the whole instType
            }

            // An inverse contract's ctVal is denominated in the QUOTE currency,
            // so multiplying a size by it would not give a base quantity at
            // all. Refuse rather than convert wrongly.
            auto type_result = entry["ctType"].get_string();
            if (type_result.error() || type_result.value() != "linear") {
                return std::nullopt;
            }

            // ctMult != 1 means size -> base is not ctVal alone. What it IS
            // then is not something to guess at.
            auto mult_result = entry["ctMult"].get_string();
            if (mult_result.error() || ParseScaledDecimal<kOkxScale>(mult_result.value()) != kScaleFactor) {
                return std::nullopt;
            }

            auto ct_val_result = entry["ctVal"].get_string();
            if (ct_val_result.error()) {
                return std::nullopt;
            }
            const QtyUnits contract_size = ParseScaledDecimal<kOkxScale>(ct_val_result.value());
            // A zero contract size would silently zero every quantity in the
            // book - every level would look like a removal.
            if (contract_size == 0) {
                return std::nullopt;
            }
            return contract_size;
        }

        return std::nullopt;  // inst_id not present in the response

    } catch (const simdjson_error&) {
        return std::nullopt;
    }
}

}  // namespace market_data
