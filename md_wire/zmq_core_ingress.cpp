#include "zmq_core_ingress.h"

#include <cerrno>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include <unistd.h>

#include "logger/logger.h"
#include "types/venue.h"
#include "wire_codec.h"

namespace market_data::wire {

namespace {

constexpr std::string_view kIpcPrefix = "ipc://";

// One frame off the ROUTER. Returns false only when the context has been
// shut down (Stop()) or the socket errored - the caller then exits the loop.
bool RecvFrame(zmq::socket_t& sock, zmq::message_t& out) {
    try {
        const zmq::recv_result_t r = sock.recv(out, zmq::recv_flags::none);
        return r.has_value();
    } catch (const zmq::error_t& e) {
        if (e.num() != ETERM) {
            Logger::Log(LogLevel::kError, "[Ingress] recv failed: {}", e.what());
        }
        return false;
    }
}

}  // namespace

ZmqCoreIngress::ZmqCoreIngress(std::string_view endpoint, Core& core, InstrumentKey expected_instrument)
    : context_(1),
      router_(context_, zmq::socket_type::router),
      core_(core),
      expected_instrument_(expected_instrument),
      endpoint_(endpoint) {}

ZmqCoreIngress::~ZmqCoreIngress() {
    Stop();
}

void ZmqCoreIngress::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;  // already started
    }

    // A SIGKILL'd previous core leaves the socket file behind and bind() then
    // fails EADDRINUSE. Safe to remove unconditionally: the direction rule
    // makes md_core the only process that ever binds this path, so there is no
    // peer whose socket this could be stealing. tcp:// has no file.
    if (endpoint_.rfind(kIpcPrefix, 0) == 0) {
        const std::string path = endpoint_.substr(kIpcPrefix.size());
        if (::unlink(path.c_str()) == 0) {
            Logger::Log(LogLevel::kInfo, "[Ingress] removed stale socket file {}", path);
        } else if (errno != ENOENT) {
            Logger::Log(LogLevel::kWarning, "[Ingress] could not unlink {} (errno {}) - bind may fail", path, errno);
        }
    }

    // Must be set BEFORE bind(). ROUTER_NOTIFY makes the ROUTER deliver a
    // zero-length message when a peer connects and when it disconnects, in
    // order with that peer's own data - which is what restores
    // the "removal is the socket closing, never silence".
    const int notify = ZMQ_NOTIFY_CONNECT | ZMQ_NOTIFY_DISCONNECT;
    router_.set(zmq::sockopt::router_notify, notify);

    router_.bind(endpoint_);
    Logger::Log(LogLevel::kInfo, "[Ingress] ROUTER bound on {}", endpoint_);

    thread_ = std::thread([this] { Run(); });
}

void ZmqCoreIngress::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;  // never started, or already stopped
    }
    // Unblocks the recv in Run(): every blocked call on a socket of this
    // context throws ETERM, which RecvFrame reports as "exit the loop".
    context_.shutdown();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void ZmqCoreIngress::Run() {
    while (running_.load(std::memory_order_acquire)) {
        zmq::message_t identity;
        if (!RecvFrame(router_, identity)) {
            break;
        }
        std::string id(static_cast<const char*>(identity.data()), identity.size());

        if (!identity.more()) {
            Logger::Log(LogLevel::kWarning, "[Ingress] identity frame with no payload - ignoring");
            continue;
        }

        zmq::message_t payload;
        if (!RecvFrame(router_, payload)) {
            break;
        }

        // Our wire is always exactly one payload frame. Anything more
        // is malformed; drain and discard the rest so the stream stays aligned.
        bool extra = false;
        while (payload.more()) {
            extra = true;
            zmq::message_t discard;
            if (!RecvFrame(router_, discard)) {
                return;
            }
        }
        if (extra) {
            Logger::Log(LogLevel::kWarning, "[Ingress] multi-frame payload - discarded trailing frames");
        }

        if (payload.size() == 0) {
            HandleNotify(id);
        } else {
            HandlePayload(id, payload);
        }
    }
}

void ZmqCoreIngress::HandleNotify(const std::string& identity) {
    const auto it = identity_to_slot_.find(identity);
    if (it == identity_to_slot_.end()) {
        // A connect notify, or the disconnect of a peer that never sent a
        // valid kHello. Core has no state for it - nothing to do.
        return;
    }

    const VenueSlot slot = it->second;
    identity_to_slot_.erase(it);

    if (!core_.EnqueueDisconnect(slot)) {
        ++enqueue_overflow_;
        Logger::Log(LogLevel::kError,
                    "[Ingress] slot {} disconnect could not be enqueued - venue merged until core drains",
                    VenueSlotIndex(slot));
        return;
    }
    Logger::Log(LogLevel::kInfo, "[Ingress] venue slot {} disconnected", VenueSlotIndex(slot));
}

void ZmqCoreIngress::HandlePayload(const std::string& identity, zmq::message_t& payload) {
    const std::span<const std::byte> buf(static_cast<const std::byte*>(payload.data()), payload.size());

    WireHeader header{};
    if (!DecodeHeader(buf, header)) {
        Logger::Log(LogLevel::kWarning, "[Ingress] payload shorter than a WireHeader ({} bytes) - dropped",
                    payload.size());
        return;
    }

    auto note_early = [this](const char* what) {
        ++data_before_hello_;
        if (!logged_data_before_hello_) {
            Logger::Log(LogLevel::kWarning, "[Ingress] {} before kHello - dropped (this and further occurrences)",
                        what);
            logged_data_before_hello_ = true;
        }
    };

    switch (static_cast<WireMsgType>(header.msg_type)) {
        case WireMsgType::kHello: {
            InstrumentKey instrument{};
            if (!DecodeHello(buf, instrument, decode_venue_name_)) {
                Logger::Log(LogLevel::kWarning, "[Ingress] malformed kHello - dropped");
                return;
            }
            if (instrument != expected_instrument_) {
                Logger::Log(LogLevel::kError,
                            "[Ingress] kHello for instrument {} but this core serves {} - connection refused",
                            VenueConverter::ToInstrumentString(instrument),
                            VenueConverter::ToInstrumentString(expected_instrument_));
                return;
            }
            // Slot assignment only - safe on this thread. The book allocation
            // runs on the consolidator when it drains the VenueRegistration
            // below, ordered ahead of this connection's first update.
            const std::optional<VenueSlot> slot = core_.RegisterVenueName(decode_venue_name_);
            if (!slot.has_value()) {
                Logger::Log(LogLevel::kError,
                            "[Ingress] kHello from venue '{}' refused - unknown venue or registry full",
                            decode_venue_name_);
                return;
            }
            identity_to_slot_[identity] = *slot;
            if (!core_.EnqueueRegistration(*slot)) {
                ++enqueue_overflow_;
                Logger::Log(LogLevel::kError, "[Ingress] venue '{}' slot {} registration could not be enqueued",
                            decode_venue_name_, VenueSlotIndex(*slot));
                return;
            }
            Logger::Log(LogLevel::kInfo, "[Ingress] venue '{}' -> slot {}", decode_venue_name_, VenueSlotIndex(*slot));
            return;
        }

        case WireMsgType::kBookUpdate: {
            const auto it = identity_to_slot_.find(identity);
            if (it == identity_to_slot_.end()) {
                note_early("kBookUpdate");
                return;
            }
            if (!DecodeBookUpdate(buf, decode_update_)) {
                Logger::Log(LogLevel::kWarning, "[Ingress] malformed kBookUpdate - dropped, resync required");
                return;
            }
            if (!core_.EnqueueUpdate(it->second, std::move(decode_update_))) {
                ++enqueue_overflow_;
                Logger::Log(LogLevel::kError,
                            "[Ingress] slot {} core queue full - depth update dropped, resync required",
                            VenueSlotIndex(it->second));
            }
            return;
        }

        case WireMsgType::kBboQuote: {
            const auto it = identity_to_slot_.find(identity);
            if (it == identity_to_slot_.end()) {
                note_early("kBboQuote");
                return;
            }
            if (!DecodeBboQuote(buf, decode_quote_)) {
                Logger::Log(LogLevel::kWarning, "[Ingress] malformed kBboQuote - dropped");
                return;
            }
            // void: a quote is a complete top-of-book snapshot, so Core drops
            // and counts it on overflow rather than asking for a resync.
            core_.EnqueueQuote(it->second, decode_quote_);
            return;
        }

        case WireMsgType::kHealth: {
            const auto it = identity_to_slot_.find(identity);
            if (it == identity_to_slot_.end()) {
                note_early("kHealth");
                return;
            }
            if (!DecodeHealth(buf, decode_health_)) {
                Logger::Log(LogLevel::kWarning, "[Ingress] malformed kHealth - dropped");
                return;
            }
            if (!core_.EnqueueHealth(it->second, decode_health_)) {
                ++enqueue_overflow_;
                Logger::Log(LogLevel::kError,
                            "[Ingress] slot {} core queue full - health event dropped, resync required",
                            VenueSlotIndex(it->second));
            }
            return;
        }

        case WireMsgType::kGoodbye: {
            GoodbyeReason reason{};
            if (!DecodeGoodbye(buf, reason)) {
                Logger::Log(LogLevel::kWarning, "[Ingress] malformed kGoodbye - ignored");
                return;
            }
            const auto it = identity_to_slot_.find(identity);
            const int slot_index = it == identity_to_slot_.end() ? -1 : static_cast<int>(VenueSlotIndex(it->second));
            // No removal here on purpose. ROUTER_NOTIFY is the trustworthy
            // disconnect signal and is ordered after this venue's last data
            // frame; kGoodbye only tells a clean stop from a crash in the log.
            Logger::Log(LogLevel::kInfo, "[Ingress] kGoodbye (reason {}) from slot {} - awaiting disconnect notify",
                        static_cast<uint32_t>(reason), slot_index);
            return;
        }
    }

    // Unrecognised msg_type: skip it, do not desync. the additive-only
    // rule plus the message boundary make this safe - an older core drops a
    // newer message type it does not know rather than misreading the stream.
    if (!logged_unknown_msg_type_) {
        Logger::Log(LogLevel::kWarning, "[Ingress] unrecognised wire msg_type {} - skipped (this and further)",
                    header.msg_type);
        logged_unknown_msg_type_ = true;
    }
}

}  // namespace market_data::wire
