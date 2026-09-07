#include "config.h"

#include <fmt/format.h>
#include <simdjson.h>

#include <array>
#include <cctype>
#include <fstream>

namespace {

// The only keys ParseJson understands. Anything else in the document is
// rejected rather than ignored - a typo like "instrment" that silently did
// nothing would be far harder to notice than a startup error naming it.
constexpr std::array<std::string_view, 5> kKnownKeys = {"venues", "depth", "connections", "grpc_port", "instruments"};

bool IsKnownKey(std::string_view key) {
    return std::find(kKnownKeys.begin(), kKnownKeys.end(), key) != kKnownKeys.end();
}

// VenueConverter::ToVenueId is exact-match and case-sensitive on purpose
// (types/venue_registry.h): on the wire a venue name comes from a remote
// process, and a near-miss must stay visible rather than resolve to a
// neighbour. A config file is not the wire - an operator typing "binance"
// is not a mismatch to catch, so this uppercases before the lookup, the same
// normalise-then-match split instrument symbols already use.
std::string ToUpper(std::string_view value) {
    std::string upper(value);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return upper;
}

// Reads an OPTIONAL unsigned field. Absence is not an error - `out` is left
// at the ServerConfig default. A field present with the wrong type is.
//
// Returns false only on that real error, with `error` set - the caller
// returns immediately rather than continuing to parse a document already
// known to be wrong.
bool ParseOptionalUint32(const simdjson::dom::object& root, std::string_view key, uint32_t& out, std::string& error) {
    simdjson::dom::element element;
    const simdjson::error_code find_error = root[key].get(element);
    if (find_error == simdjson::NO_SUCH_FIELD) {
        return true;  // absent - not an error, default stands
    }
    if (find_error) {
        error = fmt::format("config: \"{}\": {}", key, simdjson::error_message(find_error));
        return false;
    }
    int64_t value = 0;
    if (element.get(value)) {
        error = fmt::format("config: \"{}\" must be a non-negative integer", key);
        return false;
    }
    if (value < 0) {
        error = fmt::format("config: \"{}\" must be a non-negative integer, got {}", key, value);
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool ParseOptionalInt(const simdjson::dom::object& root, std::string_view key, int& out, std::string& error) {
    simdjson::dom::element element;
    const simdjson::error_code find_error = root[key].get(element);
    if (find_error == simdjson::NO_SUCH_FIELD) {
        return true;
    }
    if (find_error) {
        error = fmt::format("config: \"{}\": {}", key, simdjson::error_message(find_error));
        return false;
    }
    int64_t value = 0;
    if (element.get(value)) {
        error = fmt::format("config: \"{}\" must be an integer", key);
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

}  // namespace

ConfigLoadResult ServerConfig::ParseJson(std::string_view json, InstrumentRegistry& registry) {
    ConfigLoadResult result;

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    // parser.parse copies `json` into its own padded buffer (realloc_if_needed
    // defaults to true), so an ordinary std::string_view - a config file's
    // contents, or a literal in a test - is a valid argument without the
    // caller doing anything simdjson-specific first.
    if (const auto parse_error = parser.parse(json.data(), json.size()).get(doc); parse_error) {
        result.error = fmt::format("config: invalid JSON: {}", simdjson::error_message(parse_error));
        return result;
    }

    simdjson::dom::object root;
    if (doc.get(root)) {
        result.error = "config: the top level of the document must be an object";
        return result;
    }

    for (const auto& [key, value] : root) {
        (void)value;
        if (!IsKnownKey(key)) {
            result.error = fmt::format("config: unknown key \"{}\"", key);
            return result;
        }
    }

    // --- venues (required) --------------------------------------------------
    simdjson::dom::array venues_array;
    if (root["venues"].get(venues_array)) {
        result.error = "config: \"venues\" is required and must be an array of strings";
        return result;
    }
    for (auto venue_element : venues_array) {
        std::string_view venue_name;
        if (venue_element.get(venue_name)) {
            result.error = "config: venues[] entries must be strings";
            return result;
        }
        const VenueId venue_id = VenueConverter::ToVenueId(ToUpper(venue_name));
        if (venue_id == VenueId::COUNT) {
            result.error = fmt::format("config: venues[]: unknown venue \"{}\"", venue_name);
            return result;
        }
        if (std::find(result.config.venues.begin(), result.config.venues.end(), venue_id) !=
            result.config.venues.end()) {
            result.error = fmt::format("config: venues[]: duplicate venue \"{}\"", venue_name);
            return result;
        }
        result.config.venues.push_back(venue_id);
    }

    // --- instruments (required) ---------------------------------------------
    simdjson::dom::array instruments_array;
    if (root["instruments"].get(instruments_array)) {
        result.error = "config: \"instruments\" is required and must be an array";
        return result;
    }

    // Detects one (symbol, market) pair configured twice - directly ("BTCUSDT"
    // listed under "spot" in two different entries) or via two spellings of
    // the same symbol ("BTCUSDT" and "btc-usdt" both spot). Both produce the
    // same InstrumentKey, so its packed form is what dedup checks against,
    // not the raw JSON.
    std::vector<uint32_t> seen_keys;

    size_t index = 0;
    for (auto entry_element : instruments_array) {
        simdjson::dom::object entry_object;
        if (entry_element.get(entry_object)) {
            result.error = fmt::format("config: instruments[{}] must be an object", index);
            return result;
        }

        std::string_view symbol;
        if (entry_object["symbol"].get(symbol)) {
            result.error = fmt::format("config: instruments[{}].symbol is required and must be a string", index);
            return result;
        }

        // Validated here, before Register(), purely so the error names the
        // REASON ("contains a character that is not a letter or a digit")
        // rather than the registry's generic "could not register".
        const SymbolStatus status = ValidateSymbol(NormalizeSymbol(symbol));
        if (status != SymbolStatus::kOk) {
            result.error = fmt::format("config: instruments[{}].symbol \"{}\" rejected: {}", index, symbol,
                                       DescribeSymbolStatus(status));
            return result;
        }

        const std::optional<InstrumentId> id = registry.Register(symbol);
        if (!id.has_value()) {
            // ValidateSymbol already passed, so Register can only have failed
            // because the registry is at kMaxInstruments.
            result.error = fmt::format("config: instruments[{}].symbol \"{}\": registry full (max {} instruments)",
                                       index, symbol, kMaxInstruments);
            return result;
        }

        simdjson::dom::array markets_array;
        if (entry_object["market"].get(markets_array)) {
            result.error =
                fmt::format("config: instruments[{}].market is required and must be an array of strings", index);
            return result;
        }

        InstrumentEntry entry;
        entry.id = *id;
        entry.symbol = std::string(registry.Name(*id));  // canonical spelling, not what the config wrote

        for (auto market_element : markets_array) {
            std::string_view market_name;
            if (market_element.get(market_name)) {
                result.error = fmt::format("config: instruments[{}].market entries must be strings", index);
                return result;
            }
            const std::optional<MarketType> market = ToMarketType(market_name);
            if (!market.has_value()) {
                result.error = fmt::format("config: instruments[{}].market \"{}\" must be \"spot\" or \"futures\"",
                                           index, market_name);
                return result;
            }

            const uint32_t packed_key = MakeKey(*id, *market).Packed();
            if (std::find(seen_keys.begin(), seen_keys.end(), packed_key) != seen_keys.end()) {
                result.error = fmt::format("config: instruments[{}]: \"{}\" / \"{}\" is configured more than once",
                                           index, entry.symbol, market_name);
                return result;
            }
            seen_keys.push_back(packed_key);
            entry.markets.push_back(*market);
        }

        if (entry.markets.empty()) {
            result.error = fmt::format("config: instruments[{}].market must name at least one market", index);
            return result;
        }

        result.config.instruments.push_back(std::move(entry));
        ++index;
    }

    // --- optional scalars -----------------------------------------------------
    if (!ParseOptionalUint32(root, "depth", result.config.depth, result.error)) {
        return result;
    }
    if (!ParseOptionalUint32(root, "connections", result.config.connections, result.error)) {
        return result;
    }
    if (!ParseOptionalInt(root, "grpc_port", result.config.grpc_port, result.error)) {
        return result;
    }

    return result;  // error is still empty here - success
}

ConfigLoadResult ServerConfig::LoadFile(const std::string& path, InstrumentRegistry& registry) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ConfigLoadResult result;
        result.error = fmt::format("config: could not open \"{}\"", path);
        return result;
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return ParseJson(contents.str(), registry);
}

bool ServerConfig::Validate() const {
    if (venues.empty()) {
        std::cerr << "Error: at least one venue required\n";
        return false;
    }
    if (instruments.empty()) {
        std::cerr << "Error: at least one instrument required\n";
        return false;
    }
    // Futures used to be rejected here - the schema parsed "futures" but no
    // provider subscribed to one, so accepting it would have left Core holding
    // a book that never ticked. That guard is gone because the streams now
    // exist and were verified against the live venues:
    //   Binance  fstream/fapi, pu-based continuity  - 2340 msgs, 0 resyncs
    //   Bybit    /v5/public/linear                  - 733 msgs, 0 rule violations
    //   OKX      BTC-USDT-SWAP + ctVal conversion   - 173 msgs, 0 rule violations
    //
    // Nothing venue-specific is checked here on purpose. Whether a given
    // (venue, market) is actually reachable is a question about ENDPOINTS, and
    // this file knows nothing about venues_config.json - md_provider_main.cpp
    // answers it, before building the provider, by requiring the enabled venue
    // to have endpoints for the market being run.
    if (depth == 0) {
        std::cerr << "Error: --depth must be greater than zero\n";
        return false;
    }
    // Rejected, not clamped. Unlike depth - where the venue's fixed tiers
    // leave no choice but to round - this cap is OURS, so silently changing
    // the operator's number would hide a typo rather than report it.
    if (connections == 0) {
        std::cerr << "Error: connections must be at least 1\n";
        return false;
    }
    if (connections > kMaxConnections) {
        std::cerr << "Error: connections must not exceed " << kMaxConnections
                  << " (venue connection limits are unverified)\n";
        return false;
    }
    return true;
}

CliOverrides CliOverrides::ParseFromArgs(int argc, char* argv[]) {
    CliOverrides overrides;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.find("--depth=") == 0) {
            overrides.depth = static_cast<uint32_t>(std::stoul(arg.substr(8)));
        } else if (arg.find("--grpc_port=") == 0) {
            overrides.grpc_port = std::stoi(arg.substr(12));
        } else if (arg.find("--market=") == 0) {
            // Consumed by ParseMarketFlag, not here - listed so it does not
            // trip the "Unknown argument" branch.
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
        }
    }

    return overrides;
}

std::optional<MarketType> ParseMarketFlag(int argc, char* argv[]) {
    constexpr std::string_view kPrefix = "--market=";
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.substr(0, kPrefix.size()) == kPrefix) {
            return ToMarketType(arg.substr(kPrefix.size()));
        }
    }
    return std::nullopt;
}
