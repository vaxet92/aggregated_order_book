#pragma once

// ZmqProviderSink: the provider-side half of the DEALER/ROUTER transport -
// one DEALER socket per provider connection, connect-and-queue-to-HWM
// semantics so startup order never matters, and Send*/On* methods that mirror
// Core::Enqueue*'s bool contract exactly (false means the caller must
// resync). Everything must be called from the provider's own io_context
// thread; the class takes no lock.

#include <zmq.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "md_core/types.h"
#include "md_core/venue_health.h"
#include "wire_format.h"

namespace market_data::wire {

// Provider-side half of the DEALER/ROUTER transport. One instance per
// provider connection (today: one per (venue, instrument))). This
// is the class that replaces the in-process update/quote/health sink lambdas
// the single-process build wired to Provider - those called
// core.EnqueueUpdate/EnqueueQuote/EnqueueHealth directly; these call the
// equivalent Send* here, which puts
// the same bytes on a socket instead of into an in-process ring.
//
// Threading: every method must be called from ONE thread only - the
// provider's own io_context thread, which is already the sole caller of
// Provider::CallBack/QuoteCallBack/HealthCallback (md_provider.h). ZeroMQ
// sockets are not thread-safe, but a socket touched from exactly one thread
// needs no locking, and this class adds none.
class ZmqProviderSink {
   public:
    // `endpoint` is typically ipc:///run/md/core.ipc. Connects
    // immediately - DEALER's connect() queues to the HWM and delivers once
    // md_core binds, so construction does not care whether md_core is up yet
    // ("startup ordering").
    ZmqProviderSink(zmq::context_t& ctx, std::string_view endpoint, InstrumentKey instrument,
                    std::string_view venue_name);

    // Send kHello. Must be called exactly once, before any other Send*/On*
    // call - md_core binds this connection's identity to a venue_slot at
    // kHello (the "accept is registration") and has nothing to attribute
    // messages to before that.
    void SendHello();

    // Send kGoodbye. Clean-shutdown-only. Changes no behaviour on the wire -
    // a crashed process sends no goodbye and md_core learns of the
    // disconnect via ROUTER_NOTIFY instead - it only distinguishes clean
    // shutdown from crash in the log.
    void SendGoodbye(GoodbyeReason reason);

    // Invoked when a message that must not be lost - a depth delta or a health
    // transition - could not be sent (the "caller must resync"). Continuity
    // and resynchronization belong to the provider, which owns
    // RequestResync(); md_provider_main.cpp binds this to it once the Provider
    // exists, which it does not yet when the sink is constructed. A dropped
    // quote never calls this - the next quote supersedes it whole. Called on
    // the provider's io_context thread, same as every other method here.
    void SetResyncHook(std::function<void()> hook) { resync_hook_ = std::move(hook); }

    // Bind directly to Provider::CallBack / QuoteCallBack / HealthCallback.
    void OnUpdate(BookUpdate&& update);
    void OnQuote(const BboQuote& quote);
    void OnHealth(const VenueHealthEvent& event);

    // The primitives OnUpdate/OnHealth build on, exposed
    // directly for testing without a Provider around. Mirror
    // Core::EnqueueUpdate/EnqueueHealth's contract exactly (md_core.h) - false means
    // "caller resyncs" and true means "successfully sent". The "caller resyncs" policy is the same as the in-process
    // sink's, which is why "backpressure maps onto the policy that already exists"): false means the caller must
    // resync/escalate.
    bool SendUpdate(const BookUpdate& update);
    bool SendHealth(const VenueHealthEvent& event);

    // Overflow observability, same shape as Core::OverflowCount/QuoteDropCount.
    uint64_t OverflowCount() const { return overflow_count_; }
    uint64_t QuoteDropCount() const { return quote_drop_count_; }

   private:
    bool SendFrame();  // sends whatever Encode* just wrote into encode_buf_

    zmq::socket_t dealer_;
    InstrumentKey instrument_;
    std::string venue_name_;
    std::vector<std::byte> encode_buf_;  // reused across every Send* call
    std::function<void()> resync_hook_;  // set post-construction, see SetResyncHook

    // the "startup ordering" flag: the SAME send failure means different
    // things depending on whether any send has ever gone through, but there
    // is no way to tell "core hasn't started" from "core stopped draining"
    // with certainty from this side of the socket - both look like EAGAIN at
    // the HWM. This flag therefore changes only the log wording, never the
    // return value: every failure still means "the caller must resync."
    bool ever_sent_successfully_ = false;

    uint64_t overflow_count_ = 0;
    uint64_t quote_drop_count_ = 0;
};

}  // namespace market_data::wire
