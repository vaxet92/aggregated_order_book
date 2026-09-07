#pragma once

// Encode/Decode pairs for each wire_format.h payload type. Pure functions:
// no socket, no allocation beyond growing the caller's reused buffer, no
// clock read - zero-copy on read, one copy into a caller-owned struct on
// decode. venue and instrument identity are deliberately not always encoded;
// see the per-function comments for which fields the caller must fill in
// from the connection itself.

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "md_core/types.h"
#include "md_core/venue_health.h"
#include "wire_format.h"

namespace market_data::wire {

// Encode/Decode for the four payload types. Pure functions: no socket,
// no allocation beyond growing the caller's buffer, no clock read. This is
// the layer describes as "zero-copy on read, one copy into a reused
// BookUpdate" - Encode appends to a buffer the caller keeps across calls
// (clear() keeps capacity), and Decode fills a struct the caller keeps across
// calls the same way, so steady state allocates nothing on either side once
// both have grown to their steady-state size.
//
// Reads just the common WireHeader. Every Decode* function below also reads
// it internally (a message must be self-describing to decode at all), so
// this exists for a caller that needs to dispatch on msg_type BEFORE picking
// which Decode* to call - i.e. every real caller once a socket is involved.
// Returns false if `buf` is shorter than a WireHeader.
bool DecodeHeader(std::span<const std::byte> buf, WireHeader& out);

// --- kHello ---
void EncodeHello(InstrumentKey instrument, std::string_view venue_name, std::vector<std::byte>& out);
// False on a short buffer or a venue_name that didn't fit at encode time
// (truncation is not silently accepted - see the .cpp for why).
bool DecodeHello(std::span<const std::byte> buf, InstrumentKey& instrument, std::string& venue_name);

// --- kGoodbye ---
void EncodeGoodbye(GoodbyeReason reason, std::vector<std::byte>& out);
bool DecodeGoodbye(std::span<const std::byte> buf, GoodbyeReason& reason);

// --- kBookUpdate ---
// `update.venue` is not encoded (see file comment). Every other field -
// instrument, seq, prev_seq, both wall-clock stamps, recv_mono_ns (as
// provider_mono_ns on the wire), is_snapshot, and the full bid/ask level
// vectors - round-trips exactly.
void EncodeBookUpdate(const BookUpdate& update, std::vector<std::byte>& out);
// `out` is reused: bids/asks are cleared (keeping capacity) then repopulated,
// matching the "vectors are cleared but keep their capacity, so steady
// state allocates nothing." `out.venue` is left untouched - the caller sets
// it from the connection's bound slot.
bool DecodeBookUpdate(std::span<const std::byte> buf, BookUpdate& out);

// --- kBboQuote ---
void EncodeBboQuote(const BboQuote& quote, std::vector<std::byte>& out);
bool DecodeBboQuote(std::span<const std::byte> buf, BboQuote& out);

// --- kHealth ---
// No InstrumentKey and no VenueId are encoded (see WireHealthPayload's
// comment in wire_format.h and the file comment above) - decode leaves
// `out.venue` untouched, same convention as BookUpdate/BboQuote.
void EncodeHealth(const VenueHealthEvent& event, std::vector<std::byte>& out);
bool DecodeHealth(std::span<const std::byte> buf, VenueHealthEvent& out);

}  // namespace market_data::wire
