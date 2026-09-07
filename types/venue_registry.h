#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Runtime venue identity.
//
// `VenueId` in venue.h is a compile-time enum, and `kVenueCount` sizes every
// per-venue array in md_core. That means adding a venue is a RECOMPILE of the
// core, because it turns "add a venue"
// into "restart everything and gap every client".
//
// This registry replaces the enum as md_core's notion of a venue. The core
// stores an opaque VenueSlot; it never learns the string "binance". The name
// travels only as far as attribution and logging.
//
// Nothing here changes behaviour yet. venue.h and kVenueCount stay exactly as
// they are, and the arrays in md_core migrate one file at a time.
// ---------------------------------------------------------------------------

// Upper bound on venues known at once. Fixed CAPACITY, runtime SIZE.
//
// this is what buys the whole design. A std::vector would also be
// runtime-sized, but growing it reallocates - and the merge path reads these
// arrays from another thread, so a reallocation would invalidate references
// underneath a live reader. A fixed array cannot move, so registration is
// safe while the consolidator runs.
//
// 8 is a deliberate bound, not a guess at some limit: more venues than a
// consolidated crypto book has any use for, and small enough that the arrays
// it sizes stay cache-friendly.
inline constexpr size_t kMaxVenues = 8;

// Dense index into md_core's per-venue arrays, assigned at registration.
//
// a strong type, not a typedef for uint8_t. During the migration a
// VenueId and a VenueSlot are both small integers and - because venues are
// registered in enum order today - they hold the SAME values. That is exactly
// when a typedef stops protecting you: every mix-up compiles and does the
// right thing until the day a venue is registered out of order, at which
// point prices are attributed to the wrong exchange. An enum class makes the
// compiler refuse the mix-up instead.
enum class VenueSlot : uint8_t {
};

// Slot -> array index. The one place the underlying integer is exposed.
constexpr size_t VenueSlotIndex(VenueSlot slot) {
    return static_cast<size_t>(slot);
}

// Maps venue names to dense slots.
//
// Names, not VenueIds, because that is what the wire carries: in the split
// topology a provider announces itself in the kHello handshake and
// md_core never sees an enum. Keying on the name now means this does not have
// to be redone when the process split lands.
//
// Matching is EXACT and case-sensitive. Callers normalise - today that is
// VenueConverter::ToVenueString, which yields "BINANCE", "BYBIT", "OKX".
//
// THREADING: single writer, many readers. One thread calls Register (startup
// today; the socket-accept thread once providers dial in). Any number
// of threads may call Find/Name/size concurrently with it. Two threads calling
// Register at the same time is not supported and is not needed - registration
// happens on exactly one control path.
class VenueRegistry {
   public:
    VenueRegistry() = default;

    // Returns the slot for `name`, assigning a new one if it is unknown.
    //
    // Idempotent: registering the same name twice yields the same slot, so a
    // provider reconnecting after a crash lands back on the books it had.
    //
    // Returns nullopt only when the registry is full. That is a configuration
    // error, not a runtime condition, and the caller should refuse the
    // connection loudly rather than silently dropping a venue from the merge.
    std::optional<VenueSlot> Register(std::string_view name) {
        if (const std::optional<VenueSlot> existing = Find(name); existing.has_value()) {
            return existing;
        }

        const size_t count = size_.load(std::memory_order_relaxed);
        if (count >= kMaxVenues) {
            return std::nullopt;
        }

        names_[count].assign(name);

        // release store, paired with the acquire load in size(). The name
        // above is written BEFORE the counter is bumped, so any reader that
        // observes the new size is guaranteed to see the fully written slot.
        // Publishing the counter first would let a reader index a slot whose
        // name is still empty - a torn read of state that was never invalid.
        size_.store(count + 1, std::memory_order_release);
        return static_cast<VenueSlot>(count);
    }

    std::optional<VenueSlot> Find(std::string_view name) const {
        const size_t count = size();
        for (size_t i = 0; i < count; ++i) {
            if (names_[i] == name) {
                return static_cast<VenueSlot>(i);
            }
        }
        return std::nullopt;
    }

    // Attribution and logging only. Never a hot-path lookup key - the slot is.
    //
    // The returned view stays valid for the registry's lifetime: slots are
    // never reused or moved, so the string it points at is never reassigned.
    std::string_view Name(VenueSlot slot) const {
        const size_t index = VenueSlotIndex(slot);
        if (index >= size()) {
            return {};
        }
        return names_[index];
    }

    // Number of registered venues. Per-venue loops in md_core run to this
    // instead of to kVenueCount - which is the entire point of the class.
    size_t size() const { return size_.load(std::memory_order_acquire); }

    bool empty() const { return size() == 0; }

   private:
    // std::string, not string_view: the registry owns the names. A view would
    // point at whatever the caller passed - a parsed wire buffer that is
    // recycled on the next message.
    std::array<std::string, kMaxVenues> names_{};

    // Atomic because it is the publication point for names_ (see Register).
    std::atomic<size_t> size_{0};
};
