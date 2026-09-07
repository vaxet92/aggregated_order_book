#include "client_common.h"

#include "logger/logger.h"

#include <grpcpp/grpcpp.h>
#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace market_data {

namespace {

constexpr int kMaxBackoffSeconds = 8;

// The wire's fixed scale, documented as a protocol constant in
// aggregator.proto. Hardcoded here rather than shared from md_core: the
// client is a separate service that knows the protocol, not the server's
// internals.
constexpr uint64_t kWireScaleFactor = 100'000'000;

constexpr uint32_t kPriceDecimals = 2;  // BTCUSDT ticks in cents
constexpr uint32_t kQtyDecimals = 4;

uint64_t Pow10(uint32_t exponent) {
    uint64_t result = 1;
    for (uint32_t i = 0; i < exponent; ++i) {
        result *= 10;
    }
    return result;
}

// "1" -> 1, "100K" -> 100000, "1M" -> 1000000, then scaled to wire units.
// Returns nullopt on anything unparseable so the caller can reject the flag
// rather than silently subscribing to a threshold of zero.
std::optional<uint64_t> ParseNotional(std::string_view token) {
    if (token.empty()) {
        return std::nullopt;
    }
    uint64_t multiplier = 1;
    const char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(token.back())));
    if (suffix == 'K') {
        multiplier = 1'000;
        token.remove_suffix(1);
    } else if (suffix == 'M') {
        multiplier = 1'000'000;
        token.remove_suffix(1);
    }
    if (token.empty()) {
        return std::nullopt;
    }

    uint64_t whole = 0;
    for (char c : token) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        whole = whole * 10 + static_cast<uint64_t>(c - '0');
    }
    return whole * multiplier * kWireScaleFactor;
}

// Splits "a,b,c" on commas. Empty tokens are skipped rather than treated as
// zero.
std::vector<std::string> SplitCsv(std::string_view text) {
    std::vector<std::string> parts;
    while (!text.empty()) {
        const size_t comma = text.find(',');
        std::string_view token = text.substr(0, comma);
        if (!token.empty()) {
            parts.emplace_back(token);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return parts;
}

// Returns the value of "--name=value", or nullopt if `arg` is a different flag.
std::optional<std::string_view> FlagValue(std::string_view arg, std::string_view name) {
    if (arg.size() <= name.size() + 1 || arg.substr(0, name.size()) != name || arg[name.size()] != '=') {
        return std::nullopt;
    }
    return arg.substr(name.size() + 1);
}

}  // namespace

// ---------------------------------------------------------------- config ---

void PrintUsage(const char* program_name) {
    fmt::print(stderr,
               "usage: {} [flags]\n"
               "\n"
               "  --server=host:port              default localhost:50051\n"
               "  --symbol=BTCUSDT                default BTCUSDT\n"
               "  --market=spot|futures           REQUIRED, no default\n"
               "\n"
               "  --bbo                           subscribe to consolidated BBO\n"
               "  --volume_bands                  subscribe to volume bands, server defaults\n"
               "  --notional_band=1,100K,1M,50M   subscribe to volume bands, these thresholds\n"
               "  --price_bands                   subscribe to price bands, server defaults\n"
               "  --price_band=50,100,200,500     subscribe to price bands, these bps\n"
               "\n"
               "--market and at least one feed are required. Spot and futures are\n"
               "separate subscriptions, so the market is never assumed.\n"
               "\n"
               "Notional values take K/M suffixes; a bare number is dollars, so\n"
               "--notional_band=1 sweeps one dollar.\n",
               program_name);
}

ClientConfig ClientConfig::ParseFromArgs(int argc, char* argv[]) {
    ClientConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];

        if (arg == "--bbo") {
            config.want_bbo = true;
        } else if (arg == "--volume_bands") {
            config.want_volume_bands = true;
        } else if (arg == "--price_bands") {
            config.want_price_bands = true;
        } else if (auto value = FlagValue(arg, "--server")) {
            config.server_address = std::string(*value);
        } else if (auto value = FlagValue(arg, "--symbol")) {
            config.symbol = std::string(*value);
        } else if (auto value = FlagValue(arg, "--market")) {
            // Only the two real markets are accepted. "unspecified" is not
            // spellable on purpose - it is the server's error case, not a
            // choice a user can make.
            if (*value == "spot") {
                config.market = wire::SPOT;
            } else if (*value == "futures") {
                config.market = wire::FUTURES;
            } else {
                fmt::print(stderr, "bad --market value: '{}' (expected spot or futures)\n", *value);
                std::exit(2);
            }
        } else if (auto value = FlagValue(arg, "--notional_band")) {
            config.want_volume_bands = true;
            for (const std::string& token : SplitCsv(*value)) {
                auto notional = ParseNotional(token);
                if (!notional) {
                    fmt::print(stderr, "bad --notional_band value: '{}'\n", token);
                    std::exit(2);
                }
                config.notional_bands.push_back(*notional);
            }
        } else if (auto value = FlagValue(arg, "--price_band")) {
            config.want_price_bands = true;
            for (const std::string& token : SplitCsv(*value)) {
                config.bps_bands.push_back(static_cast<uint32_t>(std::strtoul(token.c_str(), nullptr, 10)));
            }
        } else {
            fmt::print(stderr, "unknown flag: {}\n", arg);
            std::exit(2);
        }
    }

    return config;
}

wire::SubscribeRequest ClientConfig::ToRequest() const {
    wire::SubscribeRequest request;
    request.set_symbol(symbol);
    // Left UNSPECIFIED when absent rather than substituted here. Callers check
    // HasMarket() first; if one forgets, the server rejects the request - the
    // failure stays loud instead of silently becoming a spot subscription.
    if (market.has_value()) {
        request.set_market(*market);
    }
    request.set_bbo(want_bbo);

    // Calling mutable_*() is what marks an optional sub-message PRESENT.
    // Leaving the array inside empty is how "use the server's defaults" is
    // expressed - presence and contents carry different meanings.
    if (want_volume_bands) {
        auto* bands = request.mutable_volume_bands();
        for (uint64_t notional : notional_bands) {
            bands->add_notional_bands(notional);
        }
    }
    if (want_price_bands) {
        auto* bands = request.mutable_price_bands();
        for (uint32_t bps : bps_bands) {
            bands->add_bps_bands(bps);
        }
    }
    return request;
}

// ------------------------------------------------------------ formatting ---

std::string FormatScaled(uint64_t value, uint32_t scale, uint32_t decimals) {
    const uint64_t divisor = Pow10(scale);
    const uint64_t integer_part = value / divisor;
    uint64_t frac_part = value % divisor;

    if (decimals >= scale) {
        return fmt::format("{}.{:0{}}", integer_part, frac_part, scale);
    }
    if (decimals == 0) {
        return fmt::format("{}", integer_part);
    }
    frac_part /= Pow10(scale - decimals);
    return fmt::format("{}.{:0{}}", integer_part, frac_part, decimals);
}

std::string FormatScaled(uint64_t value, uint32_t scale) {
    return FormatScaled(value, scale, scale);
}

std::string FormatCompact(uint64_t value, uint32_t scale) {
    const uint64_t whole = value / Pow10(scale);
    if (whole >= 1'000'000) {
        return fmt::format("{}.{}M", whole / 1'000'000, (whole % 1'000'000) / 100'000);
    }
    if (whole >= 1'000) {
        return fmt::format("{}K", whole / 1'000);
    }
    return fmt::format("{}", whole);
}

std::string FormatVenues(const wire::ConsolidatedPriceLevel& level, uint32_t qty_scale) {
    std::string out;
    for (int i = 0; i < level.venues_size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += fmt::format("{}:{}", wire::Venue_Name(level.venues(i).venue()),
                           FormatScaled(level.venues(i).qty(), qty_scale, kQtyDecimals));
    }
    return out;
}

std::string FormatSlippageBps(uint64_t vwap, uint64_t reference, bool is_bid) {
    if (reference == 0 || vwap == 0) {
        return "-";
    }
    const uint64_t worse = is_bid ? reference : vwap;
    const uint64_t better = is_bid ? vwap : reference;
    if (worse < better) {
        return "0.0bps";  // shouldn't happen; don't underflow on uint64
    }
    // x10 so one decimal survives integer division.
    const uint64_t tenths_of_bps = (worse - better) * 100'000 / reference;
    return fmt::format("{}.{}bps", tenths_of_bps / 10, tenths_of_bps % 10);
}

// ---------------------------------------------------------------- output ---

void PrintBbo(const wire::Update& update) {
    const auto& bbo = update.bbo();
    // Mirrored ladder, the way an exchange depth display is drawn: the bid side
    // reads INWARD toward the spread (qty, then price) and the ask side reads
    // OUTWARD from it (price, then qty). That puts the two best prices adjacent
    // on either side of the '|', so the spread - and a crossed book - is read
    // at a glance instead of by comparing two numbers at opposite ends of the
    // line.
    fmt::print("seq={} {}  bid {} [{}] : {} | {} : {} [{}] ask{}\n", update.seq(), update.symbol(),
               FormatScaled(bbo.best_bid().total_qty(), update.qty_scale(), kQtyDecimals),
               FormatVenues(bbo.best_bid(), update.qty_scale()),
               FormatScaled(bbo.best_bid().price(), update.price_scale(), kPriceDecimals),
               FormatScaled(bbo.best_ask().price(), update.price_scale(), kPriceDecimals),
               FormatScaled(bbo.best_ask().total_qty(), update.qty_scale(), kQtyDecimals),
               FormatVenues(bbo.best_ask(), update.qty_scale()), bbo.crossed() ? "  CROSSED" : "");
}

namespace {

void PrintVolumeSide(const char* side, bool is_bid, uint64_t reference, const wire::Update& update,
                     const google::protobuf::RepeatedPtrField<wire::VolumeBandResult>& bands) {
    for (const auto& band : bands) {
        // `filled` is omitted when the target was met: it always equals the
        // target by construction, so it is a column of noise. It only carries
        // information when the book ran out - and then it IS the information.
        std::string shortfall;
        if (band.insufficient_depth()) {
            shortfall = fmt::format("  INSUFFICIENT DEPTH (filled {})",
                                    FormatCompact(band.filled_notional(), update.price_scale()));
        }

        fmt::print("  {} {:>6}  vwap {}  worst {}  qty {}  lvls {:>4}  slip {}{}\n", side,
                   FormatCompact(band.notional_threshold(), update.price_scale()),
                   FormatScaled(band.vwap(), update.price_scale(), kPriceDecimals),
                   FormatScaled(band.worst_price(), update.price_scale(), kPriceDecimals),
                   FormatScaled(band.filled_qty(), update.qty_scale(), kQtyDecimals), band.level_count(),
                   FormatSlippageBps(band.vwap(), reference, is_bid), shortfall);
    }
}

void PrintPriceSide(const char* side, const wire::Update& update,
                    const google::protobuf::RepeatedPtrField<wire::PriceBandResult>& bands) {
    for (const auto& band : bands) {
        // Without this marker a truncated band looks exactly like a complete
        // one: "5.3M within 1000bps" reads as the full answer when the walk
        // actually ran out of book long before reaching the limit price.
        const char* truncated = band.insufficient_depth() ? "  BOOK EXHAUSTED (lower bound)" : "";

        fmt::print("  {} {:>5}bps  limit {}  vwap {}  qty {}  notional {}  lvls {:>4}{}\n", side, band.bps_threshold(),
                   FormatScaled(band.limit_price(), update.price_scale(), kPriceDecimals),
                   FormatScaled(band.vwap(), update.price_scale(), kPriceDecimals),
                   FormatScaled(band.cum_qty(), update.qty_scale(), kQtyDecimals),
                   FormatCompact(band.cum_notional(), update.price_scale()), band.level_count(), truncated);
    }
}

}  // namespace

void PrintVolumeBands(const wire::Update& update) {
    const auto& bands = update.volume_bands();
    fmt::print("seq={} {}  volume bands  (best bid {} / ask {})\n", update.seq(), update.symbol(),
               FormatScaled(bands.best_bid(), update.price_scale(), kPriceDecimals),
               FormatScaled(bands.best_ask(), update.price_scale(), kPriceDecimals));
    PrintVolumeSide("BID", /*is_bid=*/true, bands.best_bid(), update, bands.bid_bands());
    PrintVolumeSide("ASK", /*is_bid=*/false, bands.best_ask(), update, bands.ask_bands());
}

void PrintPriceBands(const wire::Update& update) {
    const auto& bands = update.price_bands();
    fmt::print("seq={} {}  price bands\n", update.seq(), update.symbol());
    PrintPriceSide("BID", update, bands.bid_bands());
    PrintPriceSide("ASK", update, bands.ask_bands());
}

void PrintUpdate(const wire::Update& update) {
    switch (update.payload_case()) {
        case wire::Update::kBbo:
            PrintBbo(update);
            break;
        case wire::Update::kVolumeBands:
            PrintVolumeBands(update);
            break;
        case wire::Update::kPriceBands:
            PrintPriceBands(update);
            break;
        case wire::Update::PAYLOAD_NOT_SET:
            break;  // header-only update - nothing to show
    }
}

// ---------------------------------------------------------- subscription ---

int RunSubscription(const ClientConfig& config, const char* client_name, const wire::SubscribeRequest& request,
                    const std::function<void(const wire::Update&)>& on_update) {
    int backoff_seconds = 1;

    while (true) {
        Logger::Log(LogLevel::kInfo, "[{}] connecting to {}", client_name, config.server_address);

        auto channel = grpc::CreateChannel(config.server_address, grpc::InsecureChannelCredentials());
        auto stub = wire::Aggregator::NewStub(channel);

        // A ClientContext is single-use - it must be recreated per attempt.
        grpc::ClientContext context;
        std::unique_ptr<grpc::ClientReader<wire::Update>> reader(stub->Subscribe(&context, request));

        wire::Update update;
        std::optional<uint64_t> last_seq;

        // Conflation is COUNTED always and PRINTED once.
        uint64_t conflated_total = 0;
        bool gap_reported = false;

        while (reader->Read(&update)) {
            backoff_seconds = 1;  // healthy stream - reset the backoff

            if (last_seq && update.seq() != *last_seq + 1) {
                if (update.seq() > *last_seq) {
                    // Client-side gap detection: a skipped seq means
                    // the server's depth-1 conflation overwrote a
                    // pending update before we read it. Expected under load,
                    // not an error.
                    conflated_total += update.seq() - *last_seq - 1;
                    if (!gap_reported) {
                        gap_reported = true;
                        fmt::print(stderr,
                                   "[gap] expected seq {}, got {} ({} update(s) conflated away). This is conflation "
                                   "working as designed; further gaps are counted silently and totalled when "
                                   "the stream ends.\n",
                                   *last_seq + 1, update.seq(), update.seq() - *last_seq - 1);
                    }
                } else {
                    // Not conflation - seq went backwards, meaning the server
                    // restarted (its per-session counter resets to 0) or
                    // published out of order. Subtracting would underflow.
                    fmt::print(stderr, "[seq-reset] seq went backwards: {} -> {}\n", *last_seq, update.seq());
                }
            }
            last_seq = update.seq();

            on_update(update);
        }

        // The total the per-event notices used to carry, in one line. Printed
        // only when there was something to report, and only against a seq the
        // reader can divide it by, so the RATE is recoverable rather than just
        // the count.
        if (conflated_total > 0 && last_seq) {
            fmt::print(stderr, "[gap] {} update(s) conflated away over {} published ({}%)\n", conflated_total,
                       *last_seq, conflated_total * 100 / *last_seq);
        }

        grpc::Status status = reader->Finish();
        if (!status.ok()) {
            fmt::print(stderr, "[{}] stream ended: {}\n", client_name, status.error_message());
        }

        // A rejected subscription won't succeed on retry - don't spin on it.
        if (status.error_code() == grpc::StatusCode::UNIMPLEMENTED ||
            status.error_code() == grpc::StatusCode::INVALID_ARGUMENT) {
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
        backoff_seconds = std::min(backoff_seconds * 2, kMaxBackoffSeconds);
    }
}

}  // namespace market_data
