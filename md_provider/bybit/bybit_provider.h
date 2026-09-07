#pragma once

// BybitProvider: subscribes to orderbook.50 (depth) and orderbook.1
// (fast-BBO) over Bybit's generic public endpoint, chaining depth by
// `u == last_u + 1` with an in-channel snapshot - no REST call needed.

#include "md_provider/md_provider.h"
#include "bybit_parser.h"
#include "types/venue.h"
#include <string>

namespace market_data {

class BybitProvider : public Provider {
   public:
    explicit BybitProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback = nullptr);
    ~BybitProvider() override = default;

   protected:
    void OnDepthMessage(const std::string& message, uint32_t conn_index) override;
    void OnBboMessage(const std::string& message, uint32_t conn_index) override;

    // Depth (orderbook.{depth}) and BBO (orderbook.1) live on the same
    // endpoint, so one connection carries both subscriptions in one
    // {"op":"subscribe",...} frame (§15.5).
    std::string CombinedSubscriptionMessage() const override;

    // Routes on `topic` before the real parse, then calls the unchanged
    // OnDepthMessage()/OnBboMessage() above - see the .cpp for why a raw
    // string is not enough here.
    void OnCombinedMessage(const std::string& message, uint32_t conn_index) override;

   private:
    // Last applied `u` on the depth stream (orderbook.50). Bybit increments
    // it by exactly 1 per delta, so any other step is a gap. 0 means "no
    // snapshot yet" - only touched on the io_context thread, so no lock.
    uint64_t last_depth_u_{};

    // Both OnDepthMessage and OnBboMessage run on the one io_context thread
    // and both use this parser (the orderbook.* shape is the same for the
    // depth and fast-BBO topics). No detached REST-snapshot path on Bybit.
    BybitParser parser_;

    // Precomputed once, compared against PeekTopic()'s result in
    // OnCombinedMessage - avoids building these strings on every message.
    //
    // KNOWN COLLISION: Bybit's shallowest published tier is 1 (kBybitDepthTiers
    // in config/config.h), the same number as the fast-BBO topic. If the
    // resolved depth tier is 1, depth_topic_ == bbo_topic_ and OnCombinedMessage
    // cannot tell the streams apart by topic alone - it favours BBO. This is a
    // pre-existing oddity of subscribing to depth at tier 1 (a one-level depth
    // stream is nearly a second BBO feed), sharpened by combining the sockets;
    // not resolved here.
    std::string depth_topic_;
    std::string bbo_topic_;
};

}  // namespace market_data
