#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "aggregator.grpc.pb.h"

namespace market_data {

// ---------------------------------------------------------------- config ---

struct ClientConfig {
    std::string server_address = "localhost:50051";  // Compose: "aggregator:50051"
    std::string symbol = "BTCUSDT";

    // REQUIRED, and deliberately WITHOUT a default. Spot and futures are two
    // separate subscriptions - (symbol, market) selects the book - so there is
    // no value this could sensibly fall back to.
    //
    // optional<> rather than a plain wire::MarketType, so "not given"
    // stays distinguishable from "chose spot". On the wire that distinction is
    // impossible - a proto3 enum has no presence and MARKET_UNSPECIFIED is
    // just zero - which is precisely why the server rejects it and why the
    // check has to happen HERE, before the request is built.
    //
    // wire::MarketType, not the domain MarketType: a client speaks the wire
    // protocol and has no domain layer. Holding the domain type would mean
    // linking wire_translation, and with it md_core, into every client binary
    // to map a two-value enum.
    std::optional<wire::MarketType> market;

    // The want_* flags are deliberately separate from the threshold vectors:
    // an empty vector alone cannot distinguish "subscribed, use server
    // defaults" from "not subscribed at all". That is the same ambiguity the
    // proto avoids with optional sub-messages, mirrored here.
    bool want_bbo = false;
    bool want_volume_bands = false;
    bool want_price_bands = false;
    std::vector<uint64_t> notional_bands;  // already scaled x 1e8; empty = server defaults
    std::vector<uint32_t> bps_bands;       // empty = server defaults

    // Flags, all optional:
    //   --server=host:port     --symbol=BTCUSDT
    //   --market=spot|futures                 REQUIRED
    //   --bbo
    //   --volume_bands                        (subscribe, server defaults)
    //   --notional_band=1,100K,1M,50M         (subscribe, these thresholds)
    //   --price_bands                         (subscribe, server defaults)
    //   --price_band=50,100,200,500,1000      (subscribe, these thresholds)
    //
    // Notional values accept K/M suffixes: "1" is one dollar, "100K" is
    // 100,000, "1M" is a million. Without suffixes the small values that are
    // useful for verification would be unwriteable - a $1 sweep should fill
    // part of one level, with vwap equal to the best price and zero slippage.
    static ClientConfig ParseFromArgs(int argc, char* argv[]);

    // Turns this config into the wire request, applying the presence rules.
    wire::SubscribeRequest ToRequest() const;

    // False when no feed was requested - the server would reject it, so the
    // caller can fail early with a usage message instead.
    bool HasAnyFeed() const { return want_bbo || want_volume_bands || want_price_bands; }

    // Same early-failure idea as HasAnyFeed: without a market the server
    // answers INVALID_ARGUMENT after a full connect/subscribe round trip, and
    // a usage message is a better answer than a gRPC status string.
    bool HasMarket() const { return market.has_value(); }
};

void PrintUsage(const char* program_name);

// ------------------------------------------------------------ formatting ---

// Pure integer formatting - no floating point, even at display time.
// 7831010000000 at scale=8 becomes "78310.10000000".
//
// `decimals` truncates the fractional part for readability - 8 decimals on a
// $78,000 price is 10 significant figures, and consecutive updates differ in
// the 6th, which nobody reads. Truncates rather than rounds: rounding a price
// up could imply a level that does not exist.
std::string FormatScaled(uint64_t value, uint32_t scale, uint32_t decimals);
std::string FormatScaled(uint64_t value, uint32_t scale);

// Large notionals in human units: 100000000000000 at scale=8 -> "1.0M".
std::string FormatCompact(uint64_t value, uint32_t scale);

// Per-venue attribution - e.g. "BINANCE:0.00531,OKX:0.00200"
std::string FormatVenues(const wire::ConsolidatedPriceLevel& level, uint32_t qty_scale);

// Slippage of `vwap` against `reference`, in basis points. Direction-aware:
// a bid sweep fills BELOW the best bid, an ask sweep ABOVE the best ask, so
// both come out positive and directly comparable. "-" when there is no
// reference to measure against.
std::string FormatSlippageBps(uint64_t vwap, uint64_t reference, bool is_bid);

// ---------------------------------------------------------------- output ---

// One printer per payload type, shared by client_app and the single-feed
// test binaries so formatting never drifts between them.
void PrintBbo(const wire::Update& update);
void PrintVolumeBands(const wire::Update& update);
void PrintPriceBands(const wire::Update& update);

// Dispatches on payload_case - what client_app uses, since one subscription
// may carry several feeds.
void PrintUpdate(const wire::Update& update);

// ---------------------------------------------------------- subscription ---

// Connects, subscribes, and pumps updates into `on_update` until the stream
// ends, reconnecting with exponential backoff.
//
// Gap detection happens HERE, once, for every client. It was
// previously duplicated per binary, which is how a seq-underflow bug ended
// up fixed in only one copy - the exact drift this extraction prevents.
//
// Returns non-zero only when the subscription is rejected in a way retrying
// cannot fix (UNIMPLEMENTED / INVALID_ARGUMENT). Otherwise it never returns.
int RunSubscription(const ClientConfig& config, const char* client_name, const wire::SubscribeRequest& request,
                    const std::function<void(const wire::Update&)>& on_update);

}  // namespace market_data
