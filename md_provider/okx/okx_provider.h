#pragma once

// OKXProvider: subscribes to "books" (depth) and "bbo-tbt" (fast-BBO) over
// OKX's generic public endpoint. For futures, resolves the swap's contract
// size (ctVal) via REST in OnReconnect - before any socket opens - and hands
// it to the parser so no message can be parsed at the wrong scale.

#include "md_provider/md_provider.h"
#include "okx_parser.h"
#include "types/venue.h"
#include <string>

namespace market_data {

class OKXProvider : public Provider {
   public:
    explicit OKXProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback = nullptr);
    ~OKXProvider() override = default;

   protected:
    void OnDepthMessage(const std::string& message, uint32_t conn_index) override;
    void OnBboMessage(const std::string& message, uint32_t conn_index) override;

    // "books" (depth) and "bbo-tbt" (BBO) live on the same endpoint, so one
    // connection carries both subscriptions in one {"op":"subscribe",...}
    // frame (§15.5).
    std::string CombinedSubscriptionMessage() const override;

    // Routes on `arg.channel` before the real parse, then calls the
    // unchanged OnDepthMessage()/OnBboMessage() above.
    void OnCombinedMessage(const std::string& message, uint32_t conn_index) override;

    // Resolves this instrument's contract size before any socket is opened.
    // See the definition for why it must happen here and not asynchronously.
    bool OnReconnect() override;

   private:
    // Last applied `seqId` on the depth stream (books). OKX chains messages
    // by prevSeqId == previous seqId - the ids are NOT contiguous, so a
    // "+1" check like Bybit's would be wrong here. 0 means "no snapshot
    // yet"; only touched on the io_context thread, so no lock.
    uint64_t last_depth_seq_ = 0;

    // Both OnDepthMessage and OnBboMessage run on the one io_context thread,
    // so they share this parser. OKX has no detached REST-snapshot path.
    OkxParser parser_;

    // Set once the contract size has been read from OKX and handed to
    // parser_. Cached across reconnects: a swap's ctVal is a contract
    // specification, not session state, so refetching it on every reconnect
    // would add a network round-trip to the recovery path for a number that
    // has not changed.
    //
    // Spot never needs it, so this stays false there and OnReconnect does
    // nothing - parser_ keeps its 1.0 default.
    bool contract_size_resolved_ = false;
};

}  // namespace market_data
