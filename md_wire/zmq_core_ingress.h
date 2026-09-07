#pragma once

// ZmqCoreIngress: the core-side half of the DEALER/ROUTER transport - one
// ROUTER socket, one receive thread, decoding wire frames into
// Core::Register/Enqueue* calls. accept is registration (kHello binds an
// identity to a venue slot) and removal is the socket closing
// (ROUTER_NOTIFY disconnect -> Core::EnqueueDisconnect), never a timeout -
// see the class comment below for why silence and disconnect must never be
// conflated.

#include <zmq.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "md_core/md_core.h"
#include "md_core/types.h"
#include "md_core/venue_health.h"
#include "types/venue_registry.h"

namespace market_data::wire {

// Core-side half of the DEALER/ROUTER transport - the mirror of
// ZmqProviderSink. Binds one ROUTER, runs one receive thread, and turns the
// bytes back into calls on Core:
//
//   kHello        -> Core::RegisterVenueName + Core::EnqueueRegistration
//   kBookUpdate   -> Core::EnqueueUpdate
//   kBboQuote     -> Core::EnqueueQuote
//   kHealth       -> Core::EnqueueHealth
//   kGoodbye      -> a log line only (removal is the disconnect,
//                    never the goodbye)
//   ROUTER_NOTIFY -> Core::EnqueueDisconnect
//
// This is the component that replaces the N provider io_context threads as the
// producer of Core's per-venue SPSC queues. There is now ONE producer thread
// feeding every queue, which is still single-producer per queue - if anything a
// cleaner SPSC story than the callback wiring it replaces.
//
// THREADING: every Core call this class makes happens on its own receive
// thread. Core::RegisterVenueName is safe there (it only publishes into
// VenueRegistry, which is single-writer / many-reader); the actual per-venue
// book allocation runs later, on the consolidator thread, when it drains the
// VenueRegistration control message this class enqueues. Nothing here ever
// touches a FlatOrderBook.
//
// Owns its own zmq::context_t so Stop() can shutdown() it and unblock the
// receive thread cleanly. A provider test peer connects on a SEPARATE context
// over ipc:// - which is also what a real deployment looks like (separate
// processes).
class ZmqCoreIngress {
   public:
    // `endpoint` is typically ipc:///run/md/core.ipc.
    // `expected_instrument` is the one instrument this core serves today
    // (group size 1); a kHello naming any other instrument is refused.
    ZmqCoreIngress(std::string_view endpoint, Core& core, InstrumentKey expected_instrument);
    ~ZmqCoreIngress();

    ZmqCoreIngress(const ZmqCoreIngress&) = delete;
    ZmqCoreIngress& operator=(const ZmqCoreIngress&) = delete;

    // unlink()s a stale ipc:// socket file (a SIGKILL'd previous core leaves
    // one, and bind() then fails EADDRINUSE - safe because md_core is the only
    // process that binds this path), binds the ROUTER with
    // ROUTER_NOTIFY, and spawns the receive thread. Idempotent.
    void Start();

    // Stops the receive thread and joins it. Safe to call when not started and
    // from the destructor. After this returns, Core sees no more enqueues from
    // this class - call it BEFORE Core::Stop() so the final drain is clean.
    void Stop();

    std::string_view endpoint() const { return endpoint_; }

    // Observability, same shape as ZmqProviderSink's counters.
    //
    // data_before_hello: frames on a connection that has not sent a valid
    //   kHello yet - dropped, because Core never creates state from a data
    //   message.
    // enqueue_overflow: EnqueueUpdate/EnqueueHealth/EnqueueRegistration/
    //   EnqueueDisconnect that failed after the bounded retry - each one is a
    //   resync-required event on that venue.
    // Atomic load: written on thread_ (the receive loop) and read here by
    // whoever holds the ZmqCoreIngress - a monitor, or a test polling through
    // PollUntil. Only moves on the drop/overflow path, never per good
    // message, so seq_cst costs nothing that matters.
    uint64_t DataBeforeHelloCount() const { return data_before_hello_.load(); }

   private:
    void Run();  // receive-loop body, on thread_

    // Dispatches one already-received payload frame from `identity`.
    void HandlePayload(const std::string& identity, zmq::message_t& payload);
    // Handles a zero-length frame: ROUTER_NOTIFY connect (ignored) or
    // disconnect (enqueue removal).
    void HandleNotify(const std::string& identity);

    zmq::context_t context_;
    zmq::socket_t router_;
    Core& core_;
    // Singular today (group size 1, one instrument per core). A kHello
    // naming any other InstrumentKey - including the same symbol in the other
    // MarketType, which is a different order book (types/venue.h) - is refused.
    InstrumentKey expected_instrument_;
    std::string endpoint_;

    // ROUTER identity frame -> the slot that connection registered under. The
    // identity is transport-assigned and unforgeable, so this is what
    // attributes every later message rather than anything in the payload.
    std::unordered_map<std::string, VenueSlot> identity_to_slot_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    // Reused across messages so steady state allocates nothing on decode, the
    // same convention wire_codec.h describes for the encode side.
    BookUpdate decode_update_;
    BboQuote decode_quote_{};
    VenueHealthEvent decode_health_{};
    std::string decode_venue_name_;

    std::atomic<uint64_t> data_before_hello_{0};
    std::atomic<uint64_t> enqueue_overflow_{0};
    // Touched only on thread_ (the log-once latches), so plain bools are fine.
    bool logged_data_before_hello_ = false;
    bool logged_unknown_msg_type_ = false;
};

}  // namespace market_data::wire
