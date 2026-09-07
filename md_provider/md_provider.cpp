#include "md_provider.h"
#include "config/venues_config.h"
#include "logger/logger.h"
#include <root_certificates.hpp>

using namespace market_data;

Provider::Provider(const ProviderConfig& config, CallBack callback, QuoteCallBack quote_callback)
    : config(config),
      venue_market_str_(VenueConverter::ToVenueMarketString(config.venue_id, config.instrument)),
      running(false),
      ssl_ctx(ssl::context::tlsv12_client),
      health_timer_(ioc),
      reconnect_count(0),
      callback_(std::move(callback)),
      quote_callback_(std::move(quote_callback)) {
    // Load root certificates for SSL
    load_root_certificates(ssl_ctx);
    ssl_ctx.set_verify_mode(ssl::verify_peer);
}

void Provider::ResolveStreamPaths(std::string_view venue_symbol) {
    depth_path_ = ResolvePath(config.depth_path, venue_symbol);
    bbo_path_ = ResolvePath(config.bbo_path, venue_symbol);
}

Provider::~Provider() {
    Stop();
}

void Provider::Start() {
    if (running.exchange(true)) {
        return;  // Already running
    }

    Logger::Log(LogLevel::kInfo, "[{}] Starting provider...", venue_market_str_);

    worker_thread = std::thread([this]() { this->Run(); });
}

void Provider::Stop() {
    if (!running.exchange(false)) {
        return;  // Already stopped
    }

    Logger::Log(LogLevel::kInfo, "[{}] Stopping provider...", venue_market_str_);

    // Stop() marks each session as deliberately closed, which also silences
    // its on-closed callback - otherwise every socket would ask to be
    // reconnected while we are trying to exit.
    for (auto& slot : sessions_) {
        if (slot.session) slot.session->Stop();
    }

    ioc.stop();

    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void Provider::Run() {
    while (running) {
        try {
            // Let the subclass drop per-connection state, and do any blocking
            // setup a connection depends on, before the new sessions start
            // delivering messages. A false return means that setup failed -
            // treat it exactly like a dropped connection so it backs off and
            // retries rather than connecting with state it knows is wrong.
            if (!OnReconnect()) {
                if (running) {
                    HandleReconnection();
                }
                continue;
            }

            // Reset io_context for a new run
            ioc.restart();

            // Full rebuild after a total outage or a resync. The filters
            // start fresh: the venue's ids keep climbing, so carrying the old
            // high-water mark would also work, but a clean filter means
            // socket 0's opening snapshot is unconditionally taken and there
            // is no cross-outage state left to reason about.
            depth_dedup_ = SeqDedup{};
            bbo_dedup_ = SeqDedup{};
            sessions_.clear();
            depth_live_ = 0;
            bbo_live_ = 0;

            // config.connections redundant sockets, each carrying both
            // streams. At the default of 1 this is exactly the previous
            // single-session behaviour.
            const uint32_t connections = std::max<uint32_t>(1, config.connections);

            Logger::Log(LogLevel::kInfo, "[{}] Connecting to {} ({} connection(s))...", venue_market_str_, GetHost(),
                        connections);
            sessions_.resize(connections);
            for (uint32_t i = 0; i < connections; ++i) {
                CreateCombinedSession(i);
            }

            // Armed AFTER the sessions exist, so the first tick sees the real
            // live counts rather than zero and does not report a spurious
            // kDisconnected on every startup.
            ScheduleHealthCheck();

            // Run the io_context - drives every session on this one thread,
            // which is why none of the per-session state needs a lock.
            //
            // this returns only when NO work remains, i.e. every socket
            // is gone. One socket dying leaves the others running, so a
            // partial failure never reaches the teardown below - which is
            // precisely the point of redundancy.
            ioc.run();

            if (running) {
                if (resync_requested_.exchange(false)) {
                    // Deliberate resync after a depth gap - NOT a connection
                    // failure, so it must not consume the reconnect-attempt
                    // budget that permanently stops the provider.
                    Logger::Log(LogLevel::kInfo, "[{}] Resyncing after depth gap", venue_market_str_);
                    std::this_thread::sleep_for(std::chrono::milliseconds(RESYNC_DELAY_MS));
                } else {
                    // Connection dropped, attempt reconnection
                    HandleReconnection();
                }
            }

        } catch (const std::exception& e) {
            Logger::Log(LogLevel::kError, "[{}] Error: {}", venue_market_str_, e.what());

            if (running) {
                HandleReconnection();
            }
        }
    }

    Logger::Log(LogLevel::kInfo, "[{}] Provider stopped", venue_market_str_);
}

void Provider::HandleReconnection() {
    if (!running) return;

    if (++reconnect_count > MAX_RECONNECT_ATTEMPTS) {
        Logger::Log(LogLevel::kError, "[{}] Max reconnect attempts reached. Stopping.", venue_market_str_);
        running = false;
        return;
    }

    // Exponential backoff with cap
    uint64_t delay_ms =
        std::min(INITIAL_RECONNECT_DELAY_MS * (uint64_t{1} << (reconnect_count - 1)), MAX_RECONNECT_DELAY_MS);

    Logger::Log(LogLevel::kWarning, "[{}] Reconnecting in {}ms (attempt {})", venue_market_str_, delay_ms,
                reconnect_count);

    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

void Provider::CreateCombinedSession(uint32_t index) {
    SessionSlot& slot = sessions_[index];

    // One socket, so it counts as live on both streams - and dying takes
    // both down together, the accepted cost of sharing the connection
    // (see GetCombinedPath()).
    ++depth_live_;
    ++bbo_live_;

    slot.session = std::make_shared<WebSocketSessionSSL>(
        ioc, ssl_ctx, [this, index](const std::string& msg) { this->OnCombinedMessage(msg, index); });
    slot.session->SetOnClosed([this, index]() { this->OnCombinedSessionClosed(index); });
    slot.session->Run(GetHost(), GetPort(), GetCombinedPath(), CombinedSubscriptionMessage());
}

void Provider::OnCombinedSessionClosed(uint32_t index) {
    // One socket, both streams: it dying takes both counts down together -
    // the accepted cost of sharing the connection (see CreateCombinedSession).
    if (depth_live_ > 0) {
        --depth_live_;
    }
    if (bbo_live_ > 0) {
        --bbo_live_;
    }

    Logger::Log(LogLevel::kWarning, "[{}] combined connection {}/{} closed, {} still live", venue_market_str_,
                index + 1, sessions_.size(), depth_live_);

    // Same reasoning as OnDepthSessionClosed: only a TOTAL outage (every
    // combined socket gone) invalidates sync state.
    if (depth_live_ == 0) {
        Logger::Log(LogLevel::kError, "[{}] all connections down", venue_market_str_);
        OnReconnect();
    }
}

void Provider::ScheduleHealthCheck() {
    health_timer_.expires_after(std::chrono::milliseconds(HEALTH_CHECK_INTERVAL_MS));
    health_timer_.async_wait([this](const boost::system::error_code& ec) {
        // operation_aborted is the normal path on Stop()/RequestResync(),
        // which cancel the timer. Anything else and we simply stop
        // rescheduling rather than spinning on a broken timer.
        if (ec) {
            return;
        }
        CheckHealth();

        // the timer must NOT rearm once every socket on both streams is
        // gone. ioc.run() returns only when no work remains, and a repeating
        // timer is work forever - so rescheduling unconditionally would keep
        // run() from ever returning, the reconnect path in Run() would never
        // execute, and a fully disconnected venue would stay dead for the
        // life of the process.
        //
        // The CheckHealth() above has already published kDisconnected for
        // both streams by this point, so stopping here loses no signal: the
        // last thing the timer does is report the outage, then it gets out of
        // the way so the reconnect can happen.
        if (running && (depth_live_ > 0 || bbo_live_ > 0)) {
            ScheduleHealthCheck();
        }
    });
}

void Provider::CheckHealth() {
    const int64_t now = GetMonotonicNs();

    // "connected" is depth_live_ > 0, not "all sockets up". With
    // redundant connections, one socket dying leaves the others delivering
    // the same data - the venue is still healthy and must not be excluded.
    // Only a total loss of the stream is evidence of anything.
    //
    // kResyncing is left alone. A timer has nothing useful to say about a
    // stream we switched off ourselves: it would report kStale once the old
    // stamp aged past the backstop, or kNoData if the stamp were cleared -
    // both less informative than "we are rebuilding this", and both would
    // overwrite it. The state is cleared by the first message that arrives
    // after the venue comes back (see NeedsImmediatePromotion).
    if (last_depth_health_ != VenueHealth::kResyncing) {
        PublishHealth(StreamKind::kDepth,
                      ClassifyFeed(depth_live_ > 0, last_depth_message_mono_ns_, now, config.depth_backstop_ns));
    }
    if (last_bbo_health_ != VenueHealth::kResyncing) {
        PublishHealth(StreamKind::kBbo,
                      ClassifyFeed(bbo_live_ > 0, last_bbo_message_mono_ns_, now, config.bbo_backstop_ns));
    }
}

void Provider::PublishHealth(StreamKind stream, VenueHealth health) {
    VenueHealth& previous = (stream == StreamKind::kDepth) ? last_depth_health_ : last_bbo_health_;
    if (health == previous) {
        return;  // edge-triggered - nothing changed, say nothing
    }

    const char* stream_name = (stream == StreamKind::kDepth) ? "depth" : "bbo";
    Logger::Log(health == VenueHealth::kLive ? LogLevel::kInfo : LogLevel::kWarning, "[{}] {} health: {} -> {}",
                venue_market_str_, stream_name, ToString(previous), ToString(health));

    previous = health;

    if (health_callback_) {
        // decided_mono_ns, not the time Core reads it: once this travels
        // through an SPSC queue it is consumed later than it was produced,
        // and the verdict has to carry its own timestamp to stay meaningful.
        health_callback_(VenueHealthEvent{
            .venue = config.venue_id,
            .stream = stream,
            .health = health,
            .decided_mono_ns = GetMonotonicNs(),
        });
    }
}

bool Provider::AcceptDepth(uint64_t id, uint32_t conn_index, bool venue_reset) {
    // No per-socket adjustment: venue_reset already means "the id moves
    // backwards", and an ordinary snapshot passes false so the `<=` rule
    // decides on the data alone.
    if (!depth_dedup_.Accept(id, conn_index, venue_reset)) {
        // Logged once per episode, exactly at the threshold. A stuck filter
        // drops EVERY message, so logging each one would bury the alarm in
        // its own noise.
        if (depth_dedup_.ConsecutiveDrops() == SeqDedup::kSuspiciousDropStreak) {
            Logger::Log(LogLevel::kError, "[{}] depth dedup dropped {} in a row - missed a sequence reset?",
                        venue_market_str_, SeqDedup::kSuspiciousDropStreak);
        }
        return false;
    }
    return true;
}

bool Provider::AcceptBbo(uint64_t id, uint32_t conn_index) {
    if (!bbo_dedup_.Accept(id, conn_index, /*is_reset=*/false)) {
        if (bbo_dedup_.ConsecutiveDrops() == SeqDedup::kSuspiciousDropStreak) {
            Logger::Log(LogLevel::kError, "[{}] bbo dedup dropped {} in a row - non-monotonic quote ids?",
                        venue_market_str_, SeqDedup::kSuspiciousDropStreak);
        }
        return false;
    }
    return true;
}

void Provider::Emit(BookUpdate&& update) {
    // Stamped here, at the single exit point, rather than in each venue's
    // message handler. Every update that reaches the core carries a
    // monotonic arrival time, or none of them do - there is no way for one
    // venue to be silently missing it, which would make that venue look
    // permanently stale.
    update.recv_mono_ns = GetMonotonicNs();

    if (callback_) {
        callback_(std::move(update));
    }
}

void Provider::EmitQuote(BboQuote& quote) {
    // Separate stamp from the depth stream on purpose: depth and fast-BBO
    // are different sockets, so one can die while the other keeps streaming.
    // Sharing a stamp would hide exactly that failure.
    quote.recv_mono_ns = GetMonotonicNs();

    if (quote_callback_) {
        quote_callback_(quote);
    }
}

void Provider::PostToIoContext(std::function<void()> fn) {
    net::post(ioc, std::move(fn));
}

void Provider::RequestResync() {
    resync_requested_ = true;

    // announced BEFORE the sockets are torn down, and this ordering is
    // the whole fix. Stop() sets `stopped`, which suppresses NotifyClosed() -
    // so depth_live_ never drops, health would stay kLive, and Core would go
    // on merging a book we have already decided is WRONG for the entire
    // resync window. Every update from the other venues during that window
    // publishes a consolidated book containing known-bad levels.
    //
    // Both streams, not just depth: the gap is a depth-stream fact, but this
    // function stops every socket on both, so neither will deliver until the
    // provider comes back. A venue that cannot send is not one to merge.
    PublishHealth(StreamKind::kDepth, VenueHealth::kResyncing);
    PublishHealth(StreamKind::kBbo, VenueHealth::kResyncing);

    // Every socket goes down: a gap means the book is WRONG, and no
    // surviving connection can repair that - they all carry the same broken
    // sequence. This is the one case where redundancy does not help.
    for (auto& slot : sessions_) {
        if (slot.session) slot.session->Stop();
    }
    ioc.stop();
}

int64_t Provider::GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t Provider::GetMonotonicNs() {
    // steady_clock is guaranteed never to go backwards - that is the whole
    // reason it is used here rather than system_clock. Measured at ~14ns per
    // call on an M4 Pro (-O2), against the ~13ns GetCurrentTimeMs already
    // costs, so the second stamp per message is not worth avoiding.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}