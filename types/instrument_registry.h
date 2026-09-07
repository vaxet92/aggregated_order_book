#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "types/venue.h"

// ---------------------------------------------------------------------------
// Runtime instrument identity.
//
// `InstrumentId` in venue.h is a compile-time enum, so the set of tradable
// symbols is fixed when the binary is built. Adding a symbol means editing two
// hand-written lookups in a header and recompiling - in Docker, rebuilding the
// image. A config file cannot help, because no file read at runtime can add an
// enumerator.
//
// This registry replaces the enumerators. Ids are handed out at load time, in
// the order the config lists the symbols, so the symbol universe comes from
// JSON instead of from source.
//
// It is deliberately the same design as VenueRegistry (venue_registry.h), for
// the same reason: one threading story and one lifetime story to explain
// instead of two.
//
// this costs nothing on the hot path. InstrumentId is a uint16_t either
// way - InstrumentKey's bit layout, BookUpdate's POD-ness in the SPSC ring and
// the identity hash are all untouched. The only thing that moves is WHERE the
// string <-> id table lives: out of a switch in a header, into an array built
// once at startup.
//
// ids are PROCESS-LOCAL. They depend on config order, so two aggregators
// with differently ordered configs disagree about which id is BTCUSDT. That is
// safe only because no id ever leaves the process - the gRPC wire carries the
// symbol string (aggregator.proto). The day an id goes on the wire or into a
// file, this breaks.
//
// Nothing here changes behaviour yet. venue.h keeps its enumerators and every
// existing call site still compiles; they migrate in a later step.
//
// during that migration a registry id and an InstrumentId enumerator are
// both small integers, and - because the config lists BTCUSDT first - they hold
// the SAME value. That is exactly when a mix-up compiles and does the right
// thing right up until the config is reordered. Same trap VenueRegistry calls
// out for VenueId vs VenueSlot.
// ---------------------------------------------------------------------------

// Upper bound on symbols known at once. Fixed CAPACITY, runtime SIZE.
//
// This is a bound on THIS process's name table, not a domain limit: a real
// symbol universe is thousands, and would size a reserved vector from the
// config instead. 64 is far more than this project subscribes to, and holding
// the names in a fixed array is what keeps Name()'s views valid (see Name).
inline constexpr size_t kMaxInstruments = 64;

// Canonical symbol form: uppercase, with separators removed.
//
//   "btc-usdt" -> "BTCUSDT"
//   "BTC/USDT" -> "BTCUSDT"
//   "btcusdt"  -> "BTCUSDT"
//
// only KNOWN separators are removed - '-', '_', '/' and ASCII whitespace.
// Anything else is left in place so it fails validation rather than being
// silently deleted. Deleting an unexpected character would let two different
// strings normalise to one symbol, which is how a client subscribing to one
// instrument quietly receives another.
//
// Allocates. Called once per instrument at startup and once per Subscribe
// request - never on the message path.
inline std::string NormalizeSymbol(std::string_view symbol) {
    std::string normalized;
    normalized.reserve(symbol.size());
    for (const char c : symbol) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc == '-' || uc == '_' || uc == '/' || std::isspace(uc) != 0) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::toupper(uc)));
    }
    return normalized;
}

// Why a symbol was rejected, so the caller can report WHICH problem it hit
// rather than only that something failed.
//
// the registry does not log this itself. It cannot write a useful
// message - by the time it validates, it holds only the normalised form, not
// the raw string as the operator typed it, and it knows nothing about which
// config file or which entry the symbol came from. The loader has all three,
// so the reason travels back to it and the message is written there. That also
// keeps `types` free of a dependency on the logger and on fmt.
enum class SymbolStatus : uint8_t {
    kOk,
    kEmpty,         // nothing survived normalisation - "" or "///"
    kBadCharacter,  // something that is not A-Z or 0-9 survived normalisation
};

// No `default:` label on purpose. Adding an enumerator without handling it
// here is then a compiler warning instead of a silent "unknown" at runtime -
// the exhaustiveness check the old ToInstrumentString switch gave up when it
// added `default: return "UNKNOWN"`. The trailing return only exists because
// a caller may cast an out-of-range value in.
inline std::string_view DescribeSymbolStatus(SymbolStatus status) {
    switch (status) {
        case SymbolStatus::kOk:
            return "ok";
        case SymbolStatus::kEmpty:
            return "empty after normalisation";
        case SymbolStatus::kBadCharacter:
            return "contains a character that is not a letter or a digit";
    }
    return "unrecognised status";
}

// A normalised symbol must be non-empty and contain only A-Z and 0-9.
//
// digits are allowed because they are real, not for tolerance. Binance
// lists 1000SATSUSDT and 1000PEPEUSDT, where the leading number is part of the
// symbol - it is the contract multiplier, not a typo. Rejecting digits would
// make those instruments impossible to configure.
inline SymbolStatus ValidateSymbol(std::string_view normalized) {
    if (normalized.empty()) {
        return SymbolStatus::kEmpty;
    }
    const bool all_alnum = std::all_of(normalized.begin(), normalized.end(), [](char c) {
        const auto uc = static_cast<unsigned char>(c);
        return (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9');
    });
    return all_alnum ? SymbolStatus::kOk : SymbolStatus::kBadCharacter;
}

// Maps canonical symbol names to dense ids.
//
// THREADING: single writer, many readers - the same contract as VenueRegistry.
// One thread calls Register; any number may call Find/Name/size concurrently
// with it. Two threads calling Register at once is not supported and not
// needed.
//
// Today every Register runs in main() before a single thread starts, so the
// memory ordering below is not yet load-bearing. It is here because
// Core::AddInstrument already exists as a runtime entry point, and because the
// alternative is explaining why the two registries in this project publish
// differently.
class InstrumentRegistry {
   public:
    InstrumentRegistry() = default;

    // Returns the id for `symbol`, assigning a new one if it is unknown.
    //
    // The symbol is normalised first, so "btc-usdt" and "BTCUSDT" name the
    // same instrument and get the same id.
    //
    // Idempotent: registering the same symbol twice yields the same id, so a
    // config listing BTCUSDT under both spot and futures consumes one id, not
    // two. Market type is not part of the symbol - it lives in InstrumentKey.
    //
    // nullopt means one of two configuration errors, and the caller can tell
    // them apart: run ValidateSymbol(NormalizeSymbol(symbol)) first and report
    // DescribeSymbolStatus when it is not kOk. If validation passed and this
    // still returns nullopt, the registry is full - size() == kMaxInstruments.
    //
    // Both are caught at startup, and the caller should refuse to start and
    // name the symbol rather than run with it silently missing.
    std::optional<InstrumentId> Register(std::string_view symbol) {
        const std::string normalized = NormalizeSymbol(symbol);
        if (ValidateSymbol(normalized) != SymbolStatus::kOk) {
            return std::nullopt;
        }
        if (const std::optional<InstrumentId> existing = FindNormalized(normalized); existing.has_value()) {
            return existing;
        }

        const size_t count = size_.load(std::memory_order_relaxed);
        if (count >= kMaxInstruments) {
            return std::nullopt;
        }

        names_[count] = normalized;

        // release store, paired with the acquire load in size(). The name
        // is written BEFORE the counter is bumped, so any reader that observes
        // the new size is guaranteed to see the fully written name. Publishing
        // the counter first would let a reader index a slot that is still an
        // empty string - a torn read of state that was never invalid.
        size_.store(count + 1, std::memory_order_release);
        return static_cast<InstrumentId>(count);
    }

    // Normalises, so "btc-usdt", "BTC/USDT" and "BTCUSDT" all resolve.
    std::optional<InstrumentId> Find(std::string_view symbol) const { return FindNormalized(NormalizeSymbol(symbol)); }

    // Wire attribution and logging only. Never a lookup key - the id is.
    //
    // The returned view stays valid for the registry's lifetime: ids are never
    // reused, and names_ is a fixed array, so the string it points at is never
    // reassigned and never moves.
    //
    // the fixed array is what makes that true. A std::vector<std::string>
    // would reallocate as it grew, and "BTCUSDT" is 7 characters - short enough
    // to live INSIDE the std::string object (SSO) rather than on the heap. Every
    // view into an SSO string would dangle the moment the vector grew, which is
    // a use-after-free that only appears once enough symbols are configured.
    //
    // An unknown id returns an EMPTY view, not "UNKNOWN". Ids only come from
    // Register, so this cannot happen in correct code - and if it ever does, an
    // empty symbol on the wire is a loud bug, while "UNKNOWN" is a quiet one.
    std::string_view Name(InstrumentId instrument) const {
        const auto index = static_cast<size_t>(instrument);
        if (index >= size()) {
            return {};
        }
        return names_[index];
    }

    size_t size() const { return size_.load(std::memory_order_acquire); }

    bool empty() const { return size() == 0; }

   private:
    // Linear scan, like VenueRegistry::Find. Lookups happen at startup and once
    // per Subscribe call - never per message - so a hash map would add a second
    // data structure to keep in sync for no measurable gain.
    std::optional<InstrumentId> FindNormalized(std::string_view normalized) const {
        const size_t count = size();
        for (size_t i = 0; i < count; ++i) {
            if (names_[i] == normalized) {
                return static_cast<InstrumentId>(i);
            }
        }
        return std::nullopt;
    }

    // std::string, not string_view: the registry owns the names. A view would
    // point at the parsed config buffer, which is gone once startup finishes.
    std::array<std::string, kMaxInstruments> names_{};

    // Atomic because it is the publication point for names_ (see Register).
    std::atomic<size_t> size_{0};
};
