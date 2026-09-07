#pragma once

// The internal provider <-> core wire format: WireHeader plus one flat POD
// payload struct per message type (kHello, kBookUpdate, kBboQuote, kHealth,
// kGoodbye), each with static_assert-pinned size and field offsets so a
// compiler or struct-order change fails the build instead of corrupting a
// price at runtime. Not the client-facing gRPC wire - see the file comment
// below for that boundary.

#include <cstddef>
#include <cstdint>

#include "md_core/types.h"

namespace market_data::wire {

// The internal provider <-> core wire
// NOT the client-facing gRPC wire - that translation lives in
// aggregator/wire_translation.h and is a different boundary (protobuf, lower
// rate, external consumers). This one is flat POD, both ends are ours, and it
// exists to avoid handing a second JSON/protobuf parse to md_core's hot path.
//
// Frame shape once this rides ZeroMQ:
//   frame 0: identity        - added/stripped by ROUTER, never written by us
//   frame 1: WireHeader + one fixed payload struct [+ contiguous PriceLevel run]
//

inline constexpr uint16_t kWireVersion = 1;

enum class WireMsgType : uint16_t {
    kHello = 1,
    kBookUpdate = 2,
    kBboQuote = 3,
    kHealth = 4,
    kGoodbye = 5,
};

// Every frame 1 payload starts with this. `wire_version` is checked once, at
// kHello (DESIGN.md the "version at the handshake, not per message") - it
// still rides every message here because Decode() is a pure function with no
// connection state of its own; the caller (Phase 3/4's connection handling)
// is what remembers a handshake happened and would reject a mid-stream
// version change as a protocol violation rather than re-negotiating.
struct WireHeader {
    uint16_t msg_type;      // WireMsgType
    uint16_t wire_version;  // kWireVersion
};
static_assert(sizeof(WireHeader) == 4);
static_assert(offsetof(WireHeader, msg_type) == 0);
static_assert(offsetof(WireHeader, wire_version) == 2);

// kHello: "wire version, venue name, instrument". wire_version comes
// from the common WireHeader above; this is the rest.
//
// Fixed-size, NUL-terminated, zero-padded venue name rather than a
// length-prefixed string - this message is sent once per connection, so the
// wasted bytes are irrelevant, and a fixed size keeps the struct flat POD
// like everything else here instead of being the one variable-length message
// type.
inline constexpr std::size_t kVenueNameCapacity = 16;

struct WireHelloPayload {
    uint32_t instrument_packed;  // InstrumentKey::Packed()
    char venue_name[kVenueNameCapacity];
};
static_assert(sizeof(WireHelloPayload) == 20);
static_assert(offsetof(WireHelloPayload, instrument_packed) == 0);
static_assert(offsetof(WireHelloPayload, venue_name) == 4);

enum class GoodbyeReason : uint32_t {
    kShutdown = 0,  // clean stop - kGoodbye vs. no message is what tells
                    // md_core's disconnect handling "deliberate" from "crash"
                    // (the "kGoodbye still exists and still changes no
                    // behaviour - it only distinguishes clean shutdown from
                    // unexpected disconnect in the log").
    kError = 1,
};

struct WireGoodbyePayload {
    uint32_t reason;  // GoodbyeReason
};
static_assert(sizeof(WireGoodbyePayload) == 4);

// kBookUpdate fixed header. Followed in the frame by `bid_count` PriceLevels,
// then `ask_count` PriceLevels, contiguous - the "one contiguous PriceLevel
// run". market_data::PriceLevel (md_core/types.h) is reused
// as-is: it is already two uint64_t fields with no padding, which is exactly
// the flat form this wire wants, so a parallel wire-only level type would be
// pure duplication.
//
// No `venue` field: identity comes from the transport, not the payload
// - the ROUTER's identity frame is bound to a venue_slot at kHello,
// and every later message on that connection is attributed by which peer
// sent it, never by anything the payload claims. Decode() therefore leaves
// BookUpdate::venue at its default; the caller (the ROUTER-side ingress
// thread, not built yet) is what fills it in from the connection's bound
// slot. This is deliberate, not a missing field.
//
// `instrument_packed` IS carried per message, even though today's group size
// is 1 instrument per connection. Group size is documented as "a
// knob, not an architecture" - carrying the instrument on every message now
// means raising that knob later costs a config change, not a wire change.
struct WireBookUpdateHeader {
    uint32_t instrument_packed;
    uint64_t seq;              // venue-native sequence number
    int64_t prev_seq;          // continuity field: signed, OKX uses -1
    int64_t recv_ts_ns;        // wall clock, ours
    int64_t exch_ts_ns;        // wall clock, venue's
    int64_t provider_mono_ns;  // renamed on the wire from recv_mono_ns.
                               // Provider-local monotonic - meaningless
                               // compared against md_core's own clock the
                               // moment the endpoint stops being ipc://
                               // Decodes into BookUpdate::recv_mono_ns;
                               // only the WIRE name changes.
    uint32_t bid_count;
    uint32_t ask_count;
    uint8_t is_snapshot;  // bool: true = full replace, false = incremental delta
    uint8_t reserved[7];  // explicit padding, not compiler-inserted - keeps
                          // the struct's layout independent of whatever the
                          // compiler would have chosen
                          // "static_assert ... so a padding or compiler
                          // change fails the build instead of corrupting
                          // prices at runtime."
};
// sizeof is 64, not 60: instrument_packed (u32) leaves 4 bytes of padding
// before seq so the first uint64_t field lands 8-byte aligned, and the
// trailing is_snapshot + reserved[7] pads the tail back to an 8-byte multiple.
// Spelled out here rather than left implicit, per the reason for
// asserting on offsets at all: this is exactly the kind of gap a compiler or
// struct-order change could silently move.
static_assert(sizeof(WireBookUpdateHeader) == 64);
static_assert(offsetof(WireBookUpdateHeader, instrument_packed) == 0);
static_assert(offsetof(WireBookUpdateHeader, seq) == 8);
static_assert(offsetof(WireBookUpdateHeader, prev_seq) == 16);
static_assert(offsetof(WireBookUpdateHeader, recv_ts_ns) == 24);
static_assert(offsetof(WireBookUpdateHeader, exch_ts_ns) == 32);
static_assert(offsetof(WireBookUpdateHeader, provider_mono_ns) == 40);
static_assert(offsetof(WireBookUpdateHeader, bid_count) == 48);
static_assert(offsetof(WireBookUpdateHeader, ask_count) == 52);
static_assert(offsetof(WireBookUpdateHeader, is_snapshot) == 56);
static_assert(sizeof(PriceLevel) == 16);
static_assert(offsetof(PriceLevel, price) == 0);
static_assert(offsetof(PriceLevel, qty) == 8);

// kBboQuote. Same "no venue field" reasoning as WireBookUpdateHeader above -
// this is the whole payload, no trailing level run.
struct WireBboQuotePayload {
    uint32_t instrument_packed;
    uint64_t seq;
    int64_t recv_ts_ns;
    int64_t exch_ts_ns;
    int64_t provider_mono_ns;  // see WireBookUpdateHeader's comment
    uint64_t bid_price;
    uint64_t bid_qty;
    uint64_t ask_price;
    uint64_t ask_qty;
};
static_assert(sizeof(WireBboQuotePayload) == 72);
static_assert(offsetof(WireBboQuotePayload, instrument_packed) == 0);
static_assert(offsetof(WireBboQuotePayload, seq) == 8);
static_assert(offsetof(WireBboQuotePayload, recv_ts_ns) == 16);
static_assert(offsetof(WireBboQuotePayload, exch_ts_ns) == 24);
static_assert(offsetof(WireBboQuotePayload, provider_mono_ns) == 32);
static_assert(offsetof(WireBboQuotePayload, bid_price) == 40);
static_assert(offsetof(WireBboQuotePayload, bid_qty) == 48);
static_assert(offsetof(WireBboQuotePayload, ask_price) == 56);
static_assert(offsetof(WireBboQuotePayload, ask_qty) == 64);

// kHealth (the provider-decided verdict, the "edge-triggered"
// message). Same "no venue field" reasoning as above - and no
// instrument_packed either, unlike the other three payloads: VenueHealthEvent
// (venue_health.h) itself carries no InstrumentKey, because a venue's
// connection health is a per-venue-per-stream verdict, not a per-instrument
// one, in today's one-process-per-(venue,instrument) world. If group
// size is ever raised so one connection multiplexes several instruments,
// VenueHealthEvent gains an InstrumentKey first and this payload follows it -
// this module mirrors the domain type, it does not extend it.
struct WireHealthPayload {
    uint8_t stream;  // StreamKind
    uint8_t health;  // VenueHealth
    uint8_t reserved[6];
    int64_t decided_mono_ns;
};
static_assert(sizeof(WireHealthPayload) == 16);
static_assert(offsetof(WireHealthPayload, stream) == 0);
static_assert(offsetof(WireHealthPayload, health) == 1);
static_assert(offsetof(WireHealthPayload, decided_mono_ns) == 8);

}  // namespace market_data::wire
