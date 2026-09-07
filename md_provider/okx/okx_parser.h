#pragma once

// OkxParser: parses OKX "books" (depth) and "bbo-tbt" (fast-BBO) channel
// messages, chained by prevSeqId/seqId with an in-channel snapshot. Also
// holds the futures contract-size scale factor (SetContractSize) that every
// parsed quantity is multiplied by, and ParseOkxContractSize, which reads it
// from the REST instruments response.

#include <cstdint>
#include <optional>
#include <string_view>

#include "md_core/types.h"
#include "md_provider/base_parser.h"

namespace market_data {

// Stateful parser for one OKX connection. Reuses the base Parser's simdjson
// parser and input buffer, so a steady stream of messages does no per-message
// allocation. NOT thread-safe: one instance per thread. OKX gets its
// snapshot on the WS stream (action/type == "snapshot"), so unlike Binance
// there is no separate REST-snapshot path.
class OkxParser : public Parser {
   public:
    // venue_depth: this venue's resolved book-depth tier (ProviderConfig::depth).
    // The bids/asks vectors of each parsed update are reserved to this size.
    explicit OkxParser(uint32_t venue_depth);

    // One OKX "books" channel message (depth, public spot):
    // {"arg","action","data":[{"asks","bids","ts","checksum","prevSeqId","seqId"}]}
    // Uses the first entry of "data" - OKX sends exactly one per instId.
    // Returns std::nullopt for non-books messages (subscribe acks, pongs, an
    // empty data array) and for malformed JSON. Never throws.
    //
    // fields inside the data[] entry are read in the SAME order OKX
    // sends them: asks, bids, ts, checksum (skipped), prevSeqId, seqId.
    // simdjson on-demand is forward-only; reading ts/seqId before bids/asks
    // asserts (not a normal error). test_okx_parser.cpp pins this down.
    std::optional<BookUpdate> ParseBooksMessage(std::string_view message, VenueId venue, InstrumentKey instrument);

    // One OKX "bbo-tbt" channel message (fast top-of-book, public spot):
    // {"arg","data":[{"asks","bids","ts","seqId"}]}
    //
    // A separate method rather than a flag on ParseBooksMessage: bbo-tbt has
    // no `action` and no `checksum`, so it is a genuinely different shape.
    // Gating on `data` (always present here) keeps the read forward-only.
    // Never throws.
    std::optional<BookUpdate> ParseBboMessage(std::string_view message, VenueId venue, InstrumentKey instrument);

    // Reads only `arg.channel` - for routing a message on a combined
    // depth+BBO connection ("books" vs "bbo-tbt") to the right parse method
    // before the real one runs. Returns std::nullopt for a message with no
    // `arg.channel` (subscribe acks, pongs, malformed JSON).
    //
    // The returned view is only valid until the next call that touches this
    // parser's buffer (Load()) - callers must finish comparing it before
    // calling ParseBooksMessage/ParseBboMessage, which reuse the same buffer.
    std::optional<std::string_view> PeekChannel(std::string_view message);

    // Contract size (OKX `ctVal`) for a SWAP instrument, scaled by
    // kScaleFactor. Every parsed level's quantity is multiplied by it.
    //
    // OKX quotes SWAP sizes in CONTRACTS, not the base currency.
    // BTC-USDT-SWAP has ctVal 0.01 BTC, so a raw level of "495.94" means
    // 4.9594 BTC. Binance futures and Bybit linear both quote plain BTC, and
    // all three merge into ONE consolidated futures book - so leaving OKX
    // unconverted puts it in that book 100x oversized and it dominates every
    // price level.
    //
    // The default is kScaleFactor (== 1.0), which leaves quantities untouched.
    // That is correct for SPOT, where sizes are already in the base currency,
    // and means the conversion costs spot nothing.
    //
    // Must be called before any message is parsed. OKXProvider does it on the
    // worker thread before a socket exists, so no message can be parsed at the
    // wrong scale and then silently re-scaled mid-book.
    void SetContractSize(QtyUnits contract_size_scaled) { contract_size_ = contract_size_scaled; }

   private:
    QtyUnits contract_size_ = kScaleFactor;
};

// Reads a /api/v5/public/instruments response and returns `inst_id`'s contract
// size (ctVal), scaled by kScaleFactor.
//
// Returns nullopt when the body is malformed, OKX reports a non-zero `code`,
// `inst_id` is absent, or - deliberately - when the instrument is not a plain
// linear contract with ctMult == 1:
//
//   - ctType != "linear": an INVERSE contract's ctVal is denominated in the
//     QUOTE currency (USD), not the base, so multiplying a size by it would
//     produce a number that is not a BTC quantity at all.
//   - ctMult != 1: OKX's own size-to-base conversion is then not ctVal alone,
//     and guessing the rest is exactly the kind of invented number that
//     silently corrupts a book.
//
// Refusing is safe - the caller stops the venue - while guessing is not.
//
// A free function, not a member: it runs once at startup on the worker thread
// and must not touch OkxParser's simdjson buffer, which belongs to the
// io_context thread.
std::optional<QtyUnits> ParseOkxContractSize(std::string_view body, std::string_view inst_id);

}  // namespace market_data
