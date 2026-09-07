#include <gtest/gtest.h>

#include <grpcpp/create_channel.h>
#include <grpcpp/server_builder.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "aggregator/aggregator_service.h"

using namespace market_data;

namespace {

// A REAL gRPC server over an in-process channel, not a hand-built fake.
//
// Subscribe takes a ServerContext and a ServerWriter, neither of which
// can be constructed by a test - so the only way to exercise the actual
// subscribe path (argument validation included) is to stand up a server and
// talk to it. In-process means no sockets and no ports: the call still goes
// through the generated stubs and the full service machinery, but the
// transport is a direct hand-off.
class RoutingTest : public ::testing::Test {
   protected:
    void SetUp() override {
        grpc::ServerBuilder builder;
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        ASSERT_NE(server_, nullptr);
        stub_ = wire::Aggregator::NewStub(server_->InProcessChannel(grpc::ChannelArguments()));
    }

    void TearDown() override {
        server_->Shutdown();
        server_->Wait();
    }

    // A BBO with a recognisable price, so a test can tell which book an
    // update came from without inspecting the header alone.
    static consolidated::BBO MakeBbo(uint64_t bid_price) {
        consolidated::BBO bbo;
        bbo.best_bid.price = bid_price;
        bbo.best_bid.total_qty = 1;
        bbo.best_ask.price = bid_price + 1;
        bbo.best_ask.total_qty = 1;
        return bbo;
    }

    static wire::SubscribeRequest BboRequest(wire::MarketType market) {
        wire::SubscribeRequest request;
        request.set_symbol("BTCUSDT");
        request.set_bbo(true);
        request.set_market(market);
        return request;
    }

    AggregatorServiceImpl service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<wire::Aggregator::Stub> stub_;
};

constexpr uint64_t kSpotPrice = 100'000;
constexpr uint64_t kFuturesPrice = 900'000;

}  // namespace

// The server half of the "UNSPECIFIED is an error, not a default" decision.
// The client checks this too (ClientConfig::HasMarket), but a client is not
// the only thing that can call this RPC.
TEST_F(RoutingTest, SubscribeWithoutAMarketIsRejected) {
    grpc::ClientContext context;
    wire::SubscribeRequest request;
    request.set_symbol("BTCUSDT");
    request.set_bbo(true);
    // market deliberately left unset - which on the wire IS
    // MARKET_UNSPECIFIED, indistinguishable from setting it to zero.

    auto reader = stub_->Subscribe(&context, request);
    wire::Update update;
    EXPECT_FALSE(reader->Read(&update));

    const grpc::Status status = reader->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(RoutingTest, SubscribeWithAnUnknownSymbolIsRejected) {
    grpc::ClientContext context;
    wire::SubscribeRequest request;
    request.set_symbol("NOTACOIN");
    request.set_bbo(true);
    request.set_market(wire::SPOT);

    auto reader = stub_->Subscribe(&context, request);
    wire::Update update;
    EXPECT_FALSE(reader->Read(&update));
    EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

// The core of this file: a SPOT subscriber must never see a FUTURES update.
//
// each round publishes SPOT first and FUTURES second, and that order is
// the whole trick. The session channel CONFLATES - it keeps only the latest
// update - so publishing futures first would let a BROKEN filter still leave
// the spot update as the newest value, and the test would pass while the bug
// was live. Publishing futures LAST means a broken filter overwrites spot with
// futures, and the single read below sees the wrong market. Conflation becomes
// the detector instead of the blind spot.
//
// The retry loop exists because Subscribe registers its session asynchronously,
// somewhere after the RPC starts. Publishing once could race ahead of
// registration and reach nobody. Republishing until the reader has a message
// removes the race without a sleep-and-hope.
TEST_F(RoutingTest, SpotSubscriberNeverReceivesFuturesUpdates) {
    grpc::ClientContext context;

    std::atomic<bool> got_update{false};
    wire::Update received;

    std::thread reader_thread([&] {
        auto reader = stub_->Subscribe(&context, BboRequest(wire::SPOT));
        wire::Update update;
        if (reader->Read(&update)) {
            received = update;
            got_update = true;
        }
    });

    const InstrumentKey spot_key = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    const InstrumentKey futures_key = MakeKey(InstrumentId::BTCUSDT, MarketType::kFutures);

    for (int i = 0; i < 400 && !got_update; ++i) {
        service_.PublishBbo(spot_key, MakeBbo(kSpotPrice));
        service_.PublishBbo(futures_key, MakeBbo(kFuturesPrice));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    context.TryCancel();
    reader_thread.join();

    ASSERT_TRUE(got_update) << "subscriber received nothing at all";
    EXPECT_EQ(received.market(), wire::SPOT);
    EXPECT_EQ(received.symbol(), "BTCUSDT");
    // The price is the independent check: it identifies the SOURCE book, not
    // just what the header claims about it.
    EXPECT_EQ(received.bbo().best_bid().price(), kSpotPrice);
}

// The mirror image, so neither market is special-cased by accident.
TEST_F(RoutingTest, FuturesSubscriberNeverReceivesSpotUpdates) {
    grpc::ClientContext context;

    std::atomic<bool> got_update{false};
    wire::Update received;

    std::thread reader_thread([&] {
        auto reader = stub_->Subscribe(&context, BboRequest(wire::FUTURES));
        wire::Update update;
        if (reader->Read(&update)) {
            received = update;
            got_update = true;
        }
    });

    const InstrumentKey spot_key = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    const InstrumentKey futures_key = MakeKey(InstrumentId::BTCUSDT, MarketType::kFutures);

    // Futures first, spot last - the opposite order, for the same reason.
    for (int i = 0; i < 400 && !got_update; ++i) {
        service_.PublishBbo(futures_key, MakeBbo(kFuturesPrice));
        service_.PublishBbo(spot_key, MakeBbo(kSpotPrice));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    context.TryCancel();
    reader_thread.join();

    ASSERT_TRUE(got_update) << "subscriber received nothing at all";
    EXPECT_EQ(received.market(), wire::FUTURES);
    EXPECT_EQ(received.bbo().best_bid().price(), kFuturesPrice);
}

// Routing is on the whole InstrumentKey, not the market alone. Before the key
// existed the fanout filtered by nothing, so a BTCUSDT subscriber also received
// ETHUSDT - the same defect as the market mixing, one field over.
TEST_F(RoutingTest, SubscriberNeverReceivesADifferentSymbol) {
    grpc::ClientContext context;

    std::atomic<bool> got_update{false};
    wire::Update received;

    std::thread reader_thread([&] {
        auto reader = stub_->Subscribe(&context, BboRequest(wire::SPOT));
        wire::Update update;
        if (reader->Read(&update)) {
            received = update;
            got_update = true;
        }
    });

    const InstrumentKey btc_spot = MakeKey(InstrumentId::BTCUSDT, MarketType::kSpot);
    const InstrumentKey eth_spot = MakeKey(InstrumentId::ETHUSDT, MarketType::kSpot);

    for (int i = 0; i < 400 && !got_update; ++i) {
        service_.PublishBbo(btc_spot, MakeBbo(kSpotPrice));
        service_.PublishBbo(eth_spot, MakeBbo(kFuturesPrice));  // must never arrive
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    context.TryCancel();
    reader_thread.join();

    ASSERT_TRUE(got_update) << "subscriber received nothing at all";
    EXPECT_EQ(received.symbol(), "BTCUSDT");
    EXPECT_EQ(received.bbo().best_bid().price(), kSpotPrice);
}
