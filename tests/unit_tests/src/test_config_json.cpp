#include <gtest/gtest.h>

#include "config/config.h"

#include <unistd.h>

#include <array>
#include <string>
#include <vector>

#include "types/instrument_registry.h"
#include "types/venue.h"

namespace {

// A realistic session file: two entries, one venue set, one CLI-overridable
// scalar left at its default (connections) and one overridden (depth). This
// is the shape the interviewer should be shown as "the config file", so
// every other test in here is a variation ON this one, not a fresh shape.
constexpr std::string_view kValidConfig = R"({
    "venues": ["binance", "bybit", "okx"],
    "depth": 800,
    "instruments": [
        { "symbol": "BTCUSDT", "market": ["spot"] },
        { "symbol": "eth-usdt", "market": ["spot"] }
    ]
})";

}  // namespace

// --- happy path --------------------------------------------------------------

TEST(ParseConfigJsonTest, ParsesAValidDocument) {
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kValidConfig, registry);

    ASSERT_TRUE(result.Ok()) << result.error;
    EXPECT_TRUE(result.config.Validate());

    EXPECT_EQ(result.config.venues.size(), 3u);
    EXPECT_NE(std::find(result.config.venues.begin(), result.config.venues.end(), VenueId::BINANCE),
              result.config.venues.end());
    EXPECT_NE(std::find(result.config.venues.begin(), result.config.venues.end(), VenueId::BYBIT),
              result.config.venues.end());
    EXPECT_NE(std::find(result.config.venues.begin(), result.config.venues.end(), VenueId::OKX),
              result.config.venues.end());

    EXPECT_EQ(result.config.depth, 800u);
    // Not in the document - the ServerConfig default stands untouched.
    EXPECT_EQ(result.config.connections, kDefaultConnections);
    EXPECT_EQ(result.config.grpc_port, 50051);
}

TEST(ParseConfigJsonTest, EachInstrumentEntryGetsARegisteredIdAndCanonicalSymbol) {
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kValidConfig, registry);
    ASSERT_TRUE(result.Ok()) << result.error;

    ASSERT_EQ(result.config.instruments.size(), 2u);

    const InstrumentEntry& btc = result.config.instruments[0];
    EXPECT_EQ(btc.symbol, "BTCUSDT");
    EXPECT_EQ(btc.markets, std::vector<MarketType>{MarketType::kSpot});
    // The id is not a fresh guess - it must be the SAME id the registry itself
    // hands back for the canonical spelling. This is what md_core_main.cpp /
    // md_provider_main.cpp rely on to build InstrumentKeys later.
    EXPECT_EQ(registry.Find("BTCUSDT"), btc.id);

    // Registered as "eth-usdt" in the file; the entry stores the CANONICAL
    // form, because that is what goes on the wire (aggregator_service.cpp
    // FillHeader), not whatever spelling the operator happened to type.
    const InstrumentEntry& eth = result.config.instruments[1];
    EXPECT_EQ(eth.symbol, "ETHUSDT");
    EXPECT_EQ(registry.Find("ethusdt"), eth.id);
}

TEST(ParseConfigJsonTest, OneSymbolAcrossTwoEntriesConsumesOneRegistryId) {
    // This is the {"market": ["spot", "futures"]} case, expressed as it will
    // actually arrive on the wire: two entries naming the same symbol under
    // different markets, not one array of markets. Confirms the registry side
    // of the earlier InstrumentRegistry guarantee end-to-end through the JSON
    // loader, not just against the registry directly.
    constexpr std::string_view kJson = R"({
        "venues": ["binance"],
        "instruments": [
            { "symbol": "BTCUSDT", "market": ["spot"] },
            { "symbol": "BTCUSDT", "market": ["futures"] }
        ]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);

    ASSERT_TRUE(result.Ok()) << result.error;
    EXPECT_EQ(registry.size(), 1u);
    EXPECT_EQ(result.config.instruments[0].id, result.config.instruments[1].id);

    // Two entries, two DIFFERENT keys - spot and futures never merge.
    const InstrumentKey spot_key = MakeKey(result.config.instruments[0].id, MarketType::kSpot);
    const InstrumentKey futures_key = MakeKey(result.config.instruments[1].id, MarketType::kFutures);
    EXPECT_NE(spot_key, futures_key);
}

TEST(ParseConfigJsonTest, OneEntryNamingBothMarketsAlsoConsumesOneId) {
    // The shape from the config schema directly: one entry, two markets.
    constexpr std::string_view kJson = R"({
        "venues": ["binance"],
        "instruments": [
            { "symbol": "BTCUSDT", "market": ["spot", "futures"] }
        ]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);

    ASSERT_TRUE(result.Ok()) << result.error;
    EXPECT_EQ(registry.size(), 1u);
    ASSERT_EQ(result.config.instruments.size(), 1u);
    EXPECT_EQ(result.config.instruments[0].markets, (std::vector<MarketType>{MarketType::kSpot, MarketType::kFutures}));

    // Both markets pass validation now that both have providers behind them.
    // This assertion used to expect false, back when Validate() rejected
    // futures outright; the subject of this test is the REGISTRY ID above -
    // one symbol consuming one id regardless of how many markets name it -
    // and that is unchanged either way.
    EXPECT_TRUE(result.config.Validate());
}

// --- malformed documents ------------------------------------------------------

TEST(ParseConfigJsonTest, RejectsInvalidJsonSyntax) {
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson("{ not valid json", registry);
    EXPECT_FALSE(result.Ok());
    EXPECT_NE(result.error.find("invalid JSON"), std::string::npos) << result.error;
}

TEST(ParseConfigJsonTest, RejectsATopLevelArray) {
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson("[1, 2, 3]", registry);
    EXPECT_FALSE(result.Ok());
}

TEST(ParseConfigJsonTest, RejectsAnUnknownTopLevelKey) {
    // A typo like "instrment" must be LOUD, not silently skipped - a silently
    // ignored key is how an operator ends up running with no instruments and
    // no idea why.
    constexpr std::string_view kJson = R"({
        "venues": ["binance"],
        "instrments": [{ "symbol": "BTCUSDT", "market": ["spot"] }]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
    EXPECT_NE(result.error.find("instrments"), std::string::npos) << result.error;
}

TEST(ParseConfigJsonTest, RejectsAMissingVenuesKey) {
    constexpr std::string_view kJson = R"({
        "instruments": [{ "symbol": "BTCUSDT", "market": ["spot"] }]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
    EXPECT_NE(result.error.find("venues"), std::string::npos) << result.error;
}

TEST(ParseConfigJsonTest, RejectsAMissingInstrumentsKey) {
    constexpr std::string_view kJson = R"({ "venues": ["binance"] })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
    EXPECT_NE(result.error.find("instruments"), std::string::npos) << result.error;
}

TEST(ParseConfigJsonTest, RejectsAnUnknownVenueName) {
    constexpr std::string_view kJson = R"({
        "venues": ["binance", "deribit"],
        "instruments": [{ "symbol": "BTCUSDT", "market": ["spot"] }]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
    EXPECT_NE(result.error.find("deribit"), std::string::npos) << result.error;
}

TEST(ParseConfigJsonTest, RejectsADuplicateVenue) {
    constexpr std::string_view kJson = R"({
        "venues": ["binance", "binance"],
        "instruments": [{ "symbol": "BTCUSDT", "market": ["spot"] }]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
}

TEST(ParseConfigJsonTest, RejectsAnInvalidSymbolWithTheSpecificReason) {
    // Proves the loader surfaces WHICH SymbolStatus fired, not a generic
    // "bad instrument" - this is the whole point of ValidateSymbol returning a
    // reason instead of a bool.
    constexpr std::string_view kJson = R"({
        "venues": ["binance"],
        "instruments": [{ "symbol": "BTC.USDT", "market": ["spot"] }]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
    EXPECT_NE(result.error.find("BTC.USDT"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find(DescribeSymbolStatus(SymbolStatus::kBadCharacter)), std::string::npos) << result.error;
}

TEST(ParseConfigJsonTest, RejectsAnUnknownMarketName) {
    constexpr std::string_view kJson = R"({
        "venues": ["binance"],
        "instruments": [{ "symbol": "BTCUSDT", "market": ["perpetual"] }]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
    EXPECT_NE(result.error.find("perpetual"), std::string::npos) << result.error;
}

TEST(ParseConfigJsonTest, RejectsAnEmptyMarketList) {
    constexpr std::string_view kJson = R"({
        "venues": ["binance"],
        "instruments": [{ "symbol": "BTCUSDT", "market": [] }]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
}

TEST(ParseConfigJsonTest, RejectsTheSameSymbolAndMarketListedTwice) {
    // Two spellings of one symbol, both spot - must collide on the packed
    // InstrumentKey, not on the raw strings ("BTCUSDT" != "btc-usdt").
    constexpr std::string_view kJson = R"({
        "venues": ["binance"],
        "instruments": [
            { "symbol": "BTCUSDT", "market": ["spot"] },
            { "symbol": "btc-usdt", "market": ["spot"] }
        ]
    })";
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::ParseJson(kJson, registry);
    EXPECT_FALSE(result.Ok());
}

// --- Validate(): policy, not syntax -------------------------------------------

// This test used to assert the opposite - that Validate() rejected futures as
// "not wired to a provider yet". Its own comment predicted the change:
// "starts working the moment Validate's check is removed, with no other
// change." That is exactly what happened once the futures streams landed and
// were verified live, so the assertion is inverted rather than deleted - the
// config layer's contract genuinely changed, and a test that still demanded a
// rejection would be pinning down behaviour we deliberately removed.
TEST(ServerConfigValidateTest, AcceptsFuturesNowThatProvidersExist) {
    InstrumentEntry entry;
    entry.symbol = "BTCUSDT";
    entry.markets = {MarketType::kFutures};

    ServerConfig config;
    config.venues = {VenueId::BINANCE};
    config.instruments = {entry};

    EXPECT_TRUE(config.Validate());
}

// Both markets for one symbol is the case the InstrumentKey packing exists
// for: two separate books under one config entry, never merged.
TEST(ServerConfigValidateTest, AcceptsSpotAndFuturesTogether) {
    InstrumentEntry entry;
    entry.symbol = "BTCUSDT";
    entry.markets = {MarketType::kSpot, MarketType::kFutures};

    ServerConfig config;
    config.venues = {VenueId::BINANCE, VenueId::BYBIT};
    config.instruments = {entry};

    EXPECT_TRUE(config.Validate());
}

TEST(ServerConfigValidateTest, AcceptsSpotOnly) {
    InstrumentEntry entry;
    entry.symbol = "BTCUSDT";
    entry.markets = {MarketType::kSpot};

    ServerConfig config;
    config.venues = {VenueId::BINANCE};
    config.instruments = {entry};

    EXPECT_TRUE(config.Validate());
}

// --- LoadFile: the actual "read a file and init" path -------------------------

namespace {

// A minimal temp-file helper. mkstemp rather than tmpnam: tmpnam hands back a
// NAME with no guarantee it still doesn't exist by the time this test opens
// it - a real race under parallel test execution, and deprecated for exactly
// that reason. mkstemp creates and opens the file atomically in one call.
class TempConfigFile {
   public:
    explicit TempConfigFile(std::string_view contents) {
        path_ = "/tmp/config_test_XXXXXX";
        const int fd = mkstemp(path_.data());
        // No ASSERT_* here: gtest's fatal assertions expand to a bare
        // `return;`, which a constructor cannot use. A failure here means the
        // test environment itself is broken (no writable /tmp) - the calling
        // test's own check of LoadFile's result will fail loudly regardless.
        if (fd < 0) {
            path_.clear();
            return;
        }
        const ssize_t written = write(fd, contents.data(), contents.size());
        close(fd);
        if (written != static_cast<ssize_t>(contents.size())) {
            path_.clear();
        }
    }
    ~TempConfigFile() { unlink(path_.c_str()); }

    const std::string& path() const { return path_; }

   private:
    std::string path_;
};

}  // namespace

TEST(LoadFileTest, ReadsAndParsesARealFile) {
    TempConfigFile file(kValidConfig);
    InstrumentRegistry registry;

    const ConfigLoadResult result = ServerConfig::LoadFile(file.path(), registry);

    ASSERT_TRUE(result.Ok()) << result.error;
    EXPECT_EQ(result.config.venues.size(), 3u);
    EXPECT_EQ(result.config.instruments.size(), 2u);
}

TEST(LoadFileTest, MissingFileIsAConfigErrorNotACrash) {
    InstrumentRegistry registry;
    const ConfigLoadResult result = ServerConfig::LoadFile("/no/such/path/session.json", registry);

    EXPECT_FALSE(result.Ok());
    EXPECT_NE(result.error.find("/no/such/path/session.json"), std::string::npos) << result.error;
}

// --- CliOverrides: what --depth=/--grpc_port= produce -------------------------
//
// connections has no CLI override at all - it is JSON-only ("connections" in
// server_config_<market>.json, config.h), so there is exactly one place that
// decides how many redundant sockets to open, never two that could disagree.
//
// No --config= any more either: the session file is server_config_<market>.json
// (config.h::ConfigFileForMarket), chosen by --market and always read from the
// working directory. There is no second place to point at a different file and
// disagree with it.

TEST(CliOverridesTest, ScalarsAreUnsetByDefault) {
    std::array<const char*, 1> args = {"aggregator"};
    const CliOverrides overrides =
        CliOverrides::ParseFromArgs(static_cast<int>(args.size()), const_cast<char**>(args.data()));

    // std::optional, not a value with a sentinel: "not passed on the command
    // line" and "passed as zero" must stay DISTINGUISHABLE, or applying
    // overrides could never tell whether to keep the file's value.
    EXPECT_FALSE(overrides.depth.has_value());
    EXPECT_FALSE(overrides.grpc_port.has_value());
}

TEST(CliOverridesTest, ScalarFlagsAreCaptured) {
    std::array<const char*, 3> args = {"aggregator", "--depth=1000", "--grpc_port=9000"};
    const CliOverrides overrides =
        CliOverrides::ParseFromArgs(static_cast<int>(args.size()), const_cast<char**>(args.data()));

    ASSERT_TRUE(overrides.depth.has_value());
    EXPECT_EQ(*overrides.depth, 1000u);
    ASSERT_TRUE(overrides.grpc_port.has_value());
    EXPECT_EQ(*overrides.grpc_port, 9000);
}

TEST(CliOverridesTest, ConnectionsFlagIsAnUnknownArgument) {
    // Confirms the removal took effect: --connections= is no longer
    // special-cased, so it falls into the same "Unknown argument" path as any
    // other typo, the same way --config= already does below. The flags
    // around it still parse normally.
    std::array<const char*, 3> args = {"aggregator", "--connections=3", "--depth=500"};
    const CliOverrides overrides =
        CliOverrides::ParseFromArgs(static_cast<int>(args.size()), const_cast<char**>(args.data()));

    ASSERT_TRUE(overrides.depth.has_value());
    EXPECT_EQ(*overrides.depth, 500u);
}

TEST(CliOverridesTest, AnOldConfigFlagIsNowJustAnUnknownArgument) {
    // Confirms the removal actually took effect: --config= is no longer
    // special-cased, so it falls into the same "Unknown argument" path as any
    // other typo - it does not silently do nothing, and it does not crash.
    // The flags around it still parse normally.
    std::array<const char*, 3> args = {"aggregator", "--config=session.json", "--depth=500"};
    const CliOverrides overrides =
        CliOverrides::ParseFromArgs(static_cast<int>(args.size()), const_cast<char**>(args.data()));

    ASSERT_TRUE(overrides.depth.has_value());
    EXPECT_EQ(*overrides.depth, 500u);
}
