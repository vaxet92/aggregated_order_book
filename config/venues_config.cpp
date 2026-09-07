#include "venues_config.h"

#include <fmt/format.h>
#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

namespace market_data {
namespace {

// The only keys a market object may contain. Anything else is rejected rather
// than ignored - same rule as ServerConfig::ParseJson, and for the same
// reason: a typo like "ws_hostt" that silently left the host empty would fail
// far away from its cause, as a connect error against "".
constexpr std::array<std::string_view, 8> kMarketKeys = {
    "ws_host",   "ws_port",   "depth_path",      "bbo_path",
    "rest_host", "rest_port", "rest_depth_path", "rest_instruments_path"};

bool IsKnownMarketKey(std::string_view key) {
    return std::find(kMarketKeys.begin(), kMarketKeys.end(), key) != kMarketKeys.end();
}

// Venue names in this file are operator-typed, so "binance" and "BINANCE" must
// both work - the same normalise-then-match split ServerConfig uses. Market
// names are NOT normalised, because ToMarketType is already the single spelling
// authority for "spot"/"futures" and it is used identically by server_config.
std::string ToUpper(std::string_view value) {
    std::string upper(value);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return upper;
}

// Reads a REQUIRED string field into `out`. Absent, wrong-typed and empty all
// report the same way: a host of "" is not a usable endpoint, so accepting it
// would only move the failure to connect time.
bool ParseRequiredString(const simdjson::dom::object& market_object, std::string_view key, std::string_view venue_name,
                         std::string_view market_name, std::string& out, std::string& error) {
    std::string_view value;
    if (market_object[key].get(value)) {
        error =
            fmt::format(R"(venues_config: {}.{}: "{}" is required and must be a string)", venue_name, market_name, key);
        return false;
    }
    if (value.empty()) {
        error = fmt::format(R"(venues_config: {}.{}: "{}" must not be empty)", venue_name, market_name, key);
        return false;
    }
    out = std::string(value);
    return true;
}

// Reads an OPTIONAL string field. Absence leaves `out` empty and is not an
// error; present-but-wrong-typed is.
bool ParseOptionalString(const simdjson::dom::object& market_object, std::string_view key, std::string_view venue_name,
                         std::string_view market_name, std::string& out, std::string& error) {
    std::string_view value;
    const auto field_error = market_object[key].get(value);
    if (field_error == simdjson::NO_SUCH_FIELD) {
        return true;  // absent - fine
    }
    if (field_error) {
        error = fmt::format(R"(venues_config: {}.{}: "{}" must be a string)", venue_name, market_name, key);
        return false;
    }
    out = std::string(value);
    return true;
}

}  // namespace

const VenueEndpoints* VenuesConfig::Find(VenueId venue, MarketType market) const {
    // VenueId::COUNT is a sentinel, not a venue - indexing on it would run off
    // the end of entries_. Callers get it from a failed ToVenueId lookup.
    if (venue == VenueId::COUNT) {
        return nullptr;
    }
    const auto& entry = entries_[IndexOf(venue, market)];
    return entry.has_value() ? &entry.value() : nullptr;
}

VenuesConfigLoadResult VenuesConfig::ParseJson(std::string_view json) {
    VenuesConfigLoadResult result;

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    // Same note as ServerConfig::ParseJson - parse() copies into its own padded
    // buffer, so a plain string_view is a valid argument. Every string is
    // copied OUT into the VenueEndpoints below, so nothing outlives `parser`.
    if (const auto parse_error = parser.parse(json.data(), json.size()).get(doc); parse_error) {
        result.error = fmt::format("venues_config: invalid JSON: {}", simdjson::error_message(parse_error));
        return result;
    }

    simdjson::dom::object root;
    if (doc.get(root)) {
        result.error = "venues_config: the top level of the document must be an object of venue names";
        return result;
    }

    bool any_entry = false;

    for (const auto& [venue_name, venue_value] : root) {
        const VenueId venue = VenueConverter::ToVenueId(ToUpper(venue_name));
        if (venue == VenueId::COUNT) {
            result.error = fmt::format("venues_config: unknown venue \"{}\"", venue_name);
            return result;
        }

        simdjson::dom::object venue_object;
        if (venue_value.get(venue_object)) {
            result.error =
                fmt::format(R"(venues_config: "{}" must be an object of market names ("spot"/"futures"))", venue_name);
            return result;
        }

        for (const auto& [market_name, market_value] : venue_object) {
            const std::optional<MarketType> market = ToMarketType(market_name);
            if (!market.has_value()) {
                result.error = fmt::format(R"(venues_config: {}: unknown market "{}" - must be "spot" or "futures")",
                                           venue_name, market_name);
                return result;
            }

            simdjson::dom::object market_object;
            if (market_value.get(market_object)) {
                result.error = fmt::format("venues_config: {}.{} must be an object", venue_name, market_name);
                return result;
            }

            for (const auto& [key, value] : market_object) {
                (void)value;
                if (!IsKnownMarketKey(key)) {
                    result.error =
                        fmt::format(R"(venues_config: {}.{}: unknown key "{}")", venue_name, market_name, key);
                    return result;
                }
            }

            const size_t index = IndexOf(venue, *market);
            // A repeated (venue, market) means the file defines the same
            // endpoint twice. Last-one-wins would make the live value depend on
            // key order, which is exactly the kind of silence this loader
            // rejects everywhere else.
            if (result.config.entries_[index].has_value()) {
                result.error = fmt::format("venues_config: {}.{} is defined more than once", venue_name, market_name);
                return result;
            }

            VenueEndpoints endpoints;
            if (!ParseRequiredString(market_object, "ws_host", venue_name, market_name, endpoints.ws_host,
                                     result.error) ||
                !ParseRequiredString(market_object, "ws_port", venue_name, market_name, endpoints.ws_port,
                                     result.error) ||
                !ParseRequiredString(market_object, "depth_path", venue_name, market_name, endpoints.depth_path,
                                     result.error) ||
                !ParseRequiredString(market_object, "bbo_path", venue_name, market_name, endpoints.bbo_path,
                                     result.error)) {
                return result;
            }

            if (!ParseOptionalString(market_object, "rest_host", venue_name, market_name, endpoints.rest_host,
                                     result.error) ||
                !ParseOptionalString(market_object, "rest_port", venue_name, market_name, endpoints.rest_port,
                                     result.error) ||
                !ParseOptionalString(market_object, "rest_depth_path", venue_name, market_name,
                                     endpoints.rest_depth_path, result.error) ||
                !ParseOptionalString(market_object, "rest_instruments_path", venue_name, market_name,
                                     endpoints.rest_instruments_path, result.error)) {
                return result;
            }

            // a REST block is either absent entirely, or complete enough
            // to issue a request: host, port, and at least one path.
            //
            // The two paths are deliberately INDEPENDENT rather than both
            // required. Binance needs a depth snapshot and no instrument
            // metadata; OKX futures needs the contract size and no depth
            // snapshot. Demanding both would force a dummy path into the file
            // for whichever one the venue does not use, and a dummy path is a
            // GET waiting to happen.
            //
            // What this still catches is a host with no path at all, which
            // would otherwise present as an HTTP error against "" rather than
            // as the config mistake it is.
            const bool has_path = !endpoints.rest_depth_path.empty() || !endpoints.rest_instruments_path.empty();
            const bool has_host_or_port = !endpoints.rest_host.empty() || !endpoints.rest_port.empty();
            if ((has_path || has_host_or_port) &&
                (endpoints.rest_host.empty() || endpoints.rest_port.empty() || !has_path)) {
                result.error = fmt::format(
                    "venues_config: {}.{}: a REST block needs rest_host, rest_port and at least one of "
                    "rest_depth_path / rest_instruments_path (or omit all of them for a venue that needs no REST)",
                    venue_name, market_name);
                return result;
            }

            result.config.entries_[index] = std::move(endpoints);
            any_entry = true;
        }
    }

    // An empty or all-empty document parses cleanly but configures nothing,
    // which would surface later as "every venue is unconfigured" at wiring
    // time. Failing here names the actual problem.
    if (!any_entry) {
        result.error = "venues_config: no (venue, market) endpoints defined";
        return result;
    }

    return result;  // error still empty - success
}

VenuesConfigLoadResult VenuesConfig::LoadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        VenuesConfigLoadResult result;
        result.error = fmt::format("venues_config: could not open \"{}\"", path);
        return result;
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return ParseJson(contents.str());
}

std::string ResolvePath(std::string_view path_template, std::string_view symbol) {
    std::string resolved(path_template);
    // Replaces EVERY occurrence, not just the first. No current template uses
    // the placeholder twice, but "the first one only" is a silent trap for
    // whoever writes the one that does.
    for (size_t pos = resolved.find(kSymbolPlaceholder); pos != std::string::npos;
         pos = resolved.find(kSymbolPlaceholder, pos + symbol.size())) {
        resolved.replace(pos, kSymbolPlaceholder.size(), symbol);
    }
    return resolved;
}

}  // namespace market_data
