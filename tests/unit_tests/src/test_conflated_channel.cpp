#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "aggregator/conflated_channel.h"

using namespace market_data;

namespace {

// Long enough that a slow machine cannot make a "the value arrives" test time
// out by accident. Tests that need the timeout to actually fire use the short
// one instead.
constexpr auto kGenerousTimeout = std::chrono::milliseconds(2000);
constexpr auto kShortTimeout = std::chrono::milliseconds(30);

}  // namespace

TEST(ConflatedChannelTest, PushThenWaitAndTakeReturnsValue) {
    ConflatedChannel<int> channel;
    channel.Push(5);

    auto value = channel.WaitAndTake(kGenerousTimeout);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 5);
}

TEST(ConflatedChannelTest, WaitAndTakeBlocksUntilPush) {
    ConflatedChannel<int> channel;
    std::optional<int> received;

    const auto start = std::chrono::steady_clock::now();
    std::thread consumer([&] { received = channel.WaitAndTake(kGenerousTimeout); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the consumer start waiting
    channel.Push(42);
    consumer.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(*received, 42);
    // Push() must WAKE the consumer, not let it sit until the timeout. Without
    // this bound a lost notify_one() would still pass: the value is in
    // pending_ either way, so the timed wait would find it on expiry and
    // return 42 two seconds late.
    EXPECT_LT(elapsed, kGenerousTimeout / 2);
}

TEST(ConflatedChannelTest, OverwriteSemanticsOnlyLatestValueSurvives) {
    ConflatedChannel<int> channel;
    channel.Push(1);
    channel.Push(2);  // overwrites 1 - it was never read

    auto value = channel.WaitAndTake(kGenerousTimeout);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 2);
}

TEST(ConflatedChannelTest, CloseUnblocksWaitingConsumerWithNullopt) {
    ConflatedChannel<int> channel;
    std::optional<int> received = 999;  // sentinel, should be overwritten to nullopt

    const auto start = std::chrono::steady_clock::now();
    std::thread consumer([&] { received = channel.WaitAndTake(kGenerousTimeout); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    channel.Close();
    consumer.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(received.has_value());
    EXPECT_TRUE(channel.IsClosed());
    // Close() must WAKE the consumer, not let it sit until the timeout. The
    // nullopt check alone would pass two seconds later even if Close() did
    // nothing but set the flag.
    EXPECT_LT(elapsed, kGenerousTimeout / 2);
}

// The distinction the aggregator's subscribe loop depends on: a timeout and a
// Close() both return nullopt, and only IsClosed() separates them. If this
// ever fails, that loop drops live sessions after one quiet poll interval.
TEST(ConflatedChannelTest, TimeoutReturnsNulloptWithoutClosing) {
    ConflatedChannel<int> channel;

    auto value = channel.WaitAndTake(kShortTimeout);

    EXPECT_FALSE(value.has_value());
    EXPECT_FALSE(channel.IsClosed());
}

TEST(ConflatedChannelTest, ClosePreservesAlreadyPendingValue) {
    ConflatedChannel<int> channel;
    channel.Push(7);
    channel.Close();  // a value was already waiting - Close() doesn't discard it

    auto value = channel.WaitAndTake(kGenerousTimeout);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 7);
}
