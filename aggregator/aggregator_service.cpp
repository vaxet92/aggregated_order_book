#include "aggregator_service.h"
#include "wire_translation.h"
#include "logger/logger.h"

#include <fmt/ranges.h>  // fmt::join

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace market_data {

namespace {

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// applied when a client subscribes to a band
// feed but sends an empty threshold list. Notionals are USDT x 1e8, matching
// FillToNotional's target scale.
constexpr uint64_t kMillion = 1'000'000ULL * kScaleFactor;
const std::vector<uint64_t> kDefaultNotionalBands = {
    1 * kMillion};  // 1 * kMillion, 5 * kMillion, 10 * kMillion, 25 * kMillion, 50 * kMillion
const std::vector<uint32_t> kDefaultBpsBands = {500};  // 50, 100, 200, 500, 1000

// How long a session's handler thread may stay parked before it re-checks
// whether its client is still there. NOT a latency figure: a published update
// wakes the wait immediately (ConflatedChannel::Push). This only bounds how
// long a DEAD session can linger, because the synchronous gRPC API gives no
// way to be woken on cancellation - ServerContext offers no hook a condition
// variable can wait on.
constexpr auto kCancellationPollInterval = std::chrono::milliseconds(200);

// Fills the common header every Update carries regardless of payload, so a
// client never needs state from an earlier message to interpret this one
// (a state-publishing API, not an event log).
void FillHeader(wire::Update& update, InstrumentKey instrument) {
    update.set_server_ts_ns(NowNs());
    // Symbol() only - ToInstrumentString(InstrumentKey) would emit
    // "BTCUSDT:spot", which is a log format, not a wire symbol. The market
    // travels in its own typed field below rather than glued into the symbol
    // string, so a client parses an enum instead of splitting on ':'.
    update.set_symbol(VenueConverter::ToInstrumentString(instrument.Symbol()));
    update.set_market(ToWire(instrument.Market()));
    update.set_price_scale(kScaleExponent);
    update.set_qty_scale(kScaleExponent);
}

}  // namespace

uint64_t AggregatorServiceImpl::RegisterSession(Subscription subscription) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    uint64_t id = next_session_id_++;
    sessions_[id] = std::move(subscription);
    return id;
}

void AggregatorServiceImpl::UnregisterSession(uint64_t session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session_id);
}

grpc::Status AggregatorServiceImpl::Subscribe(grpc::ServerContext* context, const wire::SubscribeRequest* request,
                                              grpc::ServerWriter<wire::Update>* writer) {
    const std::optional<InstrumentId> instrument = VenueConverter::ToInstrumentId(request->symbol());
    if (!instrument.has_value()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown symbol: " + request->symbol());
    }

    // Spot and futures are separate subscriptions, so the market is REQUIRED -
    // it is half of the key that selects the book. FromWire returns nullopt for
    // MARKET_UNSPECIFIED, which is what a client that omitted the field sends:
    // a proto3 enum has no presence, so "forgot" and "chose zero" are the same
    // bytes and cannot be told apart here. Rejecting is what makes that visible
    // rather than silently serving spot.
    const std::optional<MarketType> market = FromWire(request->market());
    if (!market.has_value()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "market must be SPOT or FUTURES");
    }

    // Feeds are selected by PRESENCE: bbo is a bool; the band feeds
    // are optional sub-messages whose presence means "subscribed" and whose
    // (possibly empty) array picks the thresholds.
    Subscription subscription;
    subscription.channel = std::make_shared<Channel>();
    subscription.instrument = MakeKey(*instrument, *market);
    subscription.wants_bbo = request->bbo();
    subscription.wants_volume_bands = request->has_volume_bands();
    subscription.wants_price_bands = request->has_price_bands();

    if (!subscription.wants_bbo && !subscription.wants_volume_bands && !subscription.wants_price_bands) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "subscription requests no feeds");
    }

    if (subscription.wants_volume_bands) {
        const auto& bands = request->volume_bands().notional_bands();
        subscription.notional_bands.assign(bands.begin(), bands.end());
        if (subscription.notional_bands.empty()) {
            subscription.notional_bands = kDefaultNotionalBands;
        }
        // FillToNotionalBands walks forward once and never rewinds, which is
        // only correct if the targets ascend. Sorted here rather than
        // trusted from the wire.
        std::sort(subscription.notional_bands.begin(), subscription.notional_bands.end());
    }
    if (subscription.wants_price_bands) {
        const auto& bands = request->price_bands().bps_bands();
        subscription.bps_bands.assign(bands.begin(), bands.end());
        if (subscription.bps_bands.empty()) {
            subscription.bps_bands = kDefaultBpsBands;
        }
        std::sort(subscription.bps_bands.begin(), subscription.bps_bands.end());
    }

    // Built BEFORE the move below - subscription's vectors are gone after it.
    // Not an if/else chain: a client may subscribe to several feeds at once,
    // and reporting only the first would misrepresent what it asked for.
    std::string feeds;
    if (subscription.wants_bbo) {
        feeds += "BBO ";
    }
    if (subscription.wants_volume_bands) {
        // Printed in whole millions rather than raw scaled integers - the
        // defaults are 1e14..5e15, which are unreadable in a log line.
        std::vector<uint64_t> millions;
        millions.reserve(subscription.notional_bands.size());
        for (uint64_t notional : subscription.notional_bands) {
            millions.push_back(notional / kScaleFactor / 1'000'000);
        }
        feeds += fmt::format("VOLUME_BANDS[{}M] ", fmt::join(millions, "M,"));
    }
    if (subscription.wants_price_bands) {
        feeds += fmt::format("PRICE_BANDS[{}bps] ", fmt::join(subscription.bps_bands, "bps,"));
    }

    auto channel = subscription.channel;  // kept alive for the read loop below
    uint64_t session_id = RegisterSession(std::move(subscription));
    Logger::Log(LogLevel::kInfo, "[Aggregator] session {} subscribed (symbol={}) {}", session_id, request->symbol(),
                feeds);

    // the timeout is what makes this loop exit at all when the client
    // disappears during a quiet market. ClientContext::TryCancel() sets a flag
    // inside gRPC; it cannot wake a thread parked on this channel's condition
    // variable, so IsCancelled() is only ever observed here, at the top.
    // Without the bounded wait, a client that disconnects with nothing pending
    // leaks this thread and its session entry for the life of the process.
    while (!context->IsCancelled()) {
        auto update = channel->WaitAndTake(kCancellationPollInterval);
        if (!update) {
            if (channel->IsClosed()) {
                break;  // session torn down deliberately
            }
            continue;  // timed out - re-check cancellation at the top
        }
        if (!writer->Write(*update)) {
            break;  // client gone
        }
    }

    UnregisterSession(session_id);
    Logger::Log(LogLevel::kInfo, "[Aggregator] session {} disconnected", session_id);
    return grpc::Status::OK;
}

void AggregatorServiceImpl::PublishBbo(InstrumentKey instrument, const consolidated::BBO& bbo) {
    wire::Update update;
    FillHeader(update, instrument);
    *update.mutable_bbo() = ToWire(bbo, venue_wire_table_);

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, subscription] : sessions_) {
        // Symbol AND market in one compare - see Subscription::instrument.
        // Without this every session received every book, spot and futures
        // alike, which is the mixing the whole InstrumentKey design prevents
        // one layer down in Core.
        if (subscription.instrument != instrument) {
            continue;  // a different book
        }
        if (!subscription.wants_bbo) {
            continue;  // subscribed to bands only
        }
        // seq is per-session and assigned under the same lock that orders
        // the pushes, so a client can never see it go backwards.
        update.set_seq(subscription.next_seq++);
        subscription.channel->Push(update);
    }
}

void AggregatorServiceImpl::PublishBook(InstrumentKey instrument, const consolidated::Book& book) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    // The publication depth cap, applied HERE rather than by the merge.
    //
    // Core's consolidated state is deliberately unbounded - capping it
    // would silently shrink the book, because after an erase the level that
    // should take the last slot lives beyond the cap and an incremental repair
    // cannot see it (consolidated_state.h). So the cap moved to the only place
    // that reads the levels: this band walk. Client output is unchanged, since
    // the bands previously saw a book the merge had already truncated to the
    // same depth.
    const auto capped = [](const std::vector<consolidated::MergedLevel>& side) {
        return std::span<const consolidated::MergedLevel>(side).first(
            std::min(side.size(), consolidated::kDefaultMaxDepth));
    };
    const std::span<const consolidated::MergedLevel> bids = capped(book.bids);
    const std::span<const consolidated::MergedLevel> asks = capped(book.asks);

    // Band math is memoized ACROSS sessions, keyed on the thresholds.
    //
    // two sessions that asked for the same thresholds receive
    // byte-identical messages apart from `seq`, so both the band walk and the
    // protobuf build are done once and shared. That is what makes
    // per-client thresholds cost O(distinct threshold sets x depth) instead of
    // O(sessions x depth). The old loop called the band functions INSIDE the
    // session loop; with the merge down to 167 ns the walk was ~90% of publish
    // cost, so paying it per session was the top remaining bottleneck.
    //
    // The cache is per publish, not persistent: the book has changed, so every
    // entry from the previous publish is stale by construction.
    //
    // Lookup is a linear scan with a vector value-compare rather than a hash
    // map. Deliberate: the number of DISTINCT threshold sets is small - one, in
    // every deployment here - and hashing a vector to avoid a handful of
    // integer compares would cost more than it saves.
    //
    // The key is a POINTER into the session's own vector, compared by value.
    // Safe because sessions_ is not modified while this lock is held, so no
    // entry can move or die during the loop below.
    struct VolumeMemo {
        const std::vector<uint64_t>* thresholds;
        wire::Update update;
    };
    struct PriceMemo {
        const std::vector<uint32_t>* thresholds;
        wire::Update update;
    };
    std::vector<VolumeMemo> volume_memo;
    std::vector<PriceMemo> price_memo;

    // Scratch for the band walks, reused across every session in this publish.
    // wants no allocation on hot paths; these still allocate on the first
    // walk, but no longer once per session.
    std::vector<consolidated::NotionalFill> bid_fills;
    std::vector<consolidated::NotionalFill> ask_fills;
    std::vector<consolidated::BpsFill> bid_bps;
    std::vector<consolidated::BpsFill> ask_bps;

    // Both helpers return an INDEX, not a reference: they push_back into the
    // memo vector, and a reference handed out here would dangle the moment a
    // later session introduced a new threshold set. The caller re-indexes at
    // the point of use.
    //
    // FillHeader's server_ts_ns is therefore shared by every session on the
    // same thresholds. That is more accurate, not less - they are all being
    // published at one instant - and it removes a clock read per session.
    const auto volume_message = [&](const std::vector<uint64_t>& thresholds) -> size_t {
        for (size_t i = 0; i < volume_memo.size(); ++i) {
            if (*volume_memo[i].thresholds == thresholds) {
                return i;
            }
        }
        consolidated::FillToNotionalBands(bids, thresholds, bid_fills);
        consolidated::FillToNotionalBands(asks, thresholds, ask_fills);

        wire::Update update;
        FillHeader(update, instrument);
        wire::VolumeBands* bands = update.mutable_volume_bands();
        // Slippage reference: the top of each side, or 0 if that side is
        // empty. Sent once per message, not per band.
        bands->set_best_bid(bids.empty() ? 0 : bids.front().price);
        bands->set_best_ask(asks.empty() ? 0 : asks.front().price);
        for (size_t i = 0; i < thresholds.size(); ++i) {
            *bands->add_bid_bands() = ToWire(bid_fills[i], thresholds[i]);
            *bands->add_ask_bands() = ToWire(ask_fills[i], thresholds[i]);
        }
        volume_memo.push_back({&thresholds, std::move(update)});
        return volume_memo.size() - 1;
    };

    const auto price_message = [&](const std::vector<uint32_t>& thresholds) -> size_t {
        for (size_t i = 0; i < price_memo.size(); ++i) {
            if (*price_memo[i].thresholds == thresholds) {
                return i;
            }
        }
        consolidated::FillToBpsBands(bids, thresholds, /*is_bid=*/true, bid_bps);
        consolidated::FillToBpsBands(asks, thresholds, /*is_bid=*/false, ask_bps);

        wire::Update update;
        FillHeader(update, instrument);
        wire::PriceBands* bands = update.mutable_price_bands();
        for (size_t i = 0; i < thresholds.size(); ++i) {
            *bands->add_bid_bands() = ToWire(bid_bps[i], thresholds[i]);
            *bands->add_ask_bands() = ToWire(ask_bps[i], thresholds[i]);
        }
        price_memo.push_back({&thresholds, std::move(update)});
        return price_memo.size() - 1;
    };

    for (auto& [id, subscription] : sessions_) {
        // Same filter as PublishBbo - symbol and market in one compare.
        if (subscription.instrument != instrument) {
            continue;  // a different book
        }
        if (subscription.wants_volume_bands) {
            const size_t index = volume_message(subscription.notional_bands);
            // seq is the ONLY per-session field. It is stamped on the shared
            // message immediately before the push that copies it - Push takes
            // its argument by value, so each session still receives its own
            // copy with its own seq. What is shared is the work of BUILDING
            // the message, not the message object.
            volume_memo[index].update.set_seq(subscription.next_seq++);
            subscription.channel->Push(volume_memo[index].update);
        }

        if (subscription.wants_price_bands) {
            const size_t index = price_message(subscription.bps_bands);
            price_memo[index].update.set_seq(subscription.next_seq++);
            subscription.channel->Push(price_memo[index].update);
        }
    }
}

}  // namespace market_data
