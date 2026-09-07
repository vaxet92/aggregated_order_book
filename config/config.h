#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <array>

#include "types/venue.h"
#include "types/instrument_registry.h"

// ---------------------------------------------------------------------------
// REMINDER: venue depth tiers (verified against the live APIs)
//
// Depth is NOT a free parameter - each venue publishes only at fixed tiers,
// and they are chosen in different places per venue. Asking for 300 levels
// means taking the smallest tier that covers it, which differs per venue.
//
//   Binance  REST /api/v3/depth?limit=   5, 10, 20, 50, 100, 500, 1000, 5000
//   Binance  WS {symbol}@depth@{updateSpeed}  500
//   Bybit    WS topic orderbook.N        1, 50, 200, 1000
//   OKX      WS channel                  1 (bbo-tbt), 5 (books5), 400 (books)
//
// OKX effectively has ONE usable tier. There is nothing between 5 and
// 400, and nothing above 400 without VIP4 (books-l2-tbt / books50-l2-tbt
// return error 64003 otherwise). So depth=800 gives Binance 1000, Bybit 1000,
// and OKX still 400 - a venue cannot always honour the request, and that is
// worth logging rather than silently under-delivering.
//
// Currently hardcoded in the providers, NOT driven by this config:
//   Binance limit=1000, Bybit orderbook.50, OKX books (400).
// ---------------------------------------------------------------------------
inline constexpr std::array<uint32_t, 8> kBinanceDepthTiers = {5, 10, 20, 50, 100, 500, 1000, 5000};
inline constexpr std::array<uint32_t, 4> kBybitDepthTiers = {1, 50, 200, 1000};
inline constexpr std::array<uint32_t, 3> kOkxDepthTiers = {1, 5, 400};

// Smallest tier that covers `desired`, or the deepest tier when none does.
// Rounds UP so the requested depth is a floor, never silently shallower than
// asked - except when the venue simply cannot go that deep, which the caller
// should log rather than hide.
template <size_t N>
constexpr uint32_t SelectDepthTier(const std::array<uint32_t, N>& tiers, uint32_t desired) {
    for (uint32_t tier : tiers) {
        if (tier >= desired) {
            return tier;
        }
    }
    return tiers.back();  // deepest available - request exceeds what this venue publishes
}

// ---------------------------------------------------------------------------
// REMINDER: venue connection limits are NOT verified.
//
// Redundant connections multiply socket count: connections x venues, since
// every venue's depth and BBO subscriptions now share one combined
// connection (Binance, Bybit, OKX all do - see md_provider.h's
// GetCombinedPath()/CombinedSubscriptionMessage()). At the default that is
// 1 x 3 = 3 sockets; at "connections": 3 it is 9, all from one IP.
//
// Every venue caps connections per IP, and OKX additionally rate-limits
// connection ATTEMPTS - which the reconnect loop hits, not just startup.
// Those limits have not been checked against the documentation.
//
// exceeding a venue's limit does not degrade gracefully. The venue
// refuses or bans the IP, which takes down every connection to it at once -
// the exact failure the redundancy was added to prevent.
// ---------------------------------------------------------------------------

// Default 1: redundancy is OPT-IN. At the default, each venue gets exactly
// one socket, not multiplied by any redundancy - so there is no dedup work
// and no risk of hitting an unverified venue limit. An operator who wants
// failover asks for it explicitly with "connections" in
// server_config_<market>.json - there is no CLI override for it (see
// CliOverrides below).
inline constexpr uint32_t kDefaultConnections = 1;

// Upper bound on "connections". This is OUR arbitrary safety limit, not a
// number from any venue's documentation. It exists so a typo like
// "connections": 300 cannot open 1800 sockets and get the IP banned.
inline constexpr uint32_t kMaxConnections = 8;

// The session config is one file per market, in the user_config directory:
// server_config_spot.json / server_config_futures.json. There is no --config=
// flag - the market (a required --market= on the core binaries, a positional
// arg on md_provider_app) is the only thing that selects the file, so there is
// no second place an operator could point it and disagree about which ran.
inline std::string ConfigFileForMarket(MarketType market) {
    return market == MarketType::kSpot ? "user_config/server_config_spot.json"
                                       : "user_config/server_config_futures.json";
}

// One "instruments[]" entry from the config file, after its symbol has been
// registered.
//
// `markets` is a list, not a single MarketType, because one config entry can
// name several: {"symbol": "BTCUSDT", "market": ["spot", "futures"]}. The
// symbol is registered ONCE - the registry does not know about markets, only
// about names (types/instrument_registry.h) - and this struct is what turns
// that one id back into N independent InstrumentKeys, one per market. Spot and
// futures never share a key, so they can never meet in a merge (venue.h).
struct InstrumentEntry {
    InstrumentId id;
    std::string symbol;  // canonical form, as returned by InstrumentRegistry::Name
    std::vector<MarketType> markets;
};

// Forward-declared only: ServerConfig::ParseJson/LoadFile return
// ConfigLoadResult by value below, which just needs the NAME here - the
// definition, which needs ServerConfig complete, comes after ServerConfig
// itself.
struct ConfigLoadResult;

struct ServerConfig {
    std::vector<VenueId> venues;
    std::vector<InstrumentEntry> instruments;
    int grpc_port = 50051;

    // Desired per-venue book depth. Each venue rounds UP to its nearest tier
    // above, falling back to its deepest tier when the request exceeds what
    // it publishes (see the reminder above).
    uint32_t depth = 500;

    // Redundant WebSocket connections per stream, per venue - "line
    // arbitration". Every connection carries the same messages; the first
    // copy to arrive wins and the rest are dropped by the dedup filter.
    //
    // The benefit is FAILOVER: one socket dying leaves connections-1 still
    // delivering, so there is no gap, no resync and no REST refetch.
    //
    // A latency benefit is NOT claimed. Real line arbitration wins because
    // the feeds travel independent physical paths; these share one NIC and
    // one route, so any gain is jitter, and it is not measured.
    //
    // Cost is linear: N sockets, N x bandwidth, N x TLS decrypt per stream.
    // Duplicates are dropped before parsing where possible, so the parse
    // cost does not scale with N.
    uint32_t connections = kDefaultConnections;

    // Parses a config document already in memory. No file access here - the
    // caller reads the file (or, in a test, supplies a literal string), which
    // is what makes this testable without touching a filesystem.
    //
    // `registry` is populated as a side effect: every instruments[].symbol is
    // registered, so every InstrumentEntry::id it returns is already valid to
    // look up. Passed by reference, not returned, because InstrumentRegistry
    // holds a std::atomic and cannot be copied or moved out of this call.
    static ConfigLoadResult ParseJson(std::string_view json, InstrumentRegistry& registry);

    // Reads `path` and calls ParseJson. A missing or unreadable file reports
    // through the same ConfigLoadResult::error as a malformed one - the
    // operator sees "config:" and the reason either way, never a crash.
    static ConfigLoadResult LoadFile(const std::string& path, InstrumentRegistry& registry);

    // Validate configuration
    bool Validate() const;
};

// What a config load produced, success or not.
//
// `config` is meaningless when `error` is non-empty - callers must check
// Ok() before reading it. A pair of out-params (bool + ServerConfig&) would
// let a caller forget the check; returning them bundled does not.
struct ConfigLoadResult {
    ServerConfig config;
    std::string error;  // empty means success

    bool Ok() const { return error.empty(); }
};

// Command-line flags, now overrides ONLY. venues and instruments come from
// the per-market session file exclusively - there is no --venues=/--instruments=
// and no --config= either, so there is no second place that can define the
// active symbol set, or point at a different file, and disagree with it.
//
// connections is NOT here, deliberately - unlike depth and grpc_port, it has
// no CLI override at all. It is set only via "connections" in
// server_config_<market>.json, so there is exactly one place that decides how
// many redundant sockets to open, not two that could disagree.
struct CliOverrides {
    // std::optional, not a value with a sentinel: "not passed on the command
    // line" and "passed as zero" must stay distinguishable, or a caller could
    // never tell whether to keep the file's value or overwrite it with zero.
    std::optional<uint32_t> depth;
    std::optional<int> grpc_port;

    static CliOverrides ParseFromArgs(int argc, char* argv[]);
};

// Reads a required --market=spot|futures from argv. nullopt means it was
// missing or unrecognised - the caller logs and refuses to start, because the
// market selects the config file (ConfigFileForMarket) and there is no
// default. Separate from CliOverrides: md_core_app does not use that struct,
// and this is a hard requirement rather than an override of a file value.
std::optional<MarketType> ParseMarketFlag(int argc, char* argv[]);
