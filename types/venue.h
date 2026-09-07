#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class InstrumentId : uint16_t {
    BTCUSDT = 0,
    ETHUSDT = 1,
    SOLUSDT = 2,
};

// Spot and futures are DIFFERENT MARKETS that happen to share a symbol. The
// same "BTCUSDT" trades at different prices with different depth on each, and
// consolidating across them would produce a book no venue will honour
enum class MarketType : uint8_t {
    kSpot = 0,
    kFutures = 1,
};

inline constexpr std::string_view ToMarketString(MarketType market) {
    return market == MarketType::kSpot ? "spot" : "futures";
}

// nullopt, not a sentinel enumerator - same reasoning as ToInstrumentId below:
// there is no "unknown market" in the domain, only a config string that named
// one incorrectly. The config loader rejects it at the boundary.
inline constexpr std::optional<MarketType> ToMarketType(std::string_view market) {
    if (market == "spot") {
        return MarketType::kSpot;
    } else if (market == "futures") {
        return MarketType::kFutures;
    }
    return std::nullopt;
}

// Bit layout of InstrumentKey::packed_:
//
//   MSB                                      LSB
//   +------------------------------+----------+
//   |          Symbol ID           |  Market  |
//   |           24 bits            |  8 bits  |
//   +------------------------------+----------+
//
// InstrumentId is uint16_t, so it uses 16 of the 24 symbol bits - room for
// 65k instruments without touching the layout.
inline constexpr uint32_t kMarketMask = 0xFF;
inline constexpr uint32_t kSymbolShift = 8;

// What Core stores books BY. Market type belongs here, on the instrument -
// NOT on the venue ("the shard key is (symbol, market_type) - venue is
// not part of it").
//
// this is what makes mixing spot and futures IMPOSSIBLE rather than
// merely forbidden. MergeBooks merges across venue slots within ONE key, so
// two different keys land in two different MapOrderBookArrays and can never meet.
// Had MarketType gone on the venue instead, both would sit in the same merge
// and correctness would depend on a filter someone has to remember to write.
//
// stored as one packed uint32_t rather than two fields, so the hash is
// the identity function and equality is a single integer compare. There are
// no hash collisions between distinct keys by construction - the two fields
// occupy disjoint bits. `operator==` is still required, because unordered_map
// reduces hash -> bucket with modulo and distinct hashes still share buckets.
class InstrumentKey {
   public:
    // Needed only so BookUpdate (and therefore ProviderMessage, and therefore
    // the SPSC ring's slot array) is default-constructible. A default key is
    // never read: a ring slot is only consumed after a push wrote real data.
    //
    // An invalid instrument never reaches this type - a client request naming
    // one is rejected at the boundary, which is why there is no UNKNOWN case
    // to encode here.
    InstrumentKey() noexcept = default;

    constexpr InstrumentKey(InstrumentId symbol, MarketType market) noexcept
        : packed_((static_cast<uint32_t>(static_cast<uint16_t>(symbol)) << kSymbolShift) |
                  static_cast<uint32_t>(market)) {}

    explicit constexpr InstrumentKey(uint32_t packed) noexcept : packed_(packed) {}

    constexpr InstrumentId Symbol() const noexcept {
        return static_cast<InstrumentId>(static_cast<uint16_t>(packed_ >> kSymbolShift));
    }

    constexpr MarketType Market() const noexcept { return static_cast<MarketType>(packed_ & kMarketMask); }

    constexpr uint32_t Packed() const noexcept { return packed_; }

    constexpr bool operator==(const InstrumentKey&) const noexcept = default;

   private:
    // Initialised here so the defaulted constructor above is valid and
    // constexpr-usable; without it the compiler cannot default-construct at
    // compile time.
    uint32_t packed_ = 0;
};

constexpr InstrumentKey MakeKey(InstrumentId symbol, MarketType market) noexcept {
    return InstrumentKey{symbol, market};
}

// Identity hash: the key IS its hash, and distinct keys have distinct packed
// values, so this is perfect for this domain and cheaper than any mixing
// function.
struct InstrumentKeyHash {
    size_t operator()(InstrumentKey key) const noexcept { return key.Packed(); }
};

enum class VenueId : uint16_t {
    BINANCE = 0,
    BYBIT = 1,
    OKX = 2,
    COUNT = 3,
};

constexpr size_t kVenueCount = static_cast<size_t>(VenueId::COUNT);

inline constexpr std::array<VenueId, kVenueCount> VenueIdArray{
    VenueId::BINANCE,
    VenueId::BYBIT,
    VenueId::OKX,
};

// ENDPOINTS (hosts, ports, WS and REST paths) are no longer here. They are
// data now, loaded per (venue, market) from venues_config.json - see
// config/venues_config.h, which also carries the reasoning that used to live
// in this block: why every venue is on 443 rather than the 9443/8443 their
// docs lead with, why OKX's 443 is only partly verified, and why Binance is
// the only venue that needs REST at all.
//
// they moved because a path could not depend on the market. kByBitPath
// was the literal string "/v5/public/spot", so a Bybit FUTURES provider had no
// way to reach /v5/public/linear without changing the type of the constant.

// Staleness backstops, per venue AND per stream.
//
// two of these are DERIVED from behaviour the venue documents about
// itself; the rest are placeholders and are labelled as such. "Bybit L1 must
// speak every 3 seconds because Bybit says it does" is defensible in a
// debrief. "30 seconds felt about right" is not - so where a number is a
// guess, it says so rather than pretending otherwise.
//
// too SHORT is the dangerous direction. A backstop below a venue's real
// quiet interval marks a healthy feed stale and flaps it in and out of the
// merge, which is worse than having no watchdog at all. Every placeholder
// below is therefore generous. These are BACKSTOPS, not detectors - fast
// detection is meant to come from connection state and, once built,
// cross-venue corroboration.
namespace staleness {

inline constexpr int64_t kSecondNs = 1'000'000'000;

// DERIVED: Bybit republishes L1 (orderbook.1, our BBO stream) with the same
// `u` after 3s of no book change. Silence past ~3x that is the venue breaking
// a promise it makes about itself, not a quiet market.
inline constexpr int64_t kBybitBboBackstopNs = 10 * kSecondNs;

// PLACEHOLDER: the 3s republish is documented for L1 only. orderbook.50 has
// no documented keepalive, so this is a guess pending measurement.
inline constexpr int64_t kBybitDepthBackstopNs = 30 * kSecondNs;

// DERIVED: OKX sends seqId == prevSeqId with empty sides after roughly 60s of
// no change on the `books` channel. 90s leaves margin for a missed keepalive.
//
// Far too slow to be a detector on its own - a dead OKX depth feed would go
// unnoticed for up to 90s on this signal alone. Connection state catches the
// common case immediately; cross-venue comparison is what will close the rest.
inline constexpr int64_t kOkxDepthBackstopNs = 90 * kSecondNs;

// PLACEHOLDER: whether bbo-tbt has its own keepalive is unverified.
inline constexpr int64_t kOkxBboBackstopNs = 30 * kSecondNs;

// PLACEHOLDER, and the weakest case. Binance publishes NO keepalive on either
// stream - it sends nothing when nothing changes, so silence carries no
// information at all and a quiet market is indistinguishable from a dead feed.
// These numbers cannot be derived from anything, only measured. Until
// cross-venue corroboration exists, connection state is the only fast signal
// Binance gives us.
inline constexpr int64_t kBinanceDepthBackstopNs = 30 * kSecondNs;
inline constexpr int64_t kBinanceBboBackstopNs = 30 * kSecondNs;

}  // namespace staleness

class VenueConverter {
   public:
    VenueConverter() = delete;

    static inline std::string ToVenueString(VenueId venue_id) {
        switch (venue_id) {
            case VenueId::BINANCE:
                return "BINANCE";
            case VenueId::BYBIT:
                return "BYBIT";
            case VenueId::OKX:
                return "OKX";
            default:
                return "UNKNOWN";
        }
    }

    static inline VenueId ToVenueId(const std::string& venue_id) {
        if (venue_id == "BINANCE") {
            return VenueId::BINANCE;
        } else if (venue_id == "BYBIT") {
            return VenueId::BYBIT;
        } else if (venue_id == "OKX") {
            return VenueId::OKX;
        }
        return VenueId::COUNT;
    }

    static inline std::string ToInstrumentString(InstrumentId instrument_id) {
        switch (instrument_id) {
            case InstrumentId::BTCUSDT:
                return "BTCUSDT";
            case InstrumentId::ETHUSDT:
                return "ETHUSDT";
            case InstrumentId::SOLUSDT:
                return "SOLUSDT";
            default:
                return "UNKNOWN";
        }
    }

    // Symbol AND market, e.g. "BTCUSDT:spot". Logs that print only the
    // symbol cannot tell the two markets apart, which is exactly the confusion
    // keeping them in separate books exists to prevent.
    static inline std::string ToInstrumentString(InstrumentKey key) {
        return ToInstrumentString(key.Symbol()) + ":" + std::string(ToMarketString(key.Market()));
    }

    // nullopt, not a sentinel enumerator: there is no such thing as an
    // "unknown instrument" in the domain, only a string that failed to name
    // one. Callers reject it at the boundary (aggregator_service::Subscribe),
    // so an invalid instrument never reaches an InstrumentKey or a book.
    static inline std::optional<InstrumentId> ToInstrumentId(const std::string& instrument) {
        if (instrument == "BTCUSDT") {
            return InstrumentId::BTCUSDT;
        } else if (instrument == "ETHUSDT") {
            return InstrumentId::ETHUSDT;
        } else if (instrument == "SOLUSDT") {
            return InstrumentId::SOLUSDT;
        }
        return std::nullopt;
    }

    // Venue AND market, e.g. "BINANCE:FUTURES".
    //
    // constexpr, returning a string_view into a string LITERAL - static
    // storage, so returning the view is safe and nothing allocates per log call.
    // ToVenueString above returns std::string and allocates on every call; these
    // tags sit on paths that can fire per gap, so the cheap form is worth having.
    static constexpr std::string_view ToVenueMarketString(VenueId venue, MarketType market) {
        switch (venue) {
            case VenueId::BINANCE:
                return market == MarketType::kSpot ? "BINANCE:SPOT" : "BINANCE:FUTURES";
            case VenueId::BYBIT:
                return market == MarketType::kSpot ? "BYBIT:SPOT" : "BYBIT:FUTURES";
            case VenueId::OKX:
                return market == MarketType::kSpot ? "OKX:SPOT" : "OKX:FUTURES";
            default:
                return market == MarketType::kSpot ? "UNKNOWN:SPOT" : "UNKNOWN:FUTURES";
        }
    }

    static constexpr std::string_view ToVenueMarketString(VenueId venue, InstrumentKey key) {
        return ToVenueMarketString(venue, key.Market());
    }
};
