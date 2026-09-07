#pragma once

// BinanceProvider: the one venue whose depth stream is differential-only and
// needs REST. Owns the buffer-then-reconcile sync state machine
// (kSyncing/kLive) - subscribe first, fetch the REST snapshot second, then
// join buffered events onto it via ReconcileSnapshot - because the reverse
// order loses every update in between.

#include "md_provider/md_provider.h"
#include "binance_parser.h"
#include <string>
#include <vector>

namespace market_data {

class BinanceProvider : public Provider {
   public:
    explicit BinanceProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback = nullptr);
    ~BinanceProvider() override = default;

   protected:
    void OnDepthMessage(const std::string& message, uint32_t conn_index) override;
    void OnBboMessage(const std::string& message, uint32_t conn_index) override;
    bool OnReconnect() override;

    // Binance's two per-stream URLs (/ws/btcusdt@depth@100ms,
    // /ws/btcusdt@bookTicker) are not one endpoint, unlike Bybit/OKX, so
    // GetCombinedPath()'s default (reuse GetDepthPath()) is wrong here - the
    // combined form is a THIRD path, /stream?streams=A/B, built from both.
    const char* GetCombinedPath() const override { return combined_path_.c_str(); }

    // Stream selection happens through the URL above, not a subscribe frame
    // - Binance needs none, on the combined endpoint same as on the two
    // separate ones it replaces.
    std::string CombinedSubscriptionMessage() const override { return ""; }

    // Routes on the `stream` envelope field before the real parse, then
    // calls the unchanged OnDepthMessage()/OnBboMessage() above with the
    // unwrapped `data` - see the .cpp for why that costs one extra
    // std::string per message here, unlike Bybit/OKX's peek-then-reparse.
    void OnCombinedMessage(const std::string& message, uint32_t conn_index) override;

   private:
    // Binance's depth stream is DIFFERENTIAL only - it never sends a
    // snapshot. The book has to be seeded from REST, and the ordering
    // matters: subscribe and buffer FIRST, fetch the
    // snapshot SECOND, then reconcile. Fetching first loses every update
    // that arrives during the round-trip.
    enum class SyncState {
        kSyncing,  // buffering WS events, waiting for the REST snapshot
        kLive,     // book seeded, applying deltas with continuity checks
    };

    // Called on the io_context thread once the stream is confirmed live.
    // Spawns a detached thread for the blocking HTTPS GET, because doing it
    // here would stall the very read loop that is buffering events, then
    // posts the result back onto the io_context thread.
    void FetchSnapshotAsync();

    // Runs on the io_context thread. Drops buffered events older than the
    // snapshot, checks that one of the survivors actually joins onto it,
    // and emits snapshot + survivors. Returns false if the snapshot is too
    // old to join (caller refetches).
    bool ReconcileSnapshot(BookUpdate snapshot);

    // Guards pending_ from growing without bound if the fetch hangs or the
    // snapshot keeps failing to join. Well above the ~10 events/sec this
    // stream produces, so hitting it means something is actually wrong.
    static constexpr size_t kMaxPendingEvents = 2000;
    // A snapshot older than the buffered events can't be joined; refetch.
    // Capped so a persistently stale endpoint can't loop forever.
    static constexpr int kMaxSnapshotAttempts = 5;

    // All of these are only touched on the io_context thread (the snapshot
    // result is marshalled back there via net::post), so no locking.
    //
    // parser_ included: OnDepthMessage and OnBboMessage both run on that one
    // thread, so they share it. The REST snapshot runs on a detached thread
    // and uses its own local BinanceParser instead.
    BinanceParser parser_;
    SyncState sync_state_ = SyncState::kSyncing;
    bool snapshot_requested_ = false;
    int snapshot_attempts_ = 0;
    std::vector<BookUpdate> pending_;
    uint64_t last_depth_u_ = 0;

    // Built once in the constructor from config.depth_path/bbo_path (already
    // resolved to /ws/<streamName> by ResolveStreamPaths), by stripping the
    // /ws/ prefix each carries as a single-stream URL and joining the two
    // names with "/" the way Binance's combined-stream endpoint expects:
    // /stream?streams=<depth stream>/<bbo stream>.
    std::string combined_path_;

    // The bare stream name Binance's combined endpoint echoes back in each
    // message's "stream" field (e.g. "btcusdt@bookTicker") - compared
    // against in OnCombinedMessage to route. Built alongside combined_path_
    // from the same two names.
    std::string bbo_stream_name_;
};

}  // namespace market_data
