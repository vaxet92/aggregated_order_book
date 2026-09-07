#include <gtest/gtest.h>

#include "continuity.h"

using namespace market_data;

namespace {

// Continuity checks only read seq / prev_seq / is_snapshot, so venue and
// instrument are fixed here - they do not affect the result.
BookUpdate MakeDelta(uint64_t seq, int64_t prev_seq = 0) {
    BookUpdate update{VenueId::BINANCE, MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot), /*reserve_levels=*/0,
                      /*is_snapshot=*/false, seq};
    update.prev_seq = prev_seq;
    return update;
}

BookUpdate MakeSnapshot(uint64_t seq, int64_t prev_seq = -1) {
    BookUpdate update = MakeDelta(seq, prev_seq);
    update.is_snapshot = true;
    return update;
}

}  // namespace

// ---------------------------------------------------------------- Bybit ---

TEST(ContinuityTest, BybitSnapshotResetsAndSetsSequence) {
    uint64_t last_u = 999;
    EXPECT_EQ(CheckBybitContinuity(MakeSnapshot(50), last_u), ContinuityAction::kReset);
    EXPECT_EQ(last_u, 50u);
}

TEST(ContinuityTest, BybitConsecutiveDeltaApplies) {
    uint64_t last_u = 10;
    EXPECT_EQ(CheckBybitContinuity(MakeDelta(11), last_u), ContinuityAction::kApply);
    EXPECT_EQ(last_u, 11u);
}

// Republished with no change - explicitly NOT a gap.
TEST(ContinuityTest, BybitRepeatedSequenceIsIgnoredNotAGap) {
    uint64_t last_u = 10;
    EXPECT_EQ(CheckBybitContinuity(MakeDelta(10), last_u), ContinuityAction::kIgnore);
    EXPECT_EQ(last_u, 10u) << "an ignored message must not advance the sequence";
}

TEST(ContinuityTest, BybitSkippedSequenceIsAGapAndDoesNotAdvance) {
    uint64_t last_u = 10;
    EXPECT_EQ(CheckBybitContinuity(MakeDelta(13), last_u), ContinuityAction::kGap);
    EXPECT_EQ(last_u, 10u) << "a gap must not advance the sequence - the caller resyncs";
}

TEST(ContinuityTest, BybitBackwardsSequenceIsAGap) {
    uint64_t last_u = 10;
    EXPECT_EQ(CheckBybitContinuity(MakeDelta(9), last_u), ContinuityAction::kGap);
}

// ------------------------------------------------------------------ OKX ---

// Walks OKX's own documented example verbatim:
//   Snapshot:                prevSeqId = -1, seqId = 10
//   Incremental 1 (normal):  prevSeqId = 10, seqId = 15
//   Incremental 2 (no upd):  prevSeqId = 15, seqId = 15
//   Incremental 3 (reset):   prevSeqId = 15, seqId = 3
//   Incremental 4 (normal):  prevSeqId = 3,  seqId = 5
TEST(ContinuityTest, OkxDocumentedSequenceExample) {
    uint64_t last_seq = 0;

    EXPECT_EQ(CheckOkxContinuity(MakeSnapshot(10, -1), last_seq), ContinuityAction::kReset);
    EXPECT_EQ(last_seq, 10u);

    EXPECT_EQ(CheckOkxContinuity(MakeDelta(15, 10), last_seq), ContinuityAction::kApply);
    EXPECT_EQ(last_seq, 15u);

    // seqId == prevSeqId: the ~60s keep-alive with empty asks/bids.
    EXPECT_EQ(CheckOkxContinuity(MakeDelta(15, 15), last_seq), ContinuityAction::kIgnore);
    EXPECT_EQ(last_seq, 15u);

    // Maintenance reset: seqId jumps BACKWARDS, but prevSeqId still chains,
    // so this is a normal update - NOT a gap.
    EXPECT_EQ(CheckOkxContinuity(MakeDelta(3, 15), last_seq), ContinuityAction::kApply);
    EXPECT_EQ(last_seq, 3u) << "after a reset the smaller seqId becomes the new chain head";

    EXPECT_EQ(CheckOkxContinuity(MakeDelta(5, 3), last_seq), ContinuityAction::kApply);
    EXPECT_EQ(last_seq, 5u);
}

TEST(ContinuityTest, OkxBrokenChainIsAGap) {
    uint64_t last_seq = 15;
    // prevSeqId doesn't match what we last saw - a message was missed.
    EXPECT_EQ(CheckOkxContinuity(MakeDelta(20, 17), last_seq), ContinuityAction::kGap);
    EXPECT_EQ(last_seq, 15u) << "a gap must not advance the chain";
}

// -1 marks a snapshot; seeing it on a non-snapshot message means the
// message is malformed, not that the chain is intact.
TEST(ContinuityTest, OkxNegativePrevSeqOnNonSnapshotIsAGap) {
    uint64_t last_seq = 15;
    EXPECT_EQ(CheckOkxContinuity(MakeDelta(20, -1), last_seq), ContinuityAction::kGap);
}

TEST(ContinuityTest, OkxFirstMessageBeforeAnySnapshotIsAGap) {
    uint64_t last_seq = 0;  // nothing seen yet
    EXPECT_EQ(CheckOkxContinuity(MakeDelta(20, 19), last_seq), ContinuityAction::kGap);
}

// -------------------------------------------------------------- Binance ---

TEST(ContinuityTest, BinanceSpotChainedEventApplies) {
    uint64_t last_u = 100;
    // U == last_u + 1
    EXPECT_EQ(CheckBinanceSpotContinuity(MakeDelta(110, 101), last_u), ContinuityAction::kApply);
    EXPECT_EQ(last_u, 110u) << "last_u advances to u (final id), not U (first id)";
}

TEST(ContinuityTest, BinanceSpotSkippedEventIsAGap) {
    uint64_t last_u = 100;
    // U == 105, but we expected 101 - events 101..104 were missed.
    EXPECT_EQ(CheckBinanceSpotContinuity(MakeDelta(110, 105), last_u), ContinuityAction::kGap);
    EXPECT_EQ(last_u, 100u);
}

// The bug this file did not catch, for two sessions.
//
// The first event after a REST snapshot STRADDLES it: U is BELOW
// lastUpdateId + 1 while u is above. Binance's own procedure calls that a
// valid join, and ReconcileBinanceSnapshot below has always accepted it - but
// the live path required U == last_u + 1 exactly, so every sync succeeded and
// then failed on its very next message, resyncing forever.
//
// Observed live: "depth synced at lastUpdateId=99584596841" followed
// immediately by "expected U=99584596842, got 99584596810".
TEST(ContinuityTest, BinanceSpotStraddlingEventAfterSnapshotApplies) {
    uint64_t last_u = 100;
    // U = 90 is 11 BELOW last_u + 1, but u = 110 is above it: the event covers
    // some ids already inside the snapshot plus some new ones.
    EXPECT_EQ(CheckBinanceSpotContinuity(MakeDelta(110, 90), last_u), ContinuityAction::kApply);
    EXPECT_EQ(last_u, 110u);
}

// The WS stream can lag the REST snapshot, so events entirely older than
// lastUpdateId keep arriving after we go live. They are already reflected in
// the book - dropping them is right, calling them a gap is not.
TEST(ContinuityTest, BinanceSpotEventFullyInsideTheSnapshotIsIgnored) {
    uint64_t last_u = 100;
    EXPECT_EQ(CheckBinanceSpotContinuity(MakeDelta(95, 90), last_u), ContinuityAction::kIgnore);
    EXPECT_EQ(last_u, 100u) << "an ignored event must not move the sequence";
}

// Boundary: u exactly equals last_u. Nothing new, so nothing to apply.
TEST(ContinuityTest, BinanceSpotEventEndingExactlyAtLastUIsIgnored) {
    uint64_t last_u = 100;
    EXPECT_EQ(CheckBinanceSpotContinuity(MakeDelta(100, 95), last_u), ContinuityAction::kIgnore);
    EXPECT_EQ(last_u, 100u);
}

// One past the boundary is a real gap and must stay one.
TEST(ContinuityTest, BinanceSpotOnePastTheJoinIsStillAGap) {
    uint64_t last_u = 100;
    EXPECT_EQ(CheckBinanceSpotContinuity(MakeDelta(110, 102), last_u), ContinuityAction::kGap);
    EXPECT_EQ(last_u, 100u);
}

// the two implementations of the SAME rule must agree. Reconcile decides
// whether a buffered event can join the snapshot; CheckBinanceSpotContinuity
// decides whether a live one can. They diverged - reconcile accepted the
// straddle, the live check rejected it - and nothing compared them.
//
// This drives one event through both and asserts they reach the same verdict.
TEST(ContinuityTest, BinanceSpotLiveAndReconcilePathsAgreeOnTheJoinRule) {
    constexpr uint64_t kLastUpdateId = 100;

    struct Case {
        uint64_t u;
        int64_t U;
        bool joinable;  // what BOTH paths must say
    };
    const Case cases[] = {
        {110, 101, true},   // exactly contiguous
        {110, 90, true},    // straddling - the case that was broken
        {110, 101, true},   // join at the boundary
        {110, 105, false},  // genuine gap
        {110, 102, false},  // one past the join
    };

    for (const Case& c : cases) {
        uint64_t last_u = kLastUpdateId;
        const auto live = CheckBinanceSpotContinuity(MakeDelta(c.u, c.U), last_u);
        const std::vector<BookUpdate> pending = {MakeDelta(c.u, c.U)};
        const auto reconciled = ReconcileBinanceSnapshot(kLastUpdateId, pending);

        const bool live_joins = (live == ContinuityAction::kApply);
        const bool reconcile_joins = reconciled.has_value() && *reconciled == 0;

        EXPECT_EQ(live_joins, c.joinable) << "live path, u=" << c.u << " U=" << c.U;
        EXPECT_EQ(reconcile_joins, c.joinable) << "reconcile path, u=" << c.u << " U=" << c.U;
        EXPECT_EQ(live_joins, reconcile_joins) << "paths disagree, u=" << c.u << " U=" << c.U;
    }
}

// ---------------------------------------- Binance snapshot reconciliation ---

TEST(ContinuityTest, BinanceReconcileEmptyBufferAppliesSnapshotAlone) {
    std::vector<BookUpdate> pending;
    auto first = ReconcileBinanceSnapshot(100, pending);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0u);
}

// Every buffered event is already inside the snapshot (u <= lastUpdateId).
TEST(ContinuityTest, BinanceReconcileAllEventsStaleAppliesSnapshotAlone) {
    std::vector<BookUpdate> pending{MakeDelta(90, 85), MakeDelta(95, 91), MakeDelta(100, 96)};
    auto first = ReconcileBinanceSnapshot(100, pending);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, pending.size()) << "nothing left to replay after the snapshot";
}

// The documented join: first surviving event has U <= lastUpdateId+1 <= u.
TEST(ContinuityTest, BinanceReconcileExactJoinReturnsFirstUsefulEvent) {
    std::vector<BookUpdate> pending{
        MakeDelta(95, 90),   // stale: u <= 100
        MakeDelta(100, 96),  // stale: u == lastUpdateId
        MakeDelta(105, 101)  // U == lastUpdateId + 1 - the exact join
    };
    auto first = ReconcileBinanceSnapshot(100, pending);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 2u);
}

// An event may straddle the snapshot: U < lastUpdateId+1 <= u. Still valid.
TEST(ContinuityTest, BinanceReconcileStraddlingEventIsAValidJoin) {
    std::vector<BookUpdate> pending{MakeDelta(105, 98)};  // U=98 <= 101 <= u=105
    auto first = ReconcileBinanceSnapshot(100, pending);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 0u);
}

// The snapshot predates the buffer: the first survivor starts after
// lastUpdateId+1, so an update between them was never seen. Unjoinable -
// the caller must refetch a newer snapshot.
TEST(ContinuityTest, BinanceReconcileSnapshotTooOldReturnsNullopt) {
    std::vector<BookUpdate> pending{MakeDelta(110, 105)};  // U=105 > 100+1
    auto first = ReconcileBinanceSnapshot(100, pending);
    EXPECT_FALSE(first.has_value());
}
