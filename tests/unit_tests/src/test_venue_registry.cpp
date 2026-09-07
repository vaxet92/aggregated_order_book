#include <gtest/gtest.h>

#include "types/venue_registry.h"

#include <atomic>
#include <string>
#include <thread>

namespace {

// Registration order is what assigns slots, so the tests spell the order out
// rather than relying on any relationship to VenueId. In the deployed split
// there is no such relationship at all - ZmqCoreIngress registers each venue as
// its provider connects, in connection order - and nothing in md_core may
// depend on one.
constexpr std::string_view kBinance = "BINANCE";
constexpr std::string_view kBybit = "BYBIT";
constexpr std::string_view kOkx = "OKX";

}  // namespace

// --- empty ------------------------------------------------------------------

TEST(VenueRegistryTest, StartsEmpty) {
    const VenueRegistry registry;
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_TRUE(registry.empty());
    EXPECT_FALSE(registry.Find(kBinance).has_value());
}

// --- assignment -------------------------------------------------------------

TEST(VenueRegistryTest, AssignsDenseSlotsInRegistrationOrder) {
    VenueRegistry registry;

    const auto binance = registry.Register(kBinance);
    const auto bybit = registry.Register(kBybit);
    const auto okx = registry.Register(kOkx);

    ASSERT_TRUE(binance.has_value());
    ASSERT_TRUE(bybit.has_value());
    ASSERT_TRUE(okx.has_value());

    // Dense and gapless: these index md_core's per-venue arrays directly, so a
    // hole would mean an array entry no venue owns and every loop reading it.
    EXPECT_EQ(VenueSlotIndex(*binance), 0u);
    EXPECT_EQ(VenueSlotIndex(*bybit), 1u);
    EXPECT_EQ(VenueSlotIndex(*okx), 2u);
    EXPECT_EQ(registry.size(), 3u);
}

TEST(VenueRegistryTest, SlotsFollowRegistrationOrderNotVenueIdOrder) {
    VenueRegistry registry;

    // Deliberately NOT enum order. Today VenueId gives BINANCE=0, BYBIT=1,
    // OKX=2, and registering in that order makes slot == enum value - which is
    // exactly the coincidence that would hide a mix-up between the two types.
    // Registering backwards proves nothing in the registry depends on it.
    const auto okx = registry.Register(kOkx);
    const auto bybit = registry.Register(kBybit);
    const auto binance = registry.Register(kBinance);

    ASSERT_TRUE(okx.has_value());
    ASSERT_TRUE(bybit.has_value());
    ASSERT_TRUE(binance.has_value());

    EXPECT_EQ(VenueSlotIndex(*okx), 0u);
    EXPECT_EQ(VenueSlotIndex(*bybit), 1u);
    EXPECT_EQ(VenueSlotIndex(*binance), 2u);
}

// --- idempotence ------------------------------------------------------------

TEST(VenueRegistryTest, RegisterIsIdempotent) {
    VenueRegistry registry;

    const auto first = registry.Register(kBinance);
    const auto second = registry.Register(kBinance);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(VenueRegistryTest, ReconnectingProviderLandsOnItsOriginalSlot) {
    VenueRegistry registry;

    const auto binance = registry.Register(kBinance);
    const auto bybit = registry.Register(kBybit);
    ASSERT_TRUE(binance.has_value());
    ASSERT_TRUE(bybit.has_value());

    // A provider process dies and its replacement dials back in. It
    // must be given the SAME slot: the slot is what md_core's books, health
    // verdicts and attribution are keyed on, so a new slot would strand the
    // old book and publish the venue's prices under a slot nobody feeds.
    const auto binance_again = registry.Register(kBinance);
    ASSERT_TRUE(binance_again.has_value());
    EXPECT_EQ(*binance_again, *binance);
    EXPECT_EQ(registry.size(), 2u);
}

// --- lookup -----------------------------------------------------------------

TEST(VenueRegistryTest, FindReturnsTheRegisteredSlot) {
    VenueRegistry registry;

    const auto binance = registry.Register(kBinance);
    const auto bybit = registry.Register(kBybit);
    ASSERT_TRUE(binance.has_value());
    ASSERT_TRUE(bybit.has_value());

    EXPECT_EQ(registry.Find(kBinance), binance);
    EXPECT_EQ(registry.Find(kBybit), bybit);
    EXPECT_FALSE(registry.Find(kOkx).has_value());
}

TEST(VenueRegistryTest, MatchingIsExactAndCaseSensitive) {
    VenueRegistry registry;
    ASSERT_TRUE(registry.Register(kBinance).has_value());

    // Callers normalise before registering (VenueConverter::ToVenueString
    // yields upper case). A near-miss must MISS rather than silently resolve
    // to a neighbour - on the wire this name arrives from a remote process,
    // and quietly accepting "binance" as "BINANCE" would hide a real mismatch.
    EXPECT_FALSE(registry.Find("binance").has_value());
    EXPECT_FALSE(registry.Find("BINANC").has_value());
    EXPECT_FALSE(registry.Find("BINANCE ").has_value());
    EXPECT_FALSE(registry.Find("").has_value());
}

TEST(VenueRegistryTest, NameRoundTrips) {
    VenueRegistry registry;

    const auto binance = registry.Register(kBinance);
    const auto okx = registry.Register(kOkx);
    ASSERT_TRUE(binance.has_value());
    ASSERT_TRUE(okx.has_value());

    EXPECT_EQ(registry.Name(*binance), kBinance);
    EXPECT_EQ(registry.Name(*okx), kOkx);
}

TEST(VenueRegistryTest, NameOfUnregisteredSlotIsEmpty) {
    VenueRegistry registry;
    ASSERT_TRUE(registry.Register(kBinance).has_value());

    // Slot 1 exists as storage but has been assigned to nobody. Returning an
    // empty view rather than the array's default-constructed string by
    // accident keeps "no such venue" distinguishable in a log line.
    EXPECT_TRUE(registry.Name(static_cast<VenueSlot>(1)).empty());
    EXPECT_TRUE(registry.Name(static_cast<VenueSlot>(kMaxVenues - 1)).empty());
}

TEST(VenueRegistryTest, NameStaysValidAcrossLaterRegistrations) {
    VenueRegistry registry;

    const auto binance = registry.Register(kBinance);
    ASSERT_TRUE(binance.has_value());
    const std::string_view name = registry.Name(*binance);

    // The fixed-capacity array is what makes this safe: registering more
    // venues never reallocates, so a view handed out earlier still points at
    // live storage. With a std::vector this is exactly where it would dangle.
    //
    // Bounded by kMaxVenues rather than a literal: filling the registry to
    // exactly capacity is the strongest version of this test, and it does not
    // have to be revisited when the cap changes.
    for (size_t i = 1; i < kMaxVenues; ++i) {
        ASSERT_TRUE(registry.Register("VENUE_" + std::to_string(i)).has_value());
    }
    EXPECT_EQ(registry.size(), kMaxVenues);
    EXPECT_EQ(name, kBinance);
}

// --- capacity ---------------------------------------------------------------

TEST(VenueRegistryTest, FillsToCapacity) {
    VenueRegistry registry;

    for (size_t i = 0; i < kMaxVenues; ++i) {
        const auto slot = registry.Register("VENUE_" + std::to_string(i));
        ASSERT_TRUE(slot.has_value()) << "failed at " << i;
        EXPECT_EQ(VenueSlotIndex(*slot), i);
    }
    EXPECT_EQ(registry.size(), kMaxVenues);
}

TEST(VenueRegistryTest, RegisterBeyondCapacityReturnsNullopt) {
    VenueRegistry registry;
    for (size_t i = 0; i < kMaxVenues; ++i) {
        ASSERT_TRUE(registry.Register("VENUE_" + std::to_string(i)).has_value());
    }

    // A configuration error, not a runtime condition. It must be reported so
    // the caller can refuse the connection loudly - silently dropping
    // a venue would leave the merge thinner with nothing in the logs.
    EXPECT_FALSE(registry.Register("ONE_TOO_MANY").has_value());
    EXPECT_EQ(registry.size(), kMaxVenues);
}

TEST(VenueRegistryTest, FullRegistryStillResolvesKnownVenues) {
    VenueRegistry registry;
    for (size_t i = 0; i < kMaxVenues; ++i) {
        ASSERT_TRUE(registry.Register("VENUE_" + std::to_string(i)).has_value());
    }
    ASSERT_FALSE(registry.Register("ONE_TOO_MANY").has_value());

    // Being full must not break lookups. A rejected registration is the one
    // path where a half-written slot would be easiest to leave behind.
    const auto first = registry.Find("VENUE_0");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(VenueSlotIndex(*first), 0u);
    EXPECT_EQ(registry.Name(*first), "VENUE_0");
    EXPECT_FALSE(registry.Find("ONE_TOO_MANY").has_value());
}

// --- publication ------------------------------------------------------------

TEST(VenueRegistryTest, ReaderNeverObservesAHalfPublishedSlot) {
    VenueRegistry registry;
    std::atomic<bool> start{false};
    std::atomic<bool> torn{false};

    // The documented contract is single-writer / many-reader: one thread
    // registers while the consolidator reads. The invariant
    // under test is the release/acquire pair in Register/size(): every slot a
    // reader can SEE via size() must already have its name written.
    //
    // this test cannot prove the ordering is correct - a race that never
    // fires is indistinguishable from one that cannot. It can only catch the
    // absence of the ordering, and only sometimes. It is here because a
    // relaxed store would fail it often enough to be worth the few
    // milliseconds, not because passing it is a proof.
    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int pass = 0; pass < 10000; ++pass) {
            const size_t count = registry.size();
            for (size_t i = 0; i < count; ++i) {
                if (registry.Name(static_cast<VenueSlot>(i)).empty()) {
                    torn.store(true, std::memory_order_relaxed);
                }
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (size_t i = 0; i < kMaxVenues; ++i) {
        ASSERT_TRUE(registry.Register("VENUE_" + std::to_string(i)).has_value());
    }
    reader.join();

    EXPECT_FALSE(torn.load(std::memory_order_relaxed));
    EXPECT_EQ(registry.size(), kMaxVenues);
}
