#include "binance_provider.h"
#include "binance_parser.h"
#include "md_provider/rest.h"
#include "continuity.h"
#include "logger/logger.h"
#include "types/venue.h"
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <optional>
#include <thread>
#include <utility>

using namespace market_data;

namespace {

std::string ToLowerSymbol(InstrumentKey instrument) {
    std::string symbol = VenueConverter::ToInstrumentString(instrument.Symbol());
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return symbol;
}

std::string UpperSymbol(InstrumentKey instrument) {
    return VenueConverter::ToInstrumentString(instrument.Symbol());
}

// Recovers the bare stream name from a resolved single-stream URL
// ("/ws/btcusdt@depth@100ms" -> "btcusdt@depth@100ms"), so the combined
// endpoint's stream list and its "stream" envelope field can be built from
// the same venues_config.json templates depth_path/bbo_path already are,
// rather than a second, separately-maintained copy of Binance's naming.
std::string_view StripWsPrefix(std::string_view path) {
    constexpr std::string_view kPrefix = "/ws/";
    return path.substr(path.rfind(kPrefix, 0) == 0 ? kPrefix.size() : 0);
}

}  // namespace

BinanceProvider::BinanceProvider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback)
    : Provider(config, std::move(callback), std::move(quote_callback)), parser_(config.depth) {
    // Binance streams are named with a LOWERCASE symbol ("btcusdt@depth@100ms").
    // The path shape itself now comes from venues_config.json - only the
    // spelling of the symbol is Binance's business.
    ResolveStreamPaths(ToLowerSymbol(config.instrument));

    const std::string_view depth_stream = StripWsPrefix(GetDepthPath());
    bbo_stream_name_ = std::string(StripWsPrefix(GetBboPath()));
    combined_path_ = fmt::format("/stream?streams={}/{}", depth_stream, bbo_stream_name_);
}

bool BinanceProvider::OnReconnect() {
    // Per-connection state only. Binance never re-sends a snapshot on the
    // stream, so without this a resync would leave us in kLive with a stale
    // last_depth_u_ and resync forever.
    sync_state_ = SyncState::kSyncing;
    snapshot_requested_ = false;
    snapshot_attempts_ = 0;
    pending_.clear();
    last_depth_u_ = 0;
    // Nothing here can fail: the REST snapshot is fetched later, off the
    // io_context thread, and its failures are handled by the resync path.
    return true;
}

void BinanceProvider::FetchSnapshotAsync() {
    snapshot_requested_ = true;
    ++snapshot_attempts_;

    // Depth is a REST query parameter on Binance - already resolved to a valid
    // limit by SelectDepthTier. An arbitrary value here would be rejected.
    //
    // the valid limits differ by MARKET. Spot accepts up to 5000; futures
    // (/fapi/v1/depth) caps at 1000 and returns {"code":-1130} above it -
    // measured, not assumed. The path comes from config so spot and futures
    // reach different endpoints, but the TIER TABLE is still shared - see the
    // note in config/config.h.
    std::string target =
        fmt::format("{}?symbol={}&limit={}", config.rest_depth_path, UpperSymbol(config.instrument), config.depth);
    std::string host = config.rest_host;
    std::string port = config.rest_port;
    VenueId venue = config.venue_id;
    InstrumentKey instrument = config.instrument;

    // Detached thread: HttpsGet blocks, and doing that on the io_context
    // thread would stall the read loop that is currently buffering events.
    std::thread([this, host, port, target, venue, instrument]() {
        auto body = HttpsGet(host, port, target);
        // Own parser: this lambda runs on a detached thread, so it must not
        // touch the io_context thread's parser_. A fresh one per snapshot is
        // fine - a snapshot happens about once per (re)sync.
        std::optional<BookUpdate> snapshot;
        if (body) {
            BinanceParser snapshot_parser(config.depth);  // config is immutable after construction
            snapshot = snapshot_parser.ParseDepthSnapshot(*body, venue, instrument);
        }

        // Back onto the io_context thread - everything below touches state
        // shared with the message handlers.
        PostToIoContext([this, snapshot = std::move(snapshot)]() mutable {
            if (!snapshot) {
                Logger::Log(LogLevel::kError, "[{}] depth snapshot fetch failed", venue_market_str_);
                if (snapshot_attempts_ >= kMaxSnapshotAttempts) {
                    RequestResync();
                    return;
                }
                FetchSnapshotAsync();
                return;
            }

            if (!ReconcileSnapshot(std::move(*snapshot))) {
                // Snapshot predates our buffered events - it cannot be
                // joined onto them. Refetch a newer one.
                if (snapshot_attempts_ >= kMaxSnapshotAttempts) {
                    Logger::Log(LogLevel::kError, "[{}] snapshot never caught up after {} attempts - resyncing",
                                venue_market_str_, snapshot_attempts_);
                    RequestResync();
                    return;
                }
                Logger::Log(LogLevel::kWarning, "[{}] snapshot too old to join buffered events - refetching",
                            venue_market_str_);
                FetchSnapshotAsync();
            }
        });
    }).detach();
}

bool BinanceProvider::ReconcileSnapshot(BookUpdate snapshot) {
    const uint64_t last_update_id = snapshot.seq;

    // Which buffered event (if any) joins onto this snapshot. nullopt means
    // the snapshot predates the buffer and cannot be joined - refetch.
    auto first = ReconcileBinanceSnapshot(last_update_id, pending_);
    if (!first) {
        return false;
    }

    // The `u` of the last event we actually SAW on the wire - captured before
    // the replay loop below, which moves out of pending_ and would leave
    // pending_.back() moved-from (same hazard the loop's own comment refuses
    // to depend on).
    const std::optional<uint64_t> last_buffered_seq =
        pending_.empty() ? std::nullopt : std::optional<uint64_t>(pending_.back().seq);

    snapshot.recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;
    Emit(std::move(snapshot));
    last_depth_u_ = last_update_id;

    for (size_t i = *first; i < pending_.size(); ++i) {
        // seq read BEFORE Emit: Emit consumes the update, and reading a
        // moved-from object afterwards happens to work only because seq is a
        // scalar the implicit move leaves alone. Not worth depending on.
        last_depth_u_ = pending_[i].seq;
        Emit(std::move(pending_[i]));
    }

    // futures continuity chains on `pu` and demands an EXACT match, so
    // last_depth_u_ must be the `u` of a REAL WS EVENT - never lastUpdateId.
    //
    // lastUpdateId is a single update id, while WS events cover RANGES [U, u],
    // so it need not land on any event boundary; and the REST snapshot is
    // usually AHEAD of the 100ms-throttled stream. When every buffered event
    // is already inside the snapshot the loop above never runs, leaving
    // last_depth_u_ at lastUpdateId - and the next live event's pu, which
    // chains from the last real WS event, then compares BELOW it and reports a
    // gap that never happened. Measured before this fix: a false resync every
    // ~130s, with pu ~1800 below the expected value.
    //
    // Spot is unaffected: its rule is a coverage-range test (U <= last_u + 1)
    // that was built to absorb exactly this REST/WS misalignment, which is why
    // this correction is futures-only.
    //
    // When the loop DID run this is a no-op - it assigns the same value the
    // loop's last iteration already did.
    if (config.instrument.Market() == MarketType::kFutures && last_buffered_seq.has_value()) {
        last_depth_u_ = *last_buffered_seq;
    }
    pending_.clear();

    sync_state_ = SyncState::kLive;
    Logger::Log(LogLevel::kInfo, "[{}] depth synced at lastUpdateId={}, now live", venue_market_str_, last_update_id);
    return true;
}

void BinanceProvider::OnDepthMessage(const std::string& message, uint32_t conn_index) {
    auto update = parser_.ParseDepthMessage(message, config.venue_id, config.instrument);
    if (!update) {
        return;  // not a depth update (e.g. a control/ack message)
    }
    update->recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;

    // Binance is the venue with NO keepalive. Its diff stream sends
    // nothing when nothing changes, so unlike Bybit L1 (3s republish) and OKX
    // books (~60s seqId == prevSeqId), silence here carries no information at
    // all - it is a quiet market and a dead feed at once. This stamp
    // therefore records real data only, and Binance's health has to come from
    // connection state plus comparison against the other venues. That is
    // precisely why cross-venue corroboration is not optional.
    //
    // Still stamped before the dedup, for consistency with the other two: a
    // duplicate from a redundant connection is bytes on the wire and proves
    // the feed is delivering, even though it is dropped as a book update.
    NoteDepthActivity();

    // Redundant-connection dedup, BEFORE the sync branch below - not after.
    //
    // during kSyncing every event is buffered into pending_, so a
    // duplicate reaching that branch would be buffered too, and
    // ReconcileSnapshot would emit each event N times when it drains. Worse,
    // pending_ is capped at kMaxPendingEvents: with three connections it
    // would fill three times faster and trip the "buffered too long,
    // resyncing" path during the exact window where the REST snapshot is
    // still in flight.
    //
    // venue_reset is always false here. Binance never sends a snapshot on the
    // stream - the book is seeded from REST - so the id only climbs and the
    // high-water mark never has to move backwards.
    if (!AcceptDepth(update->seq, conn_index, /*venue_reset=*/false)) {
        return;
    }

    if (sync_state_ == SyncState::kSyncing) {
        // Subscribe-and-buffer FIRST, snapshot second. The fetch is
        // kicked off from here - the first event proves the stream is
        // actually delivering, and Provider has no "connected" hook.
        if (!snapshot_requested_) {
            FetchSnapshotAsync();
        }
        if (pending_.size() >= kMaxPendingEvents) {
            Logger::Log(LogLevel::kError, "[{}] buffered {} events without a snapshot - resyncing", venue_market_str_,
                        pending_.size());
            RequestResync();
            return;
        }
        pending_.push_back(std::move(*update));
        return;
    }

    // Live: each event must continue the chain. Binance never sends a
    // snapshot on the stream, so kReset/kIgnore cannot occur here - only
    // kApply or kGap. Which RULE decides that differs by market - see
    // CheckBinanceFuturesContinuity for why spot's U-based rule is wrong for
    // futures (measured: 14 false-positive resyncs in 14 seconds).
    if (config.instrument.Market() == MarketType::kFutures) {
        const std::optional<uint64_t> chain_seq = parser_.LastChainSeq();
        // No pu on a message that should have one is not "assume it's fine" -
        // continuity cannot be verified without it, so this takes the same
        // fail-safe direction as everything else here: resync rather than
        // apply an update we cannot actually vouch for.
        const ContinuityAction action = chain_seq.has_value()
                                            ? CheckBinanceFuturesContinuity(*chain_seq, update->seq, last_depth_u_)
                                            : ContinuityAction::kGap;
        if (action == ContinuityAction::kGap) {
            Logger::Log(LogLevel::kWarning, "[{}] futures depth gap: expected pu={}, got {} - resyncing",
                        venue_market_str_, last_depth_u_,
                        chain_seq.has_value() ? static_cast<int64_t>(*chain_seq) : -1);
            RequestResync();
            return;
        }
    } else {
        if (CheckBinanceSpotContinuity(*update, last_depth_u_) == ContinuityAction::kGap) {
            Logger::Log(LogLevel::kWarning, "[{}] depth gap: expected U={}, got {} - resyncing", venue_market_str_,
                        last_depth_u_ + 1, update->prev_seq);
            RequestResync();
            return;
        }
    }
    Emit(std::move(*update));
}

void BinanceProvider::OnBboMessage(const std::string& message, uint32_t conn_index) {
    auto quote = parser_.ParseBboMessage(message, config.venue_id, config.instrument);
    if (!quote) {
        return;  // not a bookTicker payload (e.g. a subscribe ack)
    }

    // Same as the depth stream: @bookTicker has no keepalive either, so this
    // records real data only.
    NoteBboActivity();

    // Separate filter from the depth stream: @bookTicker and @depth carry
    // independent `u` sequences, so a shared high-water mark would silently
    // drop one stream behind the other.
    if (!AcceptBbo(quote->seq, conn_index)) {
        return;
    }

    quote->recv_ts_ns = GetCurrentTimeMs() * kTsNsMultiplier;
    EmitQuote(*quote);
}

void BinanceProvider::OnCombinedMessage(const std::string& message, uint32_t conn_index) {
    // Unwraps the {"stream":...,"data":...} envelope and decides which
    // stream this message belongs to, BEFORE the real parse below.
    auto unwrapped = parser_.UnwrapCombinedMessage(message);
    if (!unwrapped) {
        return;  // malformed frame - Binance's combined endpoint sends no
                 // subscribe acks, so this should not happen in practice
    }

    // OnDepthMessage/OnBboMessage take `const std::string&`, matching the
    // signature every message handler in this hierarchy shares (it is also
    // what the raw WebSocket session callback delivers). unwrapped->data is
    // a view into parser_'s reusable buffer, not a std::string, so handing
    // it to either call means constructing one here - one allocation per
    // message that a split connection would not pay. Accepted rather than
    // widening that shared signature to std::string_view across every venue
    // for this one case; not measured, revisit if it shows up.
    if (unwrapped->stream == bbo_stream_name_) {
        OnBboMessage(std::string(unwrapped->data), conn_index);
    } else {
        OnDepthMessage(std::string(unwrapped->data), conn_index);
    }
}
