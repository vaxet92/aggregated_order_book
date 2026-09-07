#pragma once

// Per-venue sequencing rules: ContinuityAction (kApply/kReset/kIgnore/kGap)
// and the four Check*Continuity functions that classify one incoming depth
// message against each venue's own id scheme - Bybit's u+1, OKX's
// prevSeqId/seqId chaining, Binance spot's U==last_u+1 and Binance futures'
// pu chaining. Plus ReconcileBinanceSnapshot, joining a REST snapshot onto
// buffered WS events. Pure functions: no I/O, no clock, easily unit tested.

#include <cstdint>
#include <optional>
#include <vector>

#include "md_core/types.h"

namespace market_data {

// What a venue's sequencing rules say to do with an incoming depth message.
enum class ContinuityAction {
    kApply,   // in-sequence delta - apply it
    kReset,   // snapshot - clear the book, then apply
    kIgnore,  // keep-alive / no-change - nothing to do, NOT a gap
    kGap,     // sequence broken: the book is wrong, resync
};

// Bybit orderbook.50 - `u` increments by exactly 1 per delta.
//   snapshot        -> kReset
//   u == last_u     -> kIgnore
//   u == last_u + 1 -> kApply
//   otherwise       -> kGap
// Updates last_u on kReset/kApply. Bybit's u == 1 service-restart case is
// normalised into is_snapshot by the parser, so it needs no rule here.
ContinuityAction CheckBybitContinuity(const BookUpdate& update, uint64_t& last_u);

// OKX books - chained by prevSeqId == previous seqId. seqIds are NOT
// contiguous, so Bybit's "+1" rule would be wrong here.
//   snapshot (prevSeqId == -1) -> kReset
//   prevSeqId != last_seq      -> kGap
//   seqId == prevSeqId         -> kIgnore  (~60s keep-alive, empty asks/bids)
//   otherwise                  -> kApply   (incl. seqId < prevSeqId, which is
//                                 a documented maintenance reset, not a gap)
// Updates last_seq on kReset/kApply.
ContinuityAction CheckOkxContinuity(const BookUpdate& update, uint64_t& last_seq);

// Binance SPOT depthUpdate, once live - U == last_u + 1.
// Binance never sends a snapshot on the stream, so kReset/kIgnore never
// occur here; the book is seeded from REST instead (see below).
// Updates last_u on kApply.
//
// FUTURES does NOT use this rule - see CheckBinanceFuturesContinuity below.
ContinuityAction CheckBinanceSpotContinuity(const BookUpdate& update, uint64_t& last_u);

// Binance FUTURES depthUpdate, once live.
//
// futures U/u come from a counter Binance shares across EVERY symbol on
// futures, not just this one - measured live, 2026-09-05: consecutive
// BTCUSDT messages showed U 68 to 323 higher than last_u + 1, on almost every
// message, with nothing actually missing. CheckBinanceSpotContinuity's rule
// (U <= last_u + 1) reads every one of those as a gap - live, that was 14
// resyncs in 14 seconds. U is real, but it answers a question about the
// WHOLE EXCHANGE, not about this symbol, so it cannot be the continuity
// signal here.
//
// `chain_seq` is `pu`: Binance's own answer to "what did you last send me for
// THIS symbol", independent of every other symbol's traffic. The rule is
// simply chain_seq == last_u - no straddle relaxation like spot needs, because
// pu names an exact prior message rather than a coverage range, so there is
// no overlap case to allow for.
//
// Takes raw ids, not a BookUpdate: unlike the other three Check*Continuity
// functions, this rule does not touch prev_seq/is_snapshot at all, and pu is
// deliberately not a BookUpdate field (see BinanceParser::LastChainSeq) - so
// there is nothing on BookUpdate for a third parameter to add.
//
// Updates last_u on kApply, to event_seq (this message's own `u`) - same
// meaning as every other Check*Continuity function's last_seq parameter.
ContinuityAction CheckBinanceFuturesContinuity(uint64_t chain_seq, uint64_t event_seq, uint64_t& last_u);

// Binance snapshot reconciliation. Given the REST snapshot's
// lastUpdateId and the events buffered while it was in flight, returns the
// index of the first event to apply, or nullopt if the snapshot is too old
// to join onto the buffer (caller refetches a newer one).
//
// Events with u <= lastUpdateId are already contained in the snapshot; the
// first survivor must satisfy U <= lastUpdateId + 1 <= u.
std::optional<size_t> ReconcileBinanceSnapshot(uint64_t last_update_id, const std::vector<BookUpdate>& pending);

}  // namespace market_data
