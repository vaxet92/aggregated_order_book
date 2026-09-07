#include <gtest/gtest.h>

#include "seq_dedup.h"

using namespace market_data;

// Duplicate rejection for redundant connections. The invariant under test:
// with N sockets carrying identical messages, exactly ONE copy of each id is
// accepted, and a venue sequence reset is not mistaken for a duplicate.
//
// Naming below: conn0/conn1/conn2 are the connection indices passed to
// Accept(). They only affect health reporting, never accept/reject.

// ------------------------------------------------------------- basics ---

TEST(SeqDedupTest, FirstMessageAccepted) {
    SeqDedup dedup;
    // Nothing seen yet, so there is no high-water mark to compare against.
    // Note id 0 is accepted here - `seen_` exists precisely so that a first
    // message cannot be confused with "0 <= last_".
    EXPECT_TRUE(dedup.Accept(0, 0, false));
    EXPECT_FALSE(dedup.Accept(0, 1, false));
}

TEST(SeqDedupTest, DuplicateCopiesDropped) {
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(4057, 0, false));
    EXPECT_FALSE(dedup.Accept(4057, 1, false));
    EXPECT_FALSE(dedup.Accept(4057, 2, false));

    EXPECT_TRUE(dedup.Accept(4058, 2, false));  // whichever socket wins is fine
    EXPECT_FALSE(dedup.Accept(4058, 0, false));
}

TEST(SeqDedupTest, SingleConnectionPassesEverything) {
    // "connections": 1 is the default. Redundancy off must behave exactly as
    // it did before this filter existed: every message through, nothing
    // dropped, no drop streak accumulating.
    SeqDedup dedup;
    for (uint64_t id = 100; id < 200; ++id) {
        EXPECT_TRUE(dedup.Accept(id, 0, false)) << "id " << id;
    }
    EXPECT_EQ(dedup.ConsecutiveDrops(), 0u);
    EXPECT_FALSE(dedup.LooksStuck());
}

// ------------------------------------------------------ the `<=` rule ---

TEST(SeqDedupTest, LaggingConnectionByTwoIsDropped) {
    // THE regression test for the design decision: `<=` rather than `!=`.
    //
    // One io_context thread reads all N sockets and processes whatever is
    // ready, so one connection delivering several frames before another
    // delivers its first copy is normal event-loop batching, not a rare race.
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(4056, 0, false));
    EXPECT_TRUE(dedup.Accept(4057, 0, false));
    EXPECT_TRUE(dedup.Accept(4058, 0, false));  // conn0 batched three frames

    // conn1 is now two behind. An `id != last_` rule would accept both of
    // these, apply them a second time, and the continuity check downstream
    // would read the result as a gap and trigger a resync - the exact outage
    // redundant connections exist to prevent.
    EXPECT_FALSE(dedup.Accept(4057, 1, false));
    EXPECT_FALSE(dedup.Accept(4058, 1, false));
}

TEST(SeqDedupTest, ConnectionDeathNoGap) {
    // The whole point of redundancy. conn0 runs ahead to 4100 and then its
    // socket dies; conn1 is still back at 4057. Everything conn1 replays must
    // be dropped, and the first genuinely new id must pass - no gap, no
    // resync, no REST refetch.
    SeqDedup dedup;
    for (uint64_t id = 4057; id <= 4100; ++id) {
        EXPECT_TRUE(dedup.Accept(id, 0, false)) << "id " << id;
    }

    for (uint64_t id = 4058; id <= 4100; ++id) {
        EXPECT_FALSE(dedup.Accept(id, 1, false)) << "replayed id " << id;
    }
    EXPECT_TRUE(dedup.Accept(4101, 1, false));
}

// ------------------------------------------- snapshots vs true resets ---

TEST(SeqDedupTest, StaleSnapshotFromLateSocketIsDropped) {
    // Regression for a live failure: "[BYBIT] depth gap: expected u=42348956,
    // got 42348963".
    //
    // All N sockets are CREATED before any of them CONNECTS, so creation
    // order says nothing about who delivers first. A socket that connects
    // last opens with a snapshot whose id is BEHIND what the healthy sockets
    // have already delivered.
    //
    // a plain snapshot is NOT a sequence reset. Its id moves forward in
    // the venue's own numbering - it is merely behind us - so the ordinary
    // `<=` rule is the correct and sufficient test.
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(42348950, 2, /*is_reset=*/false));  // socket 2 snapshot, first ever
    for (uint64_t id = 42348951; id <= 42348962; ++id) {
        EXPECT_TRUE(dedup.Accept(id, 2, false)) << "id " << id;
    }

    // Socket 0 finally connects, opening with a snapshot from before we
    // reached 42348962.
    EXPECT_FALSE(dedup.Accept(42348955, 0, /*is_reset=*/false));

    // The healthy socket continues with no gap.
    EXPECT_TRUE(dedup.Accept(42348963, 2, false));
}

TEST(SeqDedupTest, ResetFlagOnAStaleSnapshotIsWhatCausedTheGap) {
    // ASSERTS THE BROKEN BEHAVIOUR ON PURPOSE - the same sequence with
    // is_reset=true, which is what the first implementation passed for every
    // snapshot. Documents why the flag was narrowed to genuine sequence
    // resets only (Bybit u == 1, OKX seqId < prevSeqId).
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(42348962, 2, false));

    // Wrongly flagged: the high-water mark is dragged BACKWARDS onto a stale
    // snapshot.
    EXPECT_TRUE(dedup.Accept(42348955, 0, /*is_reset=*/true));

    // The next healthy message is now accepted out of sequence. Downstream,
    // continuity expects 42348956 and sees 42348963 - the observed gap.
    EXPECT_TRUE(dedup.Accept(42348963, 2, false));
}

// ----------------------------------------------------------- resets ---

TEST(SeqDedupTest, ResetMovesHighWaterMarkDown) {
    // OKX documented maintenance reset: prevSeqId=15, seqId=3. Not a gap -
    // the chain still links - so the filter must take it even though 3 is far
    // below the high-water mark.
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(15, 0, false));
    EXPECT_TRUE(dedup.Accept(3, 0, /*is_reset=*/true));
}

TEST(SeqDedupTest, ResetDuplicatesDropped) {
    // the other N-1 connections deliver the SAME reset moments later.
    // Clearing state on reset instead of moving the mark down would let each
    // through, and every one would re-apply a full snapshot.
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(15, 0, false));
    EXPECT_TRUE(dedup.Accept(3, 0, /*is_reset=*/true));
    EXPECT_FALSE(dedup.Accept(3, 1, /*is_reset=*/true));
    EXPECT_FALSE(dedup.Accept(3, 2, /*is_reset=*/true));
}

TEST(SeqDedupTest, PostResetSequenceResumes) {
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(15, 0, false));
    EXPECT_TRUE(dedup.Accept(3, 0, /*is_reset=*/true));
    EXPECT_FALSE(dedup.Accept(3, 1, /*is_reset=*/true));

    // ids 4 and 5 were seen before the reset, but history is meaningless now.
    EXPECT_TRUE(dedup.Accept(4, 0, false));
    EXPECT_FALSE(dedup.Accept(4, 1, false));
    EXPECT_TRUE(dedup.Accept(5, 1, false));
}

TEST(SeqDedupTest, BybitRestartSnapshotAccepted) {
    // Bybit's u == 1 service restart, normalised into is_snapshot by the
    // parser. Without the reset flag this would be dropped as "1 <= 4056" and
    // the book would freeze at the pre-restart state.
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(4056, 0, false));
    EXPECT_TRUE(dedup.Accept(1, 0, /*is_reset=*/true));
    EXPECT_FALSE(dedup.Accept(1, 1, /*is_reset=*/true));
    EXPECT_TRUE(dedup.Accept(2, 0, false));
}

// ------------------------------------------------------------ health ---

TEST(SeqDedupTest, MaskReportsDeliveringConnections) {
    SeqDedup dedup;
    dedup.Accept(4057, 0, false);
    dedup.Accept(4057, 1, false);
    dedup.Accept(4057, 2, false);

    // conn0 now goes quiet: 4058 is delivered by conn1 and conn2 only.
    //
    // The mask for 4057 is published here, not earlier - health is one
    // message late by construction, because a message's copies are only all
    // counted once the next id proves no more are coming.
    dedup.Accept(4058, 1, false);  // accepted, publishes 4057's mask
    EXPECT_EQ(dedup.LastMask(), 0b111);
    EXPECT_EQ(dedup.LastConnectionCount(), 3u);

    dedup.Accept(4058, 2, false);  // duplicate
    dedup.Accept(4059, 1, false);  // accepted, publishes 4058's mask
    EXPECT_EQ(dedup.LastMask(), 0b110);
    EXPECT_EQ(dedup.LastConnectionCount(), 2u);
}

TEST(SeqDedupTest, OutOfRangeConnectionIndexDoesNotCorruptMask) {
    // "connections" is capped at kMaxConnections (8), so this is unreachable
    // through config validation. It contributes no bit rather than shifting
    // past the width of the mask: health may under-report, but there is no
    // UB and the accept/reject decision is untouched.
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(4057, 99, false));
    EXPECT_FALSE(dedup.Accept(4057, 0, false));
    dedup.Accept(4058, 0, false);
    EXPECT_EQ(dedup.LastMask(), 0b001);
}

TEST(SeqDedupTest, DropStreakResetsOnAccept) {
    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(4057, 0, false));
    dedup.Accept(4057, 1, false);
    dedup.Accept(4057, 2, false);
    EXPECT_EQ(dedup.ConsecutiveDrops(), 2u);

    EXPECT_TRUE(dedup.Accept(4058, 0, false));
    EXPECT_EQ(dedup.ConsecutiveDrops(), 0u);
}

TEST(SeqDedupTest, LooksStuckAfterMissedReset) {
    // This test ASSERTS THE BROKEN BEHAVIOUR ON PURPOSE.
    //
    // If a venue resets its sequence and is_reset is not passed, every
    // following id sits below the high-water mark, so every message is
    // dropped - forever. The book freezes and nothing errors, because the
    // filter is doing exactly what it was told.
    //
    // The filter cannot detect this itself. The drop streak is the only
    // defence, and this test exists to prove the alarm fires.
    // The high-water mark must stay above every replayed id for the whole
    // run, otherwise the stream climbs back past it and the streak resets -
    // which is the benign case, not the one under test.
    constexpr uint64_t kBeforeReset = 1'000'000;

    SeqDedup dedup;
    EXPECT_TRUE(dedup.Accept(kBeforeReset, 0, false));

    for (uint64_t id = 3; id < 3 + SeqDedup::kSuspiciousDropStreak; ++id) {
        EXPECT_FALSE(dedup.Accept(id, 0, /*is_reset=*/false));
    }
    EXPECT_TRUE(dedup.LooksStuck());
}
