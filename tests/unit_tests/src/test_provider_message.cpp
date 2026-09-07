#include <gtest/gtest.h>

#include "md_core/provider_message.h"

#include <chrono>
#include <thread>
#include <variant>

using namespace market_data;

namespace {

// Variant alternative indices, named so the assertions read as intent rather
// than as magic numbers. Order must match the declaration in
// provider_message.h.
constexpr size_t kUpdateIndex = 0;
constexpr size_t kQuoteIndex = 1;
constexpr size_t kHealthIndex = 2;

ProviderMessage MakeUpdate(uint64_t seq) {
    BookUpdate update;
    update.venue = VenueId::BYBIT;
    update.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    update.seq = seq;
    return ProviderMessage{std::move(update)};
}

ProviderMessage MakeQuote(uint64_t seq) {
    BboQuote quote{};
    quote.venue = VenueId::BYBIT;
    quote.instrument = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    quote.seq = seq;
    return ProviderMessage{quote};
}

ProviderMessage MakeHealth(VenueHealth health) {
    return ProviderMessage{VenueHealthEvent{
        .venue = VenueId::BYBIT, .stream = StreamKind::kDepth, .health = health, .decided_mono_ns = 1}};
}

// A hang here means a real ordering/visibility bug, not a slow machine.
constexpr auto kTimeout = std::chrono::seconds(10);

}  // namespace

// --- the property that justifies one queue per venue ------------------------

TEST(ProviderMessageTest, MixedKindsPopInPushOrder) {
    ProviderQueue queue;

    ASSERT_TRUE(queue.TryPush(MakeUpdate(1)));
    ASSERT_TRUE(queue.TryPush(MakeQuote(2)));
    ASSERT_TRUE(queue.TryPush(MakeHealth(VenueHealth::kStale)));
    ASSERT_TRUE(queue.TryPush(MakeUpdate(3)));

    ProviderMessage out;
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(out.index(), kUpdateIndex);
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(out.index(), kQuoteIndex);
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(out.index(), kHealthIndex);
    ASSERT_TRUE(queue.TryPop(out));
    EXPECT_EQ(out.index(), kUpdateIndex);

    EXPECT_TRUE(queue.Empty());
}

// The scenario spelled out in Core::OnVenueHealth's comment, as an
// executable test: a venue's health verdict must land between the updates it
// was produced between, so acting on it cannot apply a pre-gap update as if
// the venue were still healthy.
//
// Split into three queues by message kind, this test is what would fail -
// the health event could be drained before update 47 or after update 48
// depending on which queue the consolidator happened to check first.
TEST(ProviderMessageTest, HealthEventStaysOrderedAgainstItsVenuesUpdates) {
    ProviderQueue queue;

    ASSERT_TRUE(queue.TryPush(MakeUpdate(47)));
    ASSERT_TRUE(queue.TryPush(MakeHealth(VenueHealth::kStale)));
    ASSERT_TRUE(queue.TryPush(MakeUpdate(48)));

    ProviderMessage out;

    ASSERT_TRUE(queue.TryPop(out));
    ASSERT_EQ(out.index(), kUpdateIndex);
    EXPECT_EQ(std::get<BookUpdate>(out).seq, 47u);

    ASSERT_TRUE(queue.TryPop(out));
    ASSERT_EQ(out.index(), kHealthIndex) << "health event must not overtake or lag its venue's updates";
    EXPECT_EQ(std::get<VenueHealthEvent>(out).health, VenueHealth::kStale);

    ASSERT_TRUE(queue.TryPop(out));
    ASSERT_EQ(out.index(), kUpdateIndex);
    EXPECT_EQ(std::get<BookUpdate>(out).seq, 48u);
}

// --- the same property, under real concurrency ------------------------------

TEST(ProviderMessageTest, ConcurrentMixedKindSequencePreservesOrder) {
    // Deliberately larger than kProviderQueueCapacity so the producer really
    // blocks and the ring really wraps, rather than the whole run fitting in
    // an empty queue.
    constexpr int kRounds = 5'000;
    ProviderQueue queue;

    // A fixed repeating pattern the consumer can predict exactly:
    // update, update, quote, health.
    const auto expected_index = [](int i) -> size_t {
        switch (i % 4) {
            case 0:
            case 1:
                return kUpdateIndex;
            case 2:
                return kQuoteIndex;
            default:
                return kHealthIndex;
        }
    };

    std::thread producer([&queue, &expected_index] {
        for (int i = 0; i < kRounds; ++i) {
            ProviderMessage message = expected_index(i) == kUpdateIndex  ? MakeUpdate(static_cast<uint64_t>(i))
                                      : expected_index(i) == kQuoteIndex ? MakeQuote(static_cast<uint64_t>(i))
                                                                         : MakeHealth(VenueHealth::kLive);
            // "Block, never drop" - the policy the queue deliberately does
            // not implement itself (see spsc_queue.h).
            while (!queue.TryPush(std::move(message))) {
                std::this_thread::yield();
            }
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    for (int i = 0; i < kRounds; ++i) {
        ProviderMessage out;
        while (!queue.TryPop(out)) {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "consumer stalled at message " << i;
            std::this_thread::yield();
        }
        ASSERT_EQ(out.index(), expected_index(i)) << "kind order broken at message " << i;

        // Payload must survive the move through the ring, not just the tag.
        if (out.index() == kUpdateIndex) {
            EXPECT_EQ(std::get<BookUpdate>(out).seq, static_cast<uint64_t>(i));
        } else if (out.index() == kQuoteIndex) {
            EXPECT_EQ(std::get<BboQuote>(out).seq, static_cast<uint64_t>(i));
        }
    }

    producer.join();
    EXPECT_TRUE(queue.Empty());
}
