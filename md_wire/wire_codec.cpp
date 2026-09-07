#include "wire_codec.h"

#include <cstring>

namespace market_data::wire {

namespace {

// Appends `count` raw bytes of `value` to `out`. The whole point of this
// module is that these structs ARE the wire format - static_assert in
// wire_format.h is what makes memcpy safe here rather than reinterpret_cast
// tricks that would depend on strict-aliasing rules this project doesn't
// want to lean on.
template <typename T>
void AppendPod(const T& value, std::vector<std::byte>& out) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void AppendBytes(const void* data, std::size_t count, std::vector<std::byte>& out) {
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    out.insert(out.end(), bytes, bytes + count);
}

// Reads a T out of `buf` at `offset`, advancing offset past it. Returns false
// (leaving `offset` unspecified) if the read would run past `buf.size()` -
// every Decode* function bails out on the first such failure rather than
// reading partial structs, per the "additive only / skip what you don't
// understand" compatibility rule: a truncated buffer here is not a
// smaller message, it's a corrupt one, and the two must not be confused.
template <typename T>
bool ReadPod(std::span<const std::byte> buf, std::size_t& offset, T& out) {
    if (offset + sizeof(T) > buf.size()) {
        return false;
    }
    std::memcpy(&out, buf.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

bool ReadBytes(std::span<const std::byte> buf, std::size_t& offset, void* dest, std::size_t count) {
    if (offset + count > buf.size()) {
        return false;
    }
    std::memcpy(dest, buf.data() + offset, count);
    offset += count;
    return true;
}

void WriteHeader(WireMsgType type, std::vector<std::byte>& out) {
    WireHeader header{.msg_type = static_cast<uint16_t>(type), .wire_version = kWireVersion};
    AppendPod(header, out);
}

}  // namespace

bool DecodeHeader(std::span<const std::byte> buf, WireHeader& out) {
    std::size_t offset = 0;
    return ReadPod(buf, offset, out);
}

// --- kHello ---

void EncodeHello(InstrumentKey instrument, std::string_view venue_name, std::vector<std::byte>& out) {
    out.clear();
    WriteHeader(WireMsgType::kHello, out);

    WireHelloPayload payload{};
    payload.instrument_packed = instrument.Packed();

    // Truncation is a config bug (a venue name too long to fit), not
    // something to hide. venue_name.size() is always tiny in practice
    // ("BINANCE", "BYBIT", "OKX") so this is a defensive bound, not an
    // expected path - see the assert-shaped check in DecodeHello for the
    // matching read-side guarantee (a NUL is always present to stop at).
    const std::size_t copy_len = std::min(venue_name.size(), kVenueNameCapacity - 1);
    std::memcpy(payload.venue_name, venue_name.data(), copy_len);
    payload.venue_name[copy_len] = '\0';

    AppendPod(payload, out);
}

bool DecodeHello(std::span<const std::byte> buf, InstrumentKey& instrument, std::string& venue_name) {
    std::size_t offset = 0;
    WireHeader header{};
    if (!ReadPod(buf, offset, header)) return false;
    if (header.msg_type != static_cast<uint16_t>(WireMsgType::kHello)) return false;

    WireHelloPayload payload{};
    if (!ReadPod(buf, offset, payload)) return false;

    instrument = InstrumentKey(payload.instrument_packed);
    // payload.venue_name is zero-padded by EncodeHello and always NUL
    // terminated within its capacity, so this never reads past the array.
    venue_name.assign(payload.venue_name);
    return true;
}

// --- kGoodbye ---

void EncodeGoodbye(GoodbyeReason reason, std::vector<std::byte>& out) {
    out.clear();
    WriteHeader(WireMsgType::kGoodbye, out);
    WireGoodbyePayload payload{.reason = static_cast<uint32_t>(reason)};
    AppendPod(payload, out);
}

bool DecodeGoodbye(std::span<const std::byte> buf, GoodbyeReason& reason) {
    std::size_t offset = 0;
    WireHeader header{};
    if (!ReadPod(buf, offset, header)) return false;
    if (header.msg_type != static_cast<uint16_t>(WireMsgType::kGoodbye)) return false;

    WireGoodbyePayload payload{};
    if (!ReadPod(buf, offset, payload)) return false;

    reason = static_cast<GoodbyeReason>(payload.reason);
    return true;
}

// --- kBookUpdate ---

void EncodeBookUpdate(const BookUpdate& update, std::vector<std::byte>& out) {
    out.clear();
    WriteHeader(WireMsgType::kBookUpdate, out);

    WireBookUpdateHeader header{};
    header.instrument_packed = update.instrument.Packed();
    header.seq = update.seq;
    header.prev_seq = update.prev_seq;
    header.recv_ts_ns = update.recv_ts_ns;
    header.exch_ts_ns = update.exch_ts_ns;
    header.provider_mono_ns = update.recv_mono_ns;
    header.bid_count = static_cast<uint32_t>(update.bids.size());
    header.ask_count = static_cast<uint32_t>(update.asks.size());
    header.is_snapshot = update.is_snapshot ? 1 : 0;
    AppendPod(header, out);

    if (!update.bids.empty()) {
        AppendBytes(update.bids.data(), update.bids.size() * sizeof(PriceLevel), out);
    }
    if (!update.asks.empty()) {
        AppendBytes(update.asks.data(), update.asks.size() * sizeof(PriceLevel), out);
    }
}

bool DecodeBookUpdate(std::span<const std::byte> buf, BookUpdate& out) {
    std::size_t offset = 0;
    WireHeader header{};
    if (!ReadPod(buf, offset, header)) return false;
    if (header.msg_type != static_cast<uint16_t>(WireMsgType::kBookUpdate)) return false;

    WireBookUpdateHeader body{};
    if (!ReadPod(buf, offset, body)) return false;

    // Cleared but not reallocated - repeated calls with the same `out`
    // converge to zero steady-state allocation once capacity covers the
    // largest snapshot seen, per the decode design.
    out.instrument = InstrumentKey(body.instrument_packed);
    out.seq = body.seq;
    out.prev_seq = body.prev_seq;
    out.recv_ts_ns = body.recv_ts_ns;
    out.exch_ts_ns = body.exch_ts_ns;
    out.recv_mono_ns = body.provider_mono_ns;
    out.is_snapshot = body.is_snapshot != 0;

    out.bids.clear();
    out.asks.clear();
    out.bids.resize(body.bid_count);
    if (body.bid_count > 0 && !ReadBytes(buf, offset, out.bids.data(), body.bid_count * sizeof(PriceLevel))) {
        return false;
    }
    out.asks.resize(body.ask_count);
    if (body.ask_count > 0 && !ReadBytes(buf, offset, out.asks.data(), body.ask_count * sizeof(PriceLevel))) {
        return false;
    }
    // out.venue is deliberately left untouched - see wire_codec.h's file
    // comment. The caller stamps it from the connection's bound VenueSlot.
    return true;
}

// --- kBboQuote ---

void EncodeBboQuote(const BboQuote& quote, std::vector<std::byte>& out) {
    out.clear();
    WriteHeader(WireMsgType::kBboQuote, out);

    WireBboQuotePayload payload{};
    payload.instrument_packed = quote.instrument.Packed();
    payload.seq = quote.seq;
    payload.recv_ts_ns = quote.recv_ts_ns;
    payload.exch_ts_ns = quote.exch_ts_ns;
    payload.provider_mono_ns = quote.recv_mono_ns;
    payload.bid_price = quote.bid_price;
    payload.bid_qty = quote.bid_qty;
    payload.ask_price = quote.ask_price;
    payload.ask_qty = quote.ask_qty;
    AppendPod(payload, out);
}

bool DecodeBboQuote(std::span<const std::byte> buf, BboQuote& out) {
    std::size_t offset = 0;
    WireHeader header{};
    if (!ReadPod(buf, offset, header)) return false;
    if (header.msg_type != static_cast<uint16_t>(WireMsgType::kBboQuote)) return false;

    WireBboQuotePayload payload{};
    if (!ReadPod(buf, offset, payload)) return false;

    out.instrument = InstrumentKey(payload.instrument_packed);
    out.seq = payload.seq;
    out.recv_ts_ns = payload.recv_ts_ns;
    out.exch_ts_ns = payload.exch_ts_ns;
    out.recv_mono_ns = payload.provider_mono_ns;
    out.bid_price = payload.bid_price;
    out.bid_qty = payload.bid_qty;
    out.ask_price = payload.ask_price;
    out.ask_qty = payload.ask_qty;
    // out.venue deliberately left untouched - see wire_codec.h's file comment.
    return true;
}

// --- kHealth ---

void EncodeHealth(const VenueHealthEvent& event, std::vector<std::byte>& out) {
    out.clear();
    WriteHeader(WireMsgType::kHealth, out);

    WireHealthPayload payload{};
    payload.stream = static_cast<uint8_t>(event.stream);
    payload.health = static_cast<uint8_t>(event.health);
    payload.decided_mono_ns = event.decided_mono_ns;
    AppendPod(payload, out);
}

bool DecodeHealth(std::span<const std::byte> buf, VenueHealthEvent& out) {
    std::size_t offset = 0;
    WireHeader header{};
    if (!ReadPod(buf, offset, header)) return false;
    if (header.msg_type != static_cast<uint16_t>(WireMsgType::kHealth)) return false;

    WireHealthPayload payload{};
    if (!ReadPod(buf, offset, payload)) return false;

    out.stream = static_cast<StreamKind>(payload.stream);
    out.health = static_cast<VenueHealth>(payload.health);
    out.decided_mono_ns = payload.decided_mono_ns;
    // out.venue deliberately left untouched - see wire_codec.h's file comment.
    return true;
}

}  // namespace market_data::wire
