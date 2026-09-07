
#pragma once

// Core: owns every venue's FlatOrderBook and the per-instrument
// consolidated::State, drains the per-venue SPSC queues on one consolidator
// thread with no lock on the book path, and fires BboCallback/BookCallback on
// every accepted update. Venue lifecycle (RegisterVenue/RemoveVenue),
// staleness admission (OnVenueHealth) and the direct (test-only) vs. queued
// (production) apply paths all live here. This is md_core's top-level type -
// everything else in the library exists to be owned or called by it.

#include "types/venue.h"
#include "types/venue_registry.h"
#include "provider_message.h"
#include "flat_order_book.h"
#include "consolidated_bbo.h"
#include "consolidated_book.h"
#include "consolidated_state.h"
#include "types.h"
#include "venue_health.h"
#include "logger/logger.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace market_data {

// Keyed by InstrumentKey, so spot and futures for the same symbol are
// different entries and their books can never be merged together.
// InstrumentKeyHash is the identity on the packed uint32_t.
//
// this typedef and the two make_unique calls in md_core.cpp are the ENTIRE
// coupling between Core and the book implementation. Everything else Core does
// goes through ApplyUpdate / BestBid / BestAsk / bids(), which both types
// provide identically - which is what made swapping the implementation a
// three-line change rather than a rewrite.
using InstrumentBooks = std::unordered_map<InstrumentKey, FlatBookArray, InstrumentKeyHash>;
using InstrumentQuotes = std::unordered_map<InstrumentKey, VenueQuoteArray, InstrumentKeyHash>;
using InstrumentBbo = std::unordered_map<InstrumentKey, consolidated::BBO, InstrumentKeyHash>;

class Core {
   public:
    using BboCallback = std::function<void(InstrumentKey, const consolidated::BBO&)>;

    // Fired after every depth update with a fresh, immutable merged book.
    // shared_ptr, not a reference: fan-out to N subscribers is N refcount
    // bumps, and the pointer stays valid for whatever the callback does with
    // it after Core moves on to the next update. Core knows nothing about
    // bands or clients - it only produces the merged book; deciding which
    // bands to compute from it, for whom, is the subscriber's job (the
    // aggregator service), which is what makes per-client custom thresholds
    // possible without Core knowing about clients at all.
    // a CONST REFERENCE, not a shared_ptr, and the difference is a
    // contract: THE BOOK IS VALID ONLY FOR THE DURATION OF THE CALL. The
    // consolidator thread resumes mutating it on the very next update, so a
    // consumer that wants to keep it must copy it.
    //
    // This replaced an 8.2 us snapshot copy per publish - 98% of what
    // publishing cost once the incremental merge had brought the merge itself
    // down to 167 ns. The copy bought a retention guarantee that was verified,
    // caller by caller, to be used by NO production consumer: PublishBook
    // computes its bands synchronously and drops the book before it returns,
    // and no DEPTH feed exists to want raw levels.
    //
    // The guarantee comes back the moment an asynchronous consumer does - and
    // the answer then is versioned buffers caught up by replaying LevelChange,
    // not a return to copying the whole book on every update.
    using BookCallback = std::function<void(InstrumentKey, const consolidated::Book&)>;

    explicit Core(BboCallback bbo_callback, BookCallback book_callback);

    // Joins the consolidator thread. NOT defaulted: a thread still running
    // when Core is destroyed would touch freed members, and std::thread's own
    // destructor calls std::terminate on a joinable thread.
    ~Core();

    void Init(const CoreConfig& config);

    // --- venue lifecycle ---------------------------------
    //
    // venues appear because a provider APPEARS, never because config
    // named one. Init allocates capacity and registers nothing. Registering
    // from config would create slots for venues that may never connect, and
    // registering lazily on the first update would be creating state from a
    // data message - which forbids, because an update arriving just
    // after a venue was removed would silently resurrect it.
    //
    // The synchronous registration path. Only the tests call it now; in the
    // deployed split, a provider connecting IS the kHello and ZmqCoreIngress
    // takes the RegisterVenueName + EnqueueRegistration path below instead
    // (accept is registration). Only the CALLER differs between the two
    // and nothing inside Core does - which is why the registry is keyed on the
    // venue name rather than on VenueId.

    // Assigns `name` a slot, and creates that venue's FlatOrderBook for every
    // instrument that already exists. Idempotent: a provider that crashed and
    // reconnected gets the SAME slot back, so it resumes the books it had
    // rather than stranding them under a slot nobody feeds.
    //
    // Returns nullopt only when the registry is full (more than kMaxVenues
    // distinct venues). That is a configuration error, not a runtime
    // condition; the caller must log it and refuse, because a venue silently
    // missing from the merge is exactly the failure this design exists to
    // avoid.
    //
    // NOT THREAD-SAFE against a running consolidator: it allocates the
    // per-venue FlatOrderBooks the merge path reads with no lock. Safe only
    // pre-Start(), which is why only the tests use it. The socket-ingress path
    // uses RegisterVenueName + EnqueueRegistration instead - see.
    std::optional<VenueSlot> RegisterVenue(std::string_view name);

    // The half of RegisterVenue that is safe to call from the socket-ingress
    // thread while the consolidator runs: it validates the name and assigns a
    // slot in VenueRegistry (single-writer / many-reader, release/acquire) and
    // does NOTHING else. It does not create books, touch active_venues_ or
    // touch venue_id_to_slot_ - that is ProcessRegistration's job, and it runs
    // on the consolidator thread when the matching VenueRegistration control
    // message is drained.
    //
    // Same nullopt contract as RegisterVenue: unknown name, or registry full.
    // The caller must then push EnqueueRegistration(slot) so the consolidator
    // activates the slot in order with that venue's data.
    std::optional<VenueSlot> RegisterVenueName(std::string_view name);

    // Takes a venue out of service: frees its FlatOrderBook for every instrument
    // and resets both stream verdicts to kNoData, so the next merge simply
    // does not see it. Instruments fed by other venues keep publishing, one
    // venue thinner - which is already the correct behaviour and already what
    // the health path does.
    //
    // this DEACTIVATES the slot; it does not release it. The name -> slot
    // mapping and venue_count() are untouched. Releasing the slot would give a
    // reconnecting provider a different one, and slot ids are held elsewhere -
    // in-flight updates, published attribution - so the venue's data would be
    // attributed to whatever else later occupied that slot. Retaining it is
    // what makes RegisterVenue's idempotence meaningful.
    //
    // Takes a VenueSlot rather than a name: the caller holds the token
    // RegisterVenue returned, so a venue that was never registered cannot be
    // removed by passing a string that happens to look right.
    void RemoveVenue(VenueSlot slot);

    // --- direct path: NOT THREAD-SAFE ---------------------------------------
    //
    // these apply immediately, on the calling thread, with NO lock. They
    // are safe only when one thread calls them and the consolidator is not
    // running. Production does not use them - ZmqCoreIngress calls Enqueue*
    // instead (no lock anywhere on the book path). They remain because the tests
    // use them as the ORACLE the queued path is compared against, which is what
    // makes "the two paths cannot diverge" a checked claim.
    //
    // Calling these while Start() is active is a data race.
    //
    // Depth path: applies the update to the per-venue book, then EAGERLY
    // rebuilds the full k-way merge across all configured venues (DESIGN
    // ) and fires book_callback_ with the result - on every single
    // update, immediately, no throttle timer (same eager-publish decision
    // already made for BBO). This is provisional pending a real benchmark
    // if depth update rate ever outpaces the merge cost, the
    // fallback is throttling the merge to a fixed cadence instead of firing
    // on every update. Does NOT publish the BBO - the fast-BBO stream does
    // that (see ApplyQuote), a separate trigger at a separate rate; the two
    // are never combined into one callback (- never mix the streams).
    void ApplyUpdate(const BookUpdate& update);

    // Fast-BBO path - this is what drives the
    // published BBO. Never touches FlatOrderBook: the two streams are not
    // mutually sequenced, so mixing them corrupts the book.
    void ApplyQuote(const BboQuote& quote);

    // --- queued path -----------------------
    //
    // The producer side. Called on a provider's own thread; takes no lock,
    // resolves nothing, and returns as soon as the message is in that
    // venue's ring.
    //
    // these take a VenueSlot, not a VenueId. The caller resolved it once
    // when the venue registered and holds it for the life of the connection
    // (- accept is registration), so the hot path does no translation
    // and cannot race RegisterVenue/RemoveVenue writing venue_id_to_slot_.
    // That race is exactly what removing the mutex would otherwise expose.
    //
    // these NEVER block indefinitely. A market-data producer cannot
    // apply backpressure to its source - blocking a provider's io_context
    // thread stops it draining its sockets, the kernel receive buffer fills,
    // and the exchange either disconnects us or we read nothing but stale
    // messages. Blocking converts a bounded queue problem into an unbounded
    // staleness problem, and hides it. Each call makes a bounded attempt
    // (kEnqueueSpinAttempts) and then applies the policy below.
    //
    // the overflow policy differs by message type, because what is safe
    // to lose differs by message type:
    //
    //   BookUpdate       - returns false. A diff is one link in a sequenced
    //                      chain; losing one makes the book untrustworthy
    //                      from that point on. The CALLER must resync. Core
    //                      does not resync on its own: continuity and
    //  resynchronization belong to the provider (
    // ), which already owns RequestResync.
    //
    //   BboQuote         - dropped, and counted. A quote is a COMPLETE
    //                      top-of-book snapshot, not a delta, so a newer one
    //                      supersedes it entirely. This is the same
    //                      conflation ConflatedChannel does for clients,
    //                      applied at ingest. Returns void: there is nothing
    //                      the caller should do about it.
    //
    //   VenueHealthEvent - returns false. A state TRANSITION, not a sample:
    //                      a lost kStale means Core keeps merging a dead
    //                      venue forever. Escalates the same way as a lost
    //                      diff, and resync re-announces health anyway.
    //
    // Nothing is ever dropped silently - a drop is either counted (quotes)
    // or reported to the caller (everything else).
    //
    // PRECONDITION: something must be draining, or every queue fills and
    // every venue ends up resyncing. Until Start() spawns the consolidator
    // thread that means the caller must call DrainOnce().
    bool EnqueueUpdate(VenueSlot slot, BookUpdate update);
    void EnqueueQuote(VenueSlot slot, BboQuote quote);
    bool EnqueueHealth(VenueSlot slot, VenueHealthEvent event);

    // Venue lifecycle, queued. The socket-ingress
    // thread calls RegisterVenueName first (slot assignment only), then pushes
    // one of these into that slot's queue. The consolidator activates or frees
    // the slot when it drains the message, IN ORDER with that venue's data -
    // which is what keeps every FlatOrderBook allocation and free on the
    // consolidator thread while the merge runs lock-free.
    //
    // Both return false only on a full queue - a fabricated out-of-range slot
    // or a wedged consolidator. A lost VenueRegistration means the venue never
    // becomes active; a lost VenueDisconnect means a dead venue is merged
    // forever. Both are for the same reason EnqueueHealth is.
    bool EnqueueRegistration(VenueSlot slot);
    bool EnqueueDisconnect(VenueSlot slot);

    // Overflow observability. Both are per venue slot and monotonic.
    //
    // overflow_count is the number of updates/health events that could not be
    // enqueued - each one should have produced a resync. quote_drop_count is
    // the number of quotes deliberately conflated away. A healthy run leaves
    // both at zero, which is what makes kProviderQueueCapacity checkable
    // rather than assumed.
    uint64_t OverflowCount(VenueSlot slot) const;
    uint64_t QuoteDropCount(VenueSlot slot) const;

    // The consumer side. Drains every venue queue until all are empty,
    // applying each message in the order its venue produced it. Returns how
    // many were processed.
    //
    // Runs the same ProcessUpdate/ProcessQuote/ProcessHealth the synchronous
    // path uses, so the two cannot diverge. Takes no lock: the contract is
    // that exactly one thread calls this.
    //
    // Public because tests drive it directly - a synchronous drain is
    // deterministic in a way a background thread is not, so the book tests
    // stay free of sleeps and retries.
    size_t DrainOnce();

    // A venue's staleness verdict changed. Pushed by the
    // provider that owns that venue, on that provider's thread, in the same
    // call sequence as its ApplyUpdate/ApplyQuote calls - so the event is
    // ordered against that venue's own data, which is what makes acting on
    // it safe. When the SPSC queues land this becomes a queue message and the
    // ordering is preserved rather than created.
    //
    // Edge-triggered: only changes arrive, so this is called a handful of
    // times in a healthy run, not per tick.
    //
    // Core may only make a pushed verdict WORSE, never better. The
    // provider can see things Core cannot - its own sockets - so a venue it
    // reports as kDisconnected is disconnected, full stop. Cross-venue
    // corroboration (signal 3, not built) will layer on top of this by
    // demoting a kLive venue that is silent while its peers are busy; it will
    // never promote one.
    void OnVenueHealth(const VenueHealthEvent& event);

    // Per-update breakdown of where ApplyUpdate's time goes. Diagnostic only.
    //
    // Exists because the live publish latency measured ~140-190 us while
    // bench_md_core predicts ~8-12 us for this work, and three hypotheses for
    // the gap have already been killed by measurement. One number cannot be
    // diagnosed; four can.
    struct ApplyTimings {
        // Time the message waited before being processed. Under the old
        // mutex path this was lock contention; on the queued path it is 0
        // (there is no lock) until the message carries an enqueue stamp.
        int64_t lock_wait_ns = 0;
        int64_t book_apply_ns = 0;  // FlatOrderBook::ApplyUpdate - the delta
        int64_t merge_ns = 0;       // MergeBooks - the k-way merge
        uint32_t merged_depth = 0;  // output levels, bid side
        uint32_t delta_levels = 0;  // levels in the incoming update, both sides

        // Which path FlatOrderBook::ApplySide took, per side (0-2). A side with
        // an empty delta counts in neither.
        //
        // this answers a question three documents currently guess at.
        // book_apply is ~40x slower live than the benchmarked in-place path for
        // the same delta size, which suggests most real deltas insert or erase
        // a level and therefore take the region rebuild. If that is right, the
        // in-place optimisation is landing on the rarer case.
        uint8_t fast_path_sides = 0;
        uint8_t applied_sides = 0;
    };

    // Core still reads no clock. It calls a function it was GIVEN, so the
    // no-I/O rule in survives literally rather than by exception - the same
    // shape as every other seam here (CallBack, BboCallback, BookCallback).
    //
    // Null clock disables instrumentation entirely: the cost when unset is one
    // null check per update, and no timing calls at all.
    using ClockFn = std::function<int64_t()>;
    using TimingsCallback = std::function<void(const ApplyTimings&)>;
    void SetInstrumentation(ClockFn clock, TimingsCallback sink) {
        clock_ = std::move(clock);
        timings_callback_ = std::move(sink);
    }

    // Starts the consolidator thread: the single thread that drains every
    // venue queue and owns every FlatOrderBook from here on.
    //
    // after Start(), the book logic is single-threaded. That is the whole
    // point - not speed, but that a single-threaded book is deterministic,
    // testable with fake input, and TSan-clean by construction rather than by
    // careful locking.
    //
    // PRECONDITION for the SYNCHRONOUS RegisterVenue(): it must run before
    // Start(). It mutates state the consolidator reads (active_venues_,
    // venue_id_to_slot_, the per-instrument book arrays) with no lock, which is
    // safe only while no consolidator thread is running - which is why only the
    // tests use it. After Start(), venues arrive dynamically through
    // RegisterVenueName + EnqueueRegistration, which the consolidator applies on
    // its own thread while draining - no shared-state race.
    //
    // Idempotent: calling Start() twice is a no-op, not a second thread.
    void Start();

    // Stops the consolidator thread and joins it, draining whatever is still
    // queued first so a clean shutdown does not silently discard messages the
    // providers already handed over.
    //
    // Safe to call when not started, and called by the destructor - a running
    // thread must never outlive the Core whose members it touches.
    void Stop();

    // How many venue slots are in use. Per-venue loops run to this instead of
    // to kVenueCount, which is what stops "add a venue" from being a recompile
    //     //
    // this is a HIGH-WATER MARK and is never decremented. Slots are dense
    // and assigned in registration order, so removing a venue leaves a HOLE,
    // not a shorter list - decrementing after removing slot 1 of 3 would make
    // the loops stop at 1 and silently drop the venue in slot 2 from the
    // merge, while the published book still looked well-formed.
    //
    // Removal is not a size change. It is already represented by
    // venue_books_[i] == nullptr and health kNoData, and both merge loops
    // already skip that (consolidated_book.cpp). A dead slot costs one test
    // per output level and corrupts nothing.
    //
    // The two repairs that look obvious are worse. Compacting the hole moves a
    // venue to a different slot, and slot ids are held elsewhere - in-flight
    // updates, published attribution - so prices would be attributed to the
    // wrong exchange. A free list reuses the slot, which lets a late update
    // from the old venue land on the new one: the remove/re-add race
    // avoids by tying lifetime to the connection.
    //
    // It barely grows: Register is idempotent by name, so a reconnecting
    // provider reclaims its old slot. The mark equals distinct venue names
    // ever seen, and is bounded by kMaxVenues.
    size_t venue_count() const { return venue_registry_.size(); }

    // Name of the venue occupying `slot`, or empty if nothing ever registered
    // there. The ONLY way out of Core for a venue's identity.
    //
    // attribution inside Core carries a slot, never a name, so
    // this is called at the WIRE boundary - once per published message to
    // build a slot -> name table - and never per level. A merged book has up
    // to 1000 levels with up to kMaxVenues contributors each; resolving names
    // there would be thousands of lookups to produce at most 8 distinct
    // answers.
    std::string_view VenueName(VenueSlot slot) const { return venue_registry_.Name(slot); }

   private:
    // Creates the FlatBookArray for `instrument`, with a FlatOrderBook for each
    // venue that is currently ACTIVE and nulls everywhere else.
    //
    // No venue list parameter: venues come from RegisterVenue, not from
    // config. The two directions have to stay in sync - a new instrument gets
    // books for the venues already registered, and a newly registered venue
    // gets books for the instruments that already exist.
    void AddInstrument(InstrumentKey instrument);
    void RemoveInstrument(InstrumentKey instrument);

    // --- the work itself, with no lock and no venue translation -------------
    //
    // These hold everything the public entry points used to do inline. They
    // take a SLOT, already resolved by the caller, and assume the caller has
    // made them safe to run: after Start(), that is the fact that only the
    // consolidator thread calls them. Before Start(), it is the caller being
    // single-threaded - which is how the tests drive them.
    //
    // splitting these out is what lets the same logic be reached two
    // ways - synchronously under the mutex (today) and from the drained
    // queue on the consolidator thread (next step) - without the book logic
    // itself existing twice. Two copies of a merge that must agree is how
    // the two paths would silently diverge.
    //
    // Not thread-safe on their own, by design. Nothing here locks.
    // Records an enqueue failure for `index` and logs only the FIRST one, so
    // a persistently full queue leaves a counter rather than a flood of
    // identical lines on a path that is by then already failing.
    void NoteOverflow(size_t index, const char* what);

    // The consolidator thread's body: drain, work, sleep, repeat.
    void ConsolidatorLoop();

    // True if any venue queue has something waiting. Used as the condition
    // variable's predicate, so it is checked under doorbell_mutex_ and closes
    // the lost-wakeup window: a message pushed between the drain returning 0
    // and the wait starting is seen by the predicate rather than missed.
    bool HasPending() const;

    // Wakes the consolidator if it is asleep. Called after every successful
    // push.
    //
    // guarded by consumer_waiting_ so the common case costs one relaxed
    // atomic load and no notify at all. notify_one() on a condition variable
    // is a futex wake - cheap when someone is waiting, pure overhead on every
    // message when nobody is. During a burst the consolidator is awake and
    // draining, which is exactly when the notify would be wasted.
    void RingDoorbell();

    void ProcessUpdate(VenueSlot slot, const BookUpdate& update, int64_t lock_wait_ns);
    void ProcessQuote(VenueSlot slot, const BboQuote& quote);
    void ProcessHealth(VenueSlot slot, const VenueHealthEvent& event);

    // Activates a slot RegisterVenueName has already assigned: sets
    // active_venues_ / venue_id_to_slot_ and creates this venue's FlatOrderBook
    // for every instrument that exists. Idempotent (if (!books[index])), so a
    // reconnecting provider that lands back on its old slot after RemoveVenue
    // simply gets its books rebuilt. Runs ONLY on the consolidator thread,
    // reached from DrainOnce on a VenueRegistration message - which is what
    // makes the allocation safe against the lock-free merge.
    void ProcessRegistration(VenueSlot slot);

    // Which slots are live, and the VenueId each one maps to.
    //
    // has_value() is the ACTIVE flag. venue_registry_.size() cannot answer
    // this: it is a high-water mark that deliberately survives RemoveVenue, so
    // a registered-then-removed slot is still inside it and must not be given
    // a book when a new instrument arrives.
    //
    // Holds the VenueId because FlatOrderBook's constructor still takes one. That
    // is the single remaining place Core depends on the enum, kept in one
    // visible spot so the step that migrates FlatOrderBook to VenueSlot can delete
    // exactly this and nothing else.
    std::array<std::optional<VenueId>, kMaxVenues> active_venues_{};

    // VenueId -> slot. The single point where the venue identity carried on an
    // incoming update becomes the index Core stores by.
    //
    // without this, every array access here is array[VenueId], which is
    // only correct while slot N happens to equal VenueId N - i.e. while venues
    // register in enum order with no gaps. This table is what removes that
    // restriction: register OKX first and it takes slot 0, and every lookup
    // still finds it.
    //
    // Sized by kVenueCount, not kMaxVenues, and that is deliberate: it is a
    // translation table FROM the enum, not storage. Its length is the number
    // of VenueIds that exist, while the arrays it maps into are sized by slot
    // capacity. The two are different quantities that happen to look alike.
    //
    // Disappears entirely once providers carry their own slot on the wire
    // - Core will read the slot directly and translate nothing.
    std::array<std::optional<VenueSlot>, kVenueCount> venue_id_to_slot_{};

    // The slot this venue's data belongs in, or nullopt if it is not
    // registered. Callers drop the message rather than guessing - see the
    // "never create state from a data message" rule in.
    std::optional<VenueSlot> SlotFor(VenueId venue) const {
        const size_t index = static_cast<size_t>(venue);
        if (index >= venue_id_to_slot_.size()) {
            return std::nullopt;
        }
        return venue_id_to_slot_[index];
    }

    // Maps venue names to the dense slots that index venue_books_,
    // venue_quotes_, depth_health_ and bbo_health_.
    //
    // Populated in Init() today, from CoreConfig::venues. Once providers dial
    // in over a socket the same call moves to the kHello handshake and
    // nothing else here changes - which is why the registry is keyed on the
    // venue NAME rather than on VenueId.
    //
    // Core still indexes by VenueId elsewhere. Migrating the loop bounds and
    // then the index itself are separate later steps; nothing reads this yet.
    VenueRegistry venue_registry_;

    // One SPSC ring per venue slot. Single producer: that venue's provider
    // thread. Single consumer: whoever calls DrainOnce - today a test, from
    // the next step the consolidator thread.
    //
    // Fixed array, not a vector: kMaxVenues rings cost 224 KB total
    // (256 slots x 112 bytes x 8, measured), and a vector would invalidate a
    // producer's reference to its own queue the moment another venue
    // registered and forced a reallocation - while that producer was
    // mid-push.
    //
    // Indexed by slot, so a dead slot simply holds an empty ring nobody
    // pushes to. ProviderQueue holds atomics, so it is neither copyable nor
    // movable - which makes Core non-movable too, and that is correct: a
    // Core whose queues were being read while it moved would be a race.
    std::array<ProviderQueue, kMaxVenues> queues_;

    // Written by producer threads, read by whoever reports. Relaxed ordering
    // throughout: these are counters, not synchronization - a reader that
    // sees a slightly stale value draws the same conclusion, and making them
    // ordered would put a barrier on the path this design exists to keep
    // clear.
    std::array<std::atomic<uint64_t>, kMaxVenues> overflow_count_{};
    std::array<std::atomic<uint64_t>, kMaxVenues> quote_drop_count_{};

    // run continuously, not a micro-optimization.
    std::thread consolidator_;
    std::atomic<bool> running_{false};

    std::mutex doorbell_mutex_;
    std::condition_variable doorbell_;

    // Set only while the consolidator is inside the wait. Lets RingDoorbell
    // skip the notify entirely when the consumer is awake and draining.
    std::atomic<bool> consumer_waiting_{false};

    // fast-BBO are separate sockets and fail independently.
    //
    // initialized to kNoData, not kLive - fail-safe. Nothing is admitted
    // to the merge until a provider has affirmatively said its feed is alive.
    // The cost of being wrong in this direction is one publish with a thinner
    // book; the cost of the other direction is publishing prices from a venue
    // we have never heard from. The provider promotes a stream out of kNoData
    // on its very first message, so this costs no startup delay.
    //
    // Written and read only on the consolidator thread (via ProcessHealth /
    // ProcessUpdate), so no lock guards them. That single-threadedness is the
    // property Start establishes, and it is why can say there is no
    // lock on the book path.
    VenueHealthArray depth_health_{};
    VenueHealthArray bbo_health_{};

    // Bumped whenever a BBO-stream verdict changes. Compared against the
    // per-instrument value below to decide whether the next quote can be
    // folded in incrementally or needs a full rescan.
    //
    // the merged Book needs no equivalent, because MergeBooks rebuilds
    // from scratch every pass - change the admission rule and the next output
    // is already correct. The BBO is different: UpdateBBOWithQuote maintains
    // PERSISTENT state, so a stale venue's price is already inside
    // consolidated_bbo_, and the venue sends nothing more to displace it.
    // Skipping its future quotes cannot remove a price that is already there.
    //
    // A version counter rather than a flag because health is per VENUE while
    // the BBO is per INSTRUMENT: one venue going stale invalidates every
    // instrument's BBO, and a counter says so without iterating them.
    uint64_t bbo_health_version_ = 0;
    std::unordered_map<InstrumentKey, uint64_t, InstrumentKeyHash> bbo_health_version_seen_;

    BboCallback bbo_callback_;
    BookCallback book_callback_;

    // Both null unless SetInstrumentation was called. Read on the hot path,
    // so the null check is the entire cost in a normal run.
    ClockFn clock_;
    TimingsCallback timings_callback_;
    InstrumentBooks venue_books_;
    InstrumentQuotes venue_quotes_;

    // The running consolidated BBO per instrument. This is persistent state,
    // maintained incrementally by UpdateBBOWithQuote rather than recomputed
    // from scratch - which is exactly why the oracle test matters: a bug in
    // the incremental update doesn't fail loudly, it accumulates here
    // silently across thousands of updates.
    InstrumentBbo consolidated_bbo_;

    // The persistent consolidated book per instrument, repaired in place from
    // one venue's LevelChanges instead of re-merged from every venue.
    //
    // this replaced a free-list of snapshot buffers. The pool existed so a
    // slow subscriber could hold an old book while the next merge filled a
    // different buffer - and every publish paid a full copy for that. Nothing
    // retains the book, so the pool never grew past one entry and the copy was
    // pure cost.
    //
    // Owned and mutated ONLY on the consolidator thread, like every other book
    // here, which is what lets it be published by reference with no lock.
    std::unordered_map<InstrumentKey, consolidated::State, InstrumentKeyHash> consolidated_state_;

    // Latches a full rebuild on every instrument. Called for the events an
    // incremental repair cannot express - a venue's health verdict changing, a
    // venue registering or being removed - because those are per VENUE while a
    // rebuild is per INSTRUMENT.
    void MarkAllForRebuild();
};

}  // namespace market_data
