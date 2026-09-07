#include <gtest/gtest.h>

#include "md_core/venue_health.h"

using namespace market_data;

namespace {

// One second, in nanoseconds. A stand-in threshold for these tests only -
// the real per-venue values are config, and are to be set from measured
// inter-arrival gaps rather than picked here.
constexpr int64_t kThresholdNs = 1'000'000'000;

// steady_clock's epoch is arbitrary and machine-local, so a realistic "now"
// is some large number with no meaning of its own. Using a large base rather
// than starting at 0 keeps the tests honest: nothing here may depend on the
// timestamps being small or on the epoch being any particular point.
constexpr int64_t kNow = 987'654'321'000'000;

}  // namespace

// --- never seen -------------------------------------------------------------

TEST(VenueHealthTest, NeverSeenIsNoData) {
    EXPECT_EQ(ClassifyVenue(/*last=*/0, kNow, kThresholdNs), VenueHealth::kNoData);
}

// The reason last_mono_ns == 0 is checked BEFORE the subtraction. A venue
// that never spoke has no age; computing one gives the process uptime, which
// is both meaningless and always large enough to look stale. A venue whose
// subscription was silently rejected would then be reported as a dead
// exchange - the wrong problem to go looking for.
TEST(VenueHealthTest, NeverSeenStaysNoDataEvenAfterLongUptime) {
    const int64_t one_day_ns = 86'400ll * 1'000'000'000ll;
    EXPECT_EQ(ClassifyVenue(/*last=*/0, one_day_ns, kThresholdNs), VenueHealth::kNoData);
}

// --- normal operation -------------------------------------------------------

TEST(VenueHealthTest, FreshIsLive) {
    const int64_t last = kNow - 1'000'000;  // 1ms ago
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, ZeroAgeIsLive) {
    EXPECT_EQ(ClassifyVenue(kNow, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, WellPastThresholdIsStale) {
    const int64_t last = kNow - 10 * kThresholdNs;
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kStale);
}

// --- the boundary -----------------------------------------------------------
//
// These two pin down the `>` vs `>=` choice. Either is defensible; what is
// not defensible is nobody knowing which one the code does. If the
// comparison is ever changed, exactly one of these two fails, which is the
// point of writing both.

TEST(VenueHealthTest, ExactlyAtThresholdIsLive) {
    const int64_t last = kNow - kThresholdNs;  // age == threshold
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, OneNanosecondPastThresholdIsStale) {
    const int64_t last = kNow - kThresholdNs - 1;
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kStale);
}

// --- clock races ------------------------------------------------------------

// The provider stamps recv_mono_ns on its own thread; the publisher reads
// `now` on another. Their steady_clock readings can cross, giving an age
// that is slightly negative. That must read as live - a message from the
// immediate future is not stale - and it must not wrap into a huge positive
// age, which is why these are int64_t and not an unsigned type.
TEST(VenueHealthTest, NegativeAgeIsLive) {
    const int64_t last = kNow + 5'000;  // stamped 5us "after" now
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, LargeNegativeAgeIsStillLive) {
    const int64_t last = kNow + 10 * kThresholdNs;
    EXPECT_EQ(ClassifyVenue(last, kNow, kThresholdNs), VenueHealth::kLive);
}

// --- admission --------------------------------------------------------------

TEST(VenueHealthTest, OnlyLiveIsAdmissible) {
    EXPECT_TRUE(IsAdmissible(VenueHealth::kLive));
    EXPECT_FALSE(IsAdmissible(VenueHealth::kStale));
    // kNoData must not sneak into the merge on the grounds that its book is
    // empty anyway. Relying on "empty book contributes nothing" would make
    // admission depend on a second, unrelated invariant holding.
    EXPECT_FALSE(IsAdmissible(VenueHealth::kNoData));
    EXPECT_FALSE(IsAdmissible(VenueHealth::kDisconnected));
    EXPECT_FALSE(IsAdmissible(VenueHealth::kResyncing));
}

// Deliberately phrased as "everything that is not kLive is rejected" rather
// than checking the four values one by one. IsAdmissible is currently written
// as `== kLive`, but the tempting rewrite is `!= kStale` - and under that,
// every state added later would silently default to ADMITTED. This asserts
// the allow-list shape, so that rewrite fails here instead of quietly letting
// a disconnected venue into the merge.
TEST(VenueHealthTest, AdmissionIsAnAllowListNotADenyList) {
    // Deliberately scans a range WIDER than the enum rather than listing the
    // states by name. A hand-written list has to be remembered every time a
    // state is added - and it silently keeps passing when it is forgotten,
    // because the new state is simply not tested. Scanning the range means a
    // future state is covered the moment it exists, with no edit here.
    //
    // Values outside the enum are safe to pass: IsAdmissible is a comparison
    // against kLive, so anything unrecognised is rejected, which is the
    // property being asserted.
    int admitted = 0;
    for (uint8_t raw = 0; raw < 16; ++raw) {
        if (IsAdmissible(static_cast<VenueHealth>(raw))) {
            ++admitted;
        }
    }
    EXPECT_EQ(admitted, 1) << "exactly one state may be admissible, and it must be kLive";
    EXPECT_TRUE(IsAdmissible(VenueHealth::kLive));
}

// kResyncing means "we tore this venue down on purpose because its book is
// wrong". It must be excluded as firmly as any other non-live state.
TEST(VenueHealthTest, ResyncingIsNotAdmissible) {
    EXPECT_FALSE(IsAdmissible(VenueHealth::kResyncing));
}

// kResyncing comes from the resync path, never from the classifier - the
// timer and the socket count cannot know that we deliberately dropped a
// stream. Pinning it so the two sources of a verdict stay separate, the same
// way ClassifyVenueNeverReturnsDisconnected does for the timer.
TEST(VenueHealthTest, ClassifyFeedNeverReturnsResyncing) {
    EXPECT_NE(ClassifyFeed(true, kNow, kNow, kThresholdNs), VenueHealth::kResyncing);
    EXPECT_NE(ClassifyFeed(true, 0, kNow, kThresholdNs), VenueHealth::kResyncing);
    EXPECT_NE(ClassifyFeed(false, kNow, kNow, kThresholdNs), VenueHealth::kResyncing);
    EXPECT_NE(ClassifyFeed(true, kNow - 10 * kThresholdNs, kNow, kThresholdNs), VenueHealth::kResyncing);
}

// Every state has a distinct name - a duplicate would make two different
// conditions indistinguishable in the logs, which is exactly where these are
// read.
TEST(VenueHealthTest, EveryStateHasItsOwnName) {
    EXPECT_STREQ(ToString(VenueHealth::kNoData), "NO_DATA");
    EXPECT_STREQ(ToString(VenueHealth::kLive), "LIVE");
    EXPECT_STREQ(ToString(VenueHealth::kDisconnected), "DISCONNECTED");
    EXPECT_STREQ(ToString(VenueHealth::kResyncing), "RESYNCING");
    EXPECT_STREQ(ToString(VenueHealth::kStale), "STALE");
}

// ClassifyVenue answers the TIMER question only, so it can never return
// kDisconnected - that verdict comes from connection state, which the
// predicate is not given and deliberately does not know about. Pinning it
// here so the two sources of a verdict stay separate.
TEST(VenueHealthTest, ClassifyVenueNeverReturnsDisconnected) {
    EXPECT_NE(ClassifyVenue(0, kNow, kThresholdNs), VenueHealth::kDisconnected);
    EXPECT_NE(ClassifyVenue(kNow, kNow, kThresholdNs), VenueHealth::kDisconnected);
    EXPECT_NE(ClassifyVenue(kNow - 10 * kThresholdNs, kNow, kThresholdNs), VenueHealth::kDisconnected);
}

// --- compile time -----------------------------------------------------------
//
// Not a runtime assertion: these fail the BUILD if the function stops being
// constexpr. It is declared constexpr so it can inline inside the merge
// loop, and losing that silently would be easy to miss.

static_assert(ClassifyVenue(0, kNow, kThresholdNs) == VenueHealth::kNoData);
static_assert(ClassifyVenue(kNow - kThresholdNs, kNow, kThresholdNs) == VenueHealth::kLive);
static_assert(ClassifyVenue(kNow - kThresholdNs - 1, kNow, kThresholdNs) == VenueHealth::kStale);
static_assert(IsAdmissible(VenueHealth::kLive));
static_assert(!IsAdmissible(VenueHealth::kStale));

// ------------------------------------------------------------ ClassifyFeed ---
//
// ClassifyFeed adds connection state on top of ClassifyVenue. The tests above
// still cover the timer half; these cover the combination, and specifically
// the asymmetry between the two signals.

TEST(VenueHealthTest, DisconnectedBeatsAFreshTimestamp) {
    // The stamp says the venue spoke a microsecond ago. It does not matter:
    // the socket is gone, so nothing more can arrive and the levels we hold
    // can no longer be corrected. Connection state settles it.
    EXPECT_EQ(ClassifyFeed(/*connected=*/false, kNow - 1'000, kNow, kThresholdNs), VenueHealth::kDisconnected);
}

TEST(VenueHealthTest, DisconnectedWithNoDataIsStillDisconnected) {
    EXPECT_EQ(ClassifyFeed(/*connected=*/false, /*last=*/0, kNow, kThresholdNs), VenueHealth::kDisconnected);
}

// The other half of the asymmetry, and the reason connection state cannot
// replace the watchdog. The socket is open, so signal 1 has nothing to say -
// but no data has arrived for ten times the backstop. This is the zombie
// connection: TCP ESTABLISHED, publisher wedged. Only the timer sees it.
TEST(VenueHealthTest, ConnectedButSilentIsStaleNotLive) {
    EXPECT_EQ(ClassifyFeed(/*connected=*/true, kNow - 10 * kThresholdNs, kNow, kThresholdNs), VenueHealth::kStale);
}

TEST(VenueHealthTest, ConnectedAndFreshIsLive) {
    EXPECT_EQ(ClassifyFeed(/*connected=*/true, kNow - 1'000'000, kNow, kThresholdNs), VenueHealth::kLive);
}

TEST(VenueHealthTest, ConnectedWithNoDataIsNoData) {
    // The startup state: sockets created, nothing received yet. Must not read
    // as kStale - a venue that never spoke needs a different response than one
    // that spoke and stopped.
    EXPECT_EQ(ClassifyFeed(/*connected=*/true, /*last=*/0, kNow, kThresholdNs), VenueHealth::kNoData);
}

// The boundary is inherited from ClassifyVenue rather than re-implemented.
// If ClassifyFeed ever grew its own comparison, these would diverge from
// ExactlyAtThresholdIsLive / OneNanosecondPastThresholdIsStale above.
TEST(VenueHealthTest, ClassifyFeedInheritsTheTimerBoundary) {
    EXPECT_EQ(ClassifyFeed(true, kNow - kThresholdNs, kNow, kThresholdNs), VenueHealth::kLive);
    EXPECT_EQ(ClassifyFeed(true, kNow - kThresholdNs - 1, kNow, kThresholdNs), VenueHealth::kStale);
}

// Bybit documents a 3-second republish on L1, so a backstop derived from it
// makes silence past that interval evidence rather than a guess. Encoded as a
// test so the derivation is visible next to the number.
TEST(VenueHealthTest, BybitL1BackstopDerivedFromItsThreeSecondRepublish) {
    constexpr int64_t kOneSecondNs = 1'000'000'000;
    constexpr int64_t kBybitL1Republish = 3 * kOneSecondNs;
    constexpr int64_t kBackstop = 10 * kOneSecondNs;  // ~3x the documented interval

    // Quiet market: Bybit republishes on schedule, so the stamp keeps moving
    // even though the book never changes.
    EXPECT_EQ(ClassifyFeed(true, kNow - kBybitL1Republish, kNow, kBackstop), VenueHealth::kLive);

    // Two republish intervals missed - still inside the margin.
    EXPECT_EQ(ClassifyFeed(true, kNow - 2 * kBybitL1Republish, kNow, kBackstop), VenueHealth::kLive);

    // Past the backstop: the venue broke a promise it makes about itself.
    EXPECT_EQ(ClassifyFeed(true, kNow - 4 * kBybitL1Republish, kNow, kBackstop), VenueHealth::kStale);
}

// Same shape, OKX's ~60s keepalive on the books channel. The point of this
// test is the CONTRAST with Bybit above: the same rule, a backstop 9x longer,
// which is why OKX depth cannot rely on the timer alone.
TEST(VenueHealthTest, OkxDepthBackstopIsMuchLongerThanBybitL1) {
    constexpr int64_t kOneSecondNs = 1'000'000'000;
    constexpr int64_t kOkxKeepalive = 60 * kOneSecondNs;
    constexpr int64_t kBackstop = 90 * kOneSecondNs;

    EXPECT_EQ(ClassifyFeed(true, kNow - kOkxKeepalive, kNow, kBackstop), VenueHealth::kLive);
    EXPECT_EQ(ClassifyFeed(true, kNow - 2 * kOkxKeepalive, kNow, kBackstop), VenueHealth::kStale);

    // A feed that died 30 seconds ago is still called live here. That is not a
    // bug in the rule - it is the honest limit of a 60s keepalive, and the
    // reason cross-venue corroboration carries the
    // load for OKX depth.
    EXPECT_EQ(ClassifyFeed(true, kNow - 30 * kOneSecondNs, kNow, kBackstop), VenueHealth::kLive);
}

static_assert(ClassifyFeed(false, kNow, kNow, kThresholdNs) == VenueHealth::kDisconnected);
static_assert(ClassifyFeed(true, kNow, kNow, kThresholdNs) == VenueHealth::kLive);
static_assert(ClassifyFeed(true, 0, kNow, kThresholdNs) == VenueHealth::kNoData);
