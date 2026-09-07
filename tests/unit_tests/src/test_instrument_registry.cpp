#include <gtest/gtest.h>

#include "types/instrument_registry.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace {

// Registration order is what assigns ids, so the tests spell the order out
// rather than leaning on any relationship to the InstrumentId enumerators that
// venue.h still declares. That relationship holds today only because BTCUSDT
// happens to be listed first, and nothing may depend on it.
constexpr std::string_view kBtc = "BTCUSDT";
constexpr std::string_view kEth = "ETHUSDT";
constexpr std::string_view kSol = "SOLUSDT";

uint16_t Raw(InstrumentId instrument) {
    return static_cast<uint16_t>(instrument);
}

// Valid filler symbols: letters and digits only, and no separator that
// normalisation would strip. "SYM_1" would collapse to "SYM1" and collide with
// the symbol generated for i == 1.
std::string Filler(size_t i) {
    return "SYM" + std::to_string(i);
}

}  // namespace

// --- normalisation ----------------------------------------------------------

TEST(NormalizeSymbolTest, UppercasesAndStripsKnownSeparators) {
    EXPECT_EQ(NormalizeSymbol("btcusdt"), "BTCUSDT");
    EXPECT_EQ(NormalizeSymbol("BTC-USDT"), "BTCUSDT");
    EXPECT_EQ(NormalizeSymbol("btc-usdt"), "BTCUSDT");
    EXPECT_EQ(NormalizeSymbol("BTC/USDT"), "BTCUSDT");
    EXPECT_EQ(NormalizeSymbol("btc_usdt"), "BTCUSDT");
    EXPECT_EQ(NormalizeSymbol("  BTC USDT  "), "BTCUSDT");
}

TEST(NormalizeSymbolTest, LeavesUnknownCharactersInPlace) {
    // the point of NOT stripping these is that they then fail validation.
    // If normalisation deleted anything it did not recognise, "BTC.USDT" and
    // "BTCUSDT" would become the same symbol - which is how a client asks for
    // one instrument and quietly receives another.
    EXPECT_EQ(NormalizeSymbol("BTC.USDT"), "BTC.USDT");
    EXPECT_EQ(NormalizeSymbol("BTC#USDT"), "BTC#USDT");
}

TEST(NormalizeSymbolTest, KeepsDigits) {
    // Real symbol: Binance lists 1000SATSUSDT. Stripping digits, or rejecting
    // a leading one, would make it unconfigurable.
    EXPECT_EQ(NormalizeSymbol("1000satsusdt"), "1000SATSUSDT");
}

// --- validation -------------------------------------------------------------

TEST(ValidateSymbolTest, AcceptsLettersAndDigits) {
    EXPECT_EQ(ValidateSymbol("BTCUSDT"), SymbolStatus::kOk);
    EXPECT_EQ(ValidateSymbol("1000SATSUSDT"), SymbolStatus::kOk);
}

TEST(ValidateSymbolTest, ReportsWhyASymbolWasRejected) {
    // The reason matters, not just the rejection: the config loader turns it
    // into a message naming the offending entry. A bare bool would leave the
    // operator with "bad config" and nothing to act on.
    EXPECT_EQ(ValidateSymbol(NormalizeSymbol("")), SymbolStatus::kEmpty);
    EXPECT_EQ(ValidateSymbol(NormalizeSymbol("///")), SymbolStatus::kEmpty);
    EXPECT_EQ(ValidateSymbol(NormalizeSymbol("   ")), SymbolStatus::kEmpty);
    EXPECT_EQ(ValidateSymbol(NormalizeSymbol("BTC.USDT")), SymbolStatus::kBadCharacter);
    EXPECT_EQ(ValidateSymbol(NormalizeSymbol("BTC USD$")), SymbolStatus::kBadCharacter);
}

TEST(ValidateSymbolTest, EveryStatusHasADistinctDescription) {
    EXPECT_EQ(DescribeSymbolStatus(SymbolStatus::kOk), "ok");
    EXPECT_NE(DescribeSymbolStatus(SymbolStatus::kEmpty), DescribeSymbolStatus(SymbolStatus::kOk));
    EXPECT_NE(DescribeSymbolStatus(SymbolStatus::kBadCharacter), DescribeSymbolStatus(SymbolStatus::kEmpty));
    EXPECT_FALSE(DescribeSymbolStatus(SymbolStatus::kBadCharacter).empty());
}

// --- empty ------------------------------------------------------------------

TEST(InstrumentRegistryTest, StartsEmpty) {
    const InstrumentRegistry registry;
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_TRUE(registry.empty());
    EXPECT_FALSE(registry.Find(kBtc).has_value());
}

// --- assignment -------------------------------------------------------------

TEST(InstrumentRegistryTest, AssignsDenseIdsInRegistrationOrder) {
    InstrumentRegistry registry;

    const auto btc = registry.Register(kBtc);
    const auto eth = registry.Register(kEth);
    const auto sol = registry.Register(kSol);

    ASSERT_TRUE(btc.has_value());
    ASSERT_TRUE(eth.has_value());
    ASSERT_TRUE(sol.has_value());

    EXPECT_EQ(Raw(*btc), 0u);
    EXPECT_EQ(Raw(*eth), 1u);
    EXPECT_EQ(Raw(*sol), 2u);
    EXPECT_EQ(registry.size(), 3u);
}

TEST(InstrumentRegistryTest, IdsFollowConfigOrderNotEnumOrder) {
    InstrumentRegistry registry;

    // Deliberately NOT the order venue.h declares. Today the enum gives
    // BTCUSDT=0, ETHUSDT=1, SOLUSDT=2, and a config listing them in that order
    // makes id == enumerator - exactly the coincidence that would hide a mix-up
    // during the migration. Registering backwards proves nothing here depends
    // on it.
    const auto sol = registry.Register(kSol);
    const auto eth = registry.Register(kEth);
    const auto btc = registry.Register(kBtc);

    ASSERT_TRUE(sol.has_value());
    ASSERT_TRUE(eth.has_value());
    ASSERT_TRUE(btc.has_value());

    EXPECT_EQ(Raw(*sol), 0u);
    EXPECT_EQ(Raw(*eth), 1u);
    EXPECT_EQ(Raw(*btc), 2u);
}

// --- idempotence ------------------------------------------------------------

TEST(InstrumentRegistryTest, RegisterIsIdempotent) {
    InstrumentRegistry registry;

    const auto first = registry.Register(kBtc);
    const auto second = registry.Register(kBtc);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(InstrumentRegistryTest, OneSymbolAcrossTwoMarketsConsumesOneId) {
    InstrumentRegistry registry;

    // A config entry of {"symbol": "BTCUSDT", "market": ["spot", "futures"]}
    // registers the symbol once per market. It must yield ONE id: market type
    // is not part of the symbol, it lives in InstrumentKey. The two markets
    // separate there, as two keys over one symbol id, and therefore land in two
    // different books that can never meet in a merge (venue.h).
    const auto spot = registry.Register(kBtc);
    const auto futures = registry.Register(kBtc);

    ASSERT_TRUE(spot.has_value());
    ASSERT_TRUE(futures.has_value());
    EXPECT_EQ(*spot, *futures);
    EXPECT_EQ(registry.size(), 1u);

    // And the two keys built from that one id are genuinely distinct.
    EXPECT_NE(MakeKey(*spot, MarketType::kSpot), MakeKey(*futures, MarketType::kFutures));
}

TEST(InstrumentRegistryTest, DifferentSpellingsOfOneSymbolShareAnId) {
    InstrumentRegistry registry;

    // This is the whole reason normalisation lives INSIDE the registry rather
    // than in each caller: an operator writing "BTC-USDT" in the config and a
    // client requesting "btcusdt" over gRPC must reach the same book.
    const auto canonical = registry.Register(kBtc);
    ASSERT_TRUE(canonical.has_value());

    EXPECT_EQ(registry.Register("btc-usdt"), canonical);
    EXPECT_EQ(registry.Register("BTC/USDT"), canonical);
    EXPECT_EQ(registry.Register("btc_usdt"), canonical);
    EXPECT_EQ(registry.size(), 1u);
}

// --- rejection --------------------------------------------------------------

TEST(InstrumentRegistryTest, RejectsInvalidSymbolsWithoutConsumingAnId) {
    InstrumentRegistry registry;
    ASSERT_TRUE(registry.Register(kBtc).has_value());

    EXPECT_FALSE(registry.Register("").has_value());
    EXPECT_FALSE(registry.Register("---").has_value());
    EXPECT_FALSE(registry.Register("BTC.USDT").has_value());

    // A rejected symbol must leave no trace. Bumping the counter and leaving
    // the name empty is the failure this guards: every later id would shift,
    // and Name() would hand a log line an empty symbol.
    EXPECT_EQ(registry.size(), 1u);
    EXPECT_EQ(Raw(*registry.Register(kEth)), 1u);
}

TEST(InstrumentRegistryTest, InvalidSymbolIsDistinguishableFromAFullRegistry) {
    InstrumentRegistry registry;

    // Both failures return nullopt, so the caller tells them apart by
    // validating first. This is the contract the config loader relies on to
    // print "symbol X is invalid" rather than "too many symbols".
    ASSERT_EQ(ValidateSymbol(NormalizeSymbol("BTC.USDT")), SymbolStatus::kBadCharacter);
    EXPECT_FALSE(registry.Register("BTC.USDT").has_value());
    EXPECT_LT(registry.size(), kMaxInstruments);

    for (size_t i = 0; i < kMaxInstruments; ++i) {
        ASSERT_TRUE(registry.Register(Filler(i)).has_value()) << "failed at " << i;
    }
    ASSERT_EQ(ValidateSymbol(NormalizeSymbol("ONETOOMANY")), SymbolStatus::kOk);
    EXPECT_FALSE(registry.Register("ONETOOMANY").has_value());
    EXPECT_EQ(registry.size(), kMaxInstruments);
}

// --- lookup -----------------------------------------------------------------

TEST(InstrumentRegistryTest, FindReturnsTheRegisteredId) {
    InstrumentRegistry registry;

    const auto btc = registry.Register(kBtc);
    const auto eth = registry.Register(kEth);
    ASSERT_TRUE(btc.has_value());
    ASSERT_TRUE(eth.has_value());

    EXPECT_EQ(registry.Find(kBtc), btc);
    EXPECT_EQ(registry.Find(kEth), eth);
    EXPECT_FALSE(registry.Find(kSol).has_value());
}

TEST(InstrumentRegistryTest, FindNormalisesItsArgument) {
    InstrumentRegistry registry;
    const auto btc = registry.Register(kBtc);
    ASSERT_TRUE(btc.has_value());

    // Unlike VenueRegistry, where matching is exact because the name arrives
    // from a remote process and a near-miss must stay visible. Here the string
    // comes from an operator's config or a client's Subscribe request, so
    // accepting the spelling they used is the requirement, not a hazard.
    EXPECT_EQ(registry.Find("btcusdt"), btc);
    EXPECT_EQ(registry.Find("BTC-USDT"), btc);
    EXPECT_EQ(registry.Find("  btc/usdt "), btc);

    // Tolerant, not sloppy: a genuinely different symbol still misses.
    EXPECT_FALSE(registry.Find("BTCUSD").has_value());
    EXPECT_FALSE(registry.Find("BTC.USDT").has_value());
    EXPECT_FALSE(registry.Find("").has_value());
}

TEST(InstrumentRegistryTest, NameRoundTripsToTheCanonicalForm) {
    InstrumentRegistry registry;

    // Registered in a sloppy spelling, stored canonical. Name() feeds the
    // symbol field of every published Update, so what goes on the wire must be
    // the canonical form regardless of how the config was written.
    const auto btc = registry.Register("btc-usdt");
    const auto eth = registry.Register("ETH/USDT");
    ASSERT_TRUE(btc.has_value());
    ASSERT_TRUE(eth.has_value());

    EXPECT_EQ(registry.Name(*btc), kBtc);
    EXPECT_EQ(registry.Name(*eth), kEth);
}

TEST(InstrumentRegistryTest, NameOfUnregisteredIdIsEmpty) {
    InstrumentRegistry registry;
    ASSERT_TRUE(registry.Register(kBtc).has_value());

    // Id 1 exists as storage but has been assigned to nothing. Empty rather
    // than "UNKNOWN": an id can only come from Register, so reaching here is a
    // bug, and an empty symbol on the wire is a loud one.
    EXPECT_TRUE(registry.Name(static_cast<InstrumentId>(1)).empty());
    EXPECT_TRUE(registry.Name(static_cast<InstrumentId>(kMaxInstruments - 1)).empty());
}

TEST(InstrumentRegistryTest, NameStaysValidAcrossLaterRegistrations) {
    InstrumentRegistry registry;

    const auto btc = registry.Register(kBtc);
    ASSERT_TRUE(btc.has_value());
    const std::string_view name = registry.Name(*btc);

    // this is the test the fixed-capacity array exists for. "BTCUSDT" is
    // 7 characters, so it lives INSIDE the std::string object (SSO), not on the
    // heap. With a std::vector<std::string> the growth below would move those
    // objects and leave `name` pointing at freed storage - a use-after-free
    // that only appears once enough symbols are configured, which is the worst
    // possible time to find it.
    //
    // Bounded by kMaxInstruments rather than a literal, so filling to exactly
    // capacity stays the strongest version of this test if the cap changes.
    for (size_t i = 1; i < kMaxInstruments; ++i) {
        ASSERT_TRUE(registry.Register(Filler(i)).has_value());
    }
    EXPECT_EQ(registry.size(), kMaxInstruments);
    EXPECT_EQ(name, kBtc);
}

// --- capacity ---------------------------------------------------------------

TEST(InstrumentRegistryTest, FillsToCapacity) {
    InstrumentRegistry registry;

    for (size_t i = 0; i < kMaxInstruments; ++i) {
        const auto id = registry.Register(Filler(i));
        ASSERT_TRUE(id.has_value()) << "failed at " << i;
        EXPECT_EQ(Raw(*id), i);
    }
    EXPECT_EQ(registry.size(), kMaxInstruments);
}

TEST(InstrumentRegistryTest, FullRegistryStillResolvesKnownSymbols) {
    InstrumentRegistry registry;
    for (size_t i = 0; i < kMaxInstruments; ++i) {
        ASSERT_TRUE(registry.Register(Filler(i)).has_value());
    }
    ASSERT_FALSE(registry.Register("ONETOOMANY").has_value());

    // Being full must not break lookups. A rejected registration is the one
    // path where a half-written slot would be easiest to leave behind.
    const auto first = registry.Find(Filler(0));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(Raw(*first), 0u);
    EXPECT_EQ(registry.Name(*first), Filler(0));
    EXPECT_FALSE(registry.Find("ONETOOMANY").has_value());
}

// --- publication ------------------------------------------------------------

TEST(InstrumentRegistryTest, ReaderNeverObservesAHalfPublishedName) {
    InstrumentRegistry registry;
    std::atomic<bool> start{false};
    std::atomic<bool> torn{false};

    // Single-writer / many-reader, the same contract as VenueRegistry. The
    // invariant under test is the release/acquire pair in Register/size():
    // every id a reader can SEE via size() must already have its name written.
    //
    // this test cannot prove the ordering is correct - a race that never
    // fires is indistinguishable from one that cannot fire. It can only catch
    // the ordering being absent, and only sometimes. It is here because a
    // relaxed store would fail it often enough to be worth the milliseconds,
    // not because passing it is a proof.
    std::thread reader([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int pass = 0; pass < 10000; ++pass) {
            const size_t count = registry.size();
            for (size_t i = 0; i < count; ++i) {
                if (registry.Name(static_cast<InstrumentId>(i)).empty()) {
                    torn.store(true, std::memory_order_relaxed);
                }
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (size_t i = 0; i < kMaxInstruments; ++i) {
        ASSERT_TRUE(registry.Register(Filler(i)).has_value());
    }
    reader.join();

    EXPECT_FALSE(torn.load(std::memory_order_relaxed));
    EXPECT_EQ(registry.size(), kMaxInstruments);
}
