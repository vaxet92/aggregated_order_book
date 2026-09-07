#include "zmq_provider_sink.h"

#include "logger/logger.h"
#include "wire_codec.h"

namespace market_data::wire {

namespace {

// DEALER send is bounded, never blocking - the key point that a
// provider must never block indefinitely on send, or it stops draining its
// exchange socket and turns a bounded queue problem into an unbounded
// staleness problem. ZMQ_DONTWAIT converts "socket at its HWM" into an
// immediate EAGAIN instead of a stall - the same trade EnqueueUpdate's
// bounded spin-then-fail already makes for the in-process queue (md_core.h).
constexpr int kSendFlags = ZMQ_DONTWAIT;

// Matches kProviderQueueCapacity (provider_message.h), not libzmq's default
// of 1000: this socket replaces that in-process queue on the wire, so
// backpressure should begin at the point the design already reasoned about,
// not at an arbitrary transport default.
constexpr int kSendHwm = 256;

}  // namespace

ZmqProviderSink::ZmqProviderSink(zmq::context_t& ctx, std::string_view endpoint, InstrumentKey instrument,
                                 std::string_view venue_name)
    : dealer_(ctx, zmq::socket_type::dealer), instrument_(instrument), venue_name_(venue_name) {
    // No explicit ZMQ_IDENTITY: ROUTER assigns one on connect. Treats
    // every connection as a fresh registration - a reconnect after a drop is
    // a new kHello, not a resumed one - so an application-chosen identity
    // would buy nothing here.
    dealer_.set(zmq::sockopt::sndhwm, kSendHwm);
    // LINGER 0, not the default -1. Default LINGER blocks socket close (and
    // therefore zmq_ctx_term / process shutdown) until every queued message
    // is either delivered or the peer connects and drains it - if core never
    // comes up at all, that is forever. Every message type here already
    // tolerates loss on close: kBookUpdate/kHealth failures already mean
    // "caller resyncs" and kGoodbye is already best-effort (a
    // missing goodbye just falls back to ROUTER_NOTIFY's disconnect signal).
    // There is nothing gained by blocking shutdown to deliver bytes nobody
    // is guaranteed to be listening for.
    dealer_.set(zmq::sockopt::linger, 0);
    dealer_.connect(std::string(endpoint));
}

bool ZmqProviderSink::SendFrame() {
    zmq::message_t msg(encode_buf_.data(), encode_buf_.size());
    zmq::send_result_t result = dealer_.send(msg, static_cast<zmq::send_flags>(kSendFlags));
    if (result.has_value()) {
        ever_sent_successfully_ = true;
        return true;
    }
    return false;
}

void ZmqProviderSink::SendHello() {
    EncodeHello(instrument_, venue_name_, encode_buf_);
    if (!SendFrame()) {
        // kHello failing means the connection cannot even register - loud,
        // not silent, because every later message on this connection is
        // meaningless without it (accept is registration).
        Logger::Log(LogLevel::kError, "[{}] kHello send failed - core unreachable or connection at capacity",
                    venue_name_);
    }
}

void ZmqProviderSink::SendGoodbye(GoodbyeReason reason) {
    EncodeGoodbye(reason, encode_buf_);
    // Best-effort: a failed goodbye changes nothing (ROUTER_NOTIFY
    // covers the disconnect either way), so there is nothing to escalate to.
    static_cast<void>(SendFrame());
}

bool ZmqProviderSink::SendUpdate(const BookUpdate& update) {
    EncodeBookUpdate(update, encode_buf_);
    if (SendFrame()) {
        return true;
    }
    ++overflow_count_;
    return false;
}

void ZmqProviderSink::OnUpdate(BookUpdate&& update) {
    if (!SendUpdate(update)) {
        // A lost depth delta breaks continuity and only the provider can fix
        // it, by resyncing. The hook is bound to Provider::RequestResync
        // - the same recovery the in-process update sink triggered.
        Logger::Log(LogLevel::kError, "[{}] {} - depth update dropped, resync required", venue_name_,
                    ever_sent_successfully_ ? "core stuck (send at capacity)" : "core not up yet");
        if (resync_hook_) {
            resync_hook_();
        }
    }
}

void ZmqProviderSink::OnQuote(const BboQuote& quote) {
    EncodeBboQuote(quote, encode_buf_);
    if (!SendFrame()) {
        // Dropped and counted, never escalated: a quote is a complete
        // top-of-book snapshot, so the next one supersedes it whole -
        // identical policy to Core::EnqueueQuote (md_core.h).
        ++quote_drop_count_;
    }
}

bool ZmqProviderSink::SendHealth(const VenueHealthEvent& event) {
    EncodeHealth(event, encode_buf_);
    if (SendFrame()) {
        return true;
    }
    ++overflow_count_;
    return false;
}

void ZmqProviderSink::OnHealth(const VenueHealthEvent& event) {
    if (!SendHealth(event)) {
        // A state TRANSITION, not a sample - a lost kStale would leave a
        // dead venue merged forever. Same escalation the in-process
        // Core::EnqueueHealth caller performed.
        Logger::Log(LogLevel::kError, "[{}] {} - health event dropped, resync required", venue_name_,
                    ever_sent_successfully_ ? "core stuck (send at capacity)" : "core not up yet");
        if (resync_hook_) {
            resync_hook_();
        }
    }
}

}  // namespace market_data::wire
