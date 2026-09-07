#pragma once

// BybitParser: parses Bybit orderbook.* messages (v5 public/spot) into
// BookUpdate - the same message shape serves both the depth topic
// (orderbook.50) and the fast-BBO topic (orderbook.1). Bybit sends its
// snapshot in-channel, so there is no separate REST path.

#include <cstdint>
#include <optional>
#include <string_view>

#include "md_core/types.h"
#include "md_provider/base_parser.h"

namespace market_data {

// Stateful parser for one Bybit connection. Reuses the base Parser's simdjson
// parser and input buffer, so a steady stream of messages does no per-message
// allocation. NOT thread-safe: one instance per thread. Bybit gets its
// snapshot on the WS stream (type == "snapshot", or u == 1 after a service
// restart), so there is no separate REST-snapshot path.
class BybitParser : public Parser {
   public:
    // venue_depth: this venue's resolved book-depth tier (ProviderConfig::depth).
    // The bids/asks vectors of each parsed update are reserved to this size.
    explicit BybitParser(uint32_t venue_depth);

    // One Bybit orderbook.* message (v5 public/spot: {"topic","type","ts",
    // "data":{...},"cts"}). The same shape serves both the depth topic
    // (orderbook.50) and the fast-BBO topic (orderbook.1). Returns
    // std::nullopt for non-orderbook messages (subscribe acks, pongs) and
    // for malformed JSON. Never throws.
    //
    // fields are read in the SAME order they appear on the wire (topic,
    // type, ts, then inside data: u, b, a). simdjson on-demand is a single
    // forward pass; reordering these reads crashes (assertion), not just
    // misparses. test_bybit_parser.cpp pins this down against a real sample.
    std::optional<BookUpdate> ParseOrderbookMessage(std::string_view message, VenueId venue, InstrumentKey instrument);

    // Reads only the `topic` field - for routing a message on a combined
    // depth+BBO connection to the right handler before the real parse.
    // Returns std::nullopt for a non-orderbook message, same contract as
    // ParseOrderbookMessage.
    //
    // The returned view is only valid until the next call that touches this
    // parser's buffer (Load()) - callers must finish comparing it before
    // calling ParseOrderbookMessage, which reuses the same buffer.
    std::optional<std::string_view> PeekTopic(std::string_view message);
};

}  // namespace market_data
