#pragma once

// BinanceParser: parses Binance depthUpdate (spot and futures), the REST
// depth-snapshot body, and bookTicker messages into normalised
// BookUpdate/BboQuote. Also exposes LastChainSeq() - futures' `pu` field,
// used only by the continuity check and never carried onto BookUpdate.

#include <optional>
#include <string_view>

#include "md_core/types.h"
#include "md_provider/base_parser.h"

namespace market_data {

// Binance sends prices/quantities as decimal strings; keep 8 fractional
// digits to match PriceTicks/QtyUnits.
inline constexpr uint64_t kBinanceScale = 8;

// Stateful parser for one Binance stream. Holds a reusable simdjson parser
// and input buffer, so a steady stream of messages does no per-message
// allocation.
//
// NOT thread-safe: one instance serves one thread. The depth and bookTicker
// handlers run on the same io_context thread and may share one instance; the
// REST snapshot runs on its own thread and needs its own instance.
class BinanceParser : public Parser {
   public:
    // venue_depth: this venue's resolved book-depth tier (ProviderConfig::depth).
    // The bids/asks vectors of each parsed update are reserved to this size, so
    // a full depth snapshot fills them without a reallocation. A @100ms delta
    // carries fewer levels and only uses part of it.
    explicit BinanceParser(uint32_t venue_depth);

    // One Binance depthUpdate. Spot: {"e","E","s","U","u","b","a"}. FUTURES
    // additionally carries "T" (ignored) and "pu" between "u" and "b" - see
    // LastChainSeq() below. `u` -> seq, `U` -> prev_seq (both the sync
    // reconciliation and the SPOT continuity chain need `U`). Returns
    // std::nullopt for non-depth messages (subscribe acks) and for malformed
    // JSON - never throws.
    std::optional<BookUpdate> ParseDepthMessage(std::string_view message, VenueId venue, InstrumentKey instrument);

    // The `pu` field from the FUTURES depthUpdate just parsed - the id of the
    // immediately preceding depthUpdate in THIS symbol's own real-time chain.
    // std::nullopt after a spot message (the field does not exist there) or a
    // malformed one.
    //
    // this is NOT the same thing as prev_seq (`U`). U is where this
    // event's coverage starts in a counter Binance shares across every
    // futures symbol, so a gap in U proves nothing about THIS symbol - see
    // CheckBinanceFuturesContinuity. pu is Binance's own answer to "what did
    // you last send me for BTCUSDT", and that is why futures needs a
    // different continuity rule from spot, not a different value plugged into
    // the same one.
    //
    // Deliberately NOT a field on BookUpdate/ProviderMessage: it is used and
    // discarded entirely within BinanceProvider::OnDepthMessage, before
    // Emit(), so it never needs to survive the trip through the SPSC queue -
    // see continuity.h.
    //
    // Set every time ParseDepthMessage reaches a real update (to nullopt on a
    // spot message too), so it is never stale whenever there is an update to
    // check it against. An early return (subscribe ack, malformed JSON) does
    // NOT touch it - harmless, because the caller only ever consults this
    // after ParseDepthMessage has returned a real update, never after a
    // std::nullopt one.
    //
    // Safe to read only immediately after the call that produced it: this
    // parser is NOT thread-safe and belongs to one io_context thread, same as
    // every other piece of its state.
    std::optional<uint64_t> LastChainSeq() const { return last_chain_seq_; }

    // The REST depth snapshot body from GET /api/v3/depth:
    // {"lastUpdateId": N, "bids": [[px,qty],...], "asks": [[px,qty],...]}.
    // A different shape from depthUpdate (no e/E/U/u), so its own method.
    // Returns is_snapshot = true, seq = lastUpdateId. Never throws.
    std::optional<BookUpdate> ParseDepthSnapshot(std::string_view body, VenueId venue, InstrumentKey instrument);

    // One Binance bookTicker message: {"u","s","b","B","a","A"} - always both
    // sides, no deltas, so a complete BboQuote comes from every message with
    // no carried-over state. `u` is the order-book update ID. Returns
    // std::nullopt for non-bookTicker messages. Never throws.
    std::optional<BboQuote> ParseBboMessage(std::string_view message, VenueId venue, InstrumentKey instrument);

    // One message from Binance's combined-stream endpoint
    // (/stream?streams=A/B): {"stream":"<name>","data":<the real event>}.
    // Reads `stream` (routing signal) and copies `data`'s raw JSON into this
    // parser's own reusable buffer - for routing a message to
    // ParseDepthMessage or ParseBboMessage before the real parse, the same
    // way PeekTopic/PeekChannel work for Bybit/OKX, except here the payload
    // itself needs unwrapping first since it arrives wrapped in an envelope
    // neither of those methods expects.
    //
    // Returns std::nullopt for a message with no `stream`/`data` (Binance's
    // combined endpoint sends no subscribe acks, but a malformed frame is
    // still possible). `data` is only valid until the next call that touches
    // this parser (this one again, or any Parse*Message call) - copy it out
    // before that if it needs to outlive the immediate routing decision.
    struct CombinedStreamMessage {
        std::string_view stream;  // e.g. "btcusdt@depth@100ms" - view into
                                  // this parser's input_ buffer (Load())
        std::string_view data;    // the `data` object's raw JSON - view into
                                  // data_buffer_ below
    };
    std::optional<CombinedStreamMessage> UnwrapCombinedMessage(std::string_view message);

   private:
    std::optional<uint64_t> last_chain_seq_;

    // Reusable copy target for UnwrapCombinedMessage's `data` substring.
    // raw_json() on `data` points into input_ (Load()'s buffer), which the
    // caller's next Load() - for the real parse - would overwrite; copying
    // it here first, into a buffer that only grows, keeps that safe without
    // a fresh heap allocation per message once warmed up.
    std::string data_buffer_;
};

}  // namespace market_data
