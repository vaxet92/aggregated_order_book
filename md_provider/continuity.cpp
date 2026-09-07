#include "continuity.h"

namespace market_data {

ContinuityAction CheckBybitContinuity(const BookUpdate& update, uint64_t& last_u) {
    // Includes Bybit's u == 1 service-restart case - the parser normalises
    // that into is_snapshot, so nothing venue-specific is needed here.
    if (update.is_snapshot) {
        last_u = update.seq;
        return ContinuityAction::kReset;
    }
    if (update.seq == last_u) {
        // Republished with no change. Documented for level 1 on linear/
        // inverse perps; shouldn't occur on spot orderbook.50, but it is
        // explicitly not a gap.
        return ContinuityAction::kIgnore;
    }
    if (update.seq == last_u + 1) {
        last_u = update.seq;
        return ContinuityAction::kApply;
    }
    return ContinuityAction::kGap;
}

ContinuityAction CheckOkxContinuity(const BookUpdate& update, uint64_t& last_seq) {
    if (update.is_snapshot) {
        last_seq = update.seq;  // snapshot carries prevSeqId == -1
        return ContinuityAction::kReset;
    }
    if (update.prev_seq < 0 || static_cast<uint64_t>(update.prev_seq) != last_seq) {
        return ContinuityAction::kGap;
    }
    if (update.seq == static_cast<uint64_t>(update.prev_seq)) {
        // ~60s keep-alive: empty asks/bids, seqId == prevSeqId. Chain is
        // intact, nothing to apply.
        return ContinuityAction::kIgnore;
    }
    // Normal update. seqId may be SMALLER than prevSeqId after an OKX
    // maintenance sequence reset - documented as normal, and the chain still
    // holds, so it must NOT be treated as a gap.
    last_seq = update.seq;
    return ContinuityAction::kApply;
}

ContinuityAction CheckBinanceSpotContinuity(const BookUpdate& update, uint64_t& last_u) {
    // U and u answer DIFFERENT questions. `prev_seq` (U) is where this
    // event's coverage STARTS, `seq` (u) is where it ENDS. Continuity is
    // checked against U and advanced by u, and the two diverge at exactly one
    // place - the snapshot boundary.
    //
    // Requiring U == last_u + 1 assumes events never overlap. That holds in
    // steady state and is FALSE for the first event after a REST snapshot,
    // which straddles it: U < lastUpdateId+1 <= u. Binance's own "manage a
    // local order book" procedure allows exactly that, and rejecting it made
    // every sync fail on its very next message and resync forever.

    // Entirely inside what we already hold. The WS stream can lag the REST
    // snapshot, so events older than lastUpdateId keep arriving after we go
    // live. They are already reflected in the book - drop them rather than
    // reporting a gap that does not exist.
    if (update.seq <= last_u) {
        return ContinuityAction::kIgnore;
    }

    // Contiguous (U == last_u + 1) or straddling (U < last_u + 1 <= u). Both
    // apply. `<=` subsumes the steady-state case, so this is simpler than the
    // equality it replaces, not more permissive in any way that matters:
    // re-writing a level we already hold is safe because Binance quantities
    // are ABSOLUTE, not increments.
    //
    // The same rule already lives in ReconcileBinanceSnapshot below, which is
    // how the buffered path got this right while the live path did not.
    if (static_cast<uint64_t>(update.prev_seq) <= last_u + 1) {
        last_u = update.seq;
        return ContinuityAction::kApply;
    }

    // U > last_u + 1: events between last_u and U were genuinely missed, so
    // the book is now WRONG rather than merely stale.
    return ContinuityAction::kGap;
}

ContinuityAction CheckBinanceFuturesContinuity(uint64_t chain_seq, uint64_t event_seq, uint64_t& last_u) {
    // Same "the WS can lag the REST snapshot" case CheckBinanceSpotContinuity
    // guards against: an event entirely inside what was already emitted
    // (snapshot + replayed buffer). Already reflected in the book - ignore,
    // not a gap. event_seq (u) is a real per-symbol high-water mark even
    // though its numeric SPACING is shared across symbols, so this compare is
    // still meaningful.
    if (event_seq <= last_u) {
        return ContinuityAction::kIgnore;
    }

    // The real rule: THIS message must chain from the last one WE actually
    // saw for this symbol. Verified live, 906 combined Bybit+OKX futures
    // messages showed their existing rules already hold unchanged - Binance
    // futures is the one venue+market that needed a genuinely different
    // signal, and pu is it.
    if (chain_seq == last_u) {
        last_u = event_seq;
        return ContinuityAction::kApply;
    }

    return ContinuityAction::kGap;
}

std::optional<size_t> ReconcileBinanceSnapshot(uint64_t last_update_id, const std::vector<BookUpdate>& pending) {
    // Everything with u <= lastUpdateId is already inside the snapshot.
    size_t first = 0;
    while (first < pending.size() && pending[first].seq <= last_update_id) {
        ++first;
    }

    // No events left to apply: the snapshot is simply newer than everything
    // buffered. Valid - apply the snapshot alone.
    if (first == pending.size()) {
        return first;
    }

    // The first survivor must straddle the snapshot: U <= lastUpdateId + 1.
    // If it starts after that, an update was missed between the snapshot and
    // the buffer, and this snapshot cannot be joined onto it.
    if (static_cast<uint64_t>(pending[first].prev_seq) > last_update_id + 1) {
        return std::nullopt;
    }
    return first;
}

}  // namespace market_data
