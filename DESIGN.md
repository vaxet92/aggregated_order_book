# Consolidated Order Book Aggregator — Design

Aggregates BTCUSDT, ETHUSDT and SOLUSDT market data from Binance, OKX and Bybit into one consolidated order book per market and publishes derived views — consolidated BBO, notional volume bands, bps price bands — over gRPC. Spot and futures both run, as two fully independent stacks that never share a book.

C++20 · Boost.Beast/Asio + OpenSSL (WSS) · simdjson · ZeroMQ (provider↔core) · gRPC + Protobuf (client) · CMake + vcpkg · GoogleTest · a small `fmt`-based `Logger`.

This document is the design reasoning: what was built, what was measured, what was rejected, and what is knowingly missing. Build/run is in `QuickStart.md`; `README.md` is the high-level summary.

---

## 1. Scope

### 1.1 In scope

| Area | Deliverable |
|---|---|
| Market data ingest | 3 venue adapters × 2 markets: snapshot+delta sync, gap detection, resync, reconnect; symbol set = `BTCUSDT`, `ETHUSDT`, `SOLUSDT` (config-selected) |
| Consolidated book | Per-venue books + incremental deep merge with per-venue attribution |
| Staleness policy | Watchdog, drift detection, venue admission/exclusion, status published to clients |
| gRPC API | Single extensible `Subscribe` streaming RPC, conflated fan-out |
| Derived views | Consolidated BBO; VWAP-to-notional bands; cumulative-depth-within-bps bands |
| Clients | 3 client binaries (BBO / volume bands / price bands) → stdout |
| Transport | ZeroMQ between each provider and its core |
| Packaging | One multi-stage Dockerfile, docker-compose per market |
| Tests | Unit + property + oracle + integration; `md_core`'s no-I/O design makes book/staleness/band logic directly testable |

### 1.2 Out of scope

Multi-symbol beyond the three defined `InstrumentId`s (`BTCUSDT`/`ETHUSDT`/`SOLUSDT`); only `BTCUSDT` is exercised against live venues · order entry / execution · persistence of market data beyond process memory · authentication/TLS on the gRPC hop · cross-venue fee, withdrawal, or inventory modelling · kernel bypass, busy-poll, CPU pinning, hugepages.

### 1.3 Spot and futures are separate stacks

BTCUSDT **spot** and the BTCUSDT **perpetual** are different instruments with different prices; merging them into one book is a correctness bug, not a rounding artifact. Both markets run, each with its own provider processes and its own core. Every book, health array and queue is keyed by `InstrumentKey` — symbol and market packed into one `uint32_t` — so no code path can merge them. The separation is asserted in config validation, not only documented, and Compose expresses it as a default `spot` stack plus a `futures` profile (§11).

---

## 2. Architecture

Three components, one process each. Providers own all exchange-specific protocol handling; the core consumes only normalized, validated updates; gRPC is the client boundary.

```
   exchanges                one process per (venue, market)
  ┌──────────┐  WSS  ┌────────────────────────────────────┐
  │ Binance  │──────►│ md_provider_app                    │
  │ OKX      │──────►│  WS + TLS + simdjson parse         │──┐ ZeroMQ
  │ Bybit    │──────►│  continuity, gap detect, resync    │  │ DEALER
  └──────────┘       │  dedup, staleness watchdog         │  │ ipc://
       ▲ REST        └────────────────────────────────────┘  │
       │ (Binance snapshot only)                             ▼
                        ┌──────────────────────────────────────────┐
                        │ md_core_app --market=spot                │
                        │  ZmqCoreIngress (ROUTER)                 │
                        │  per-venue SPSC queues                   │
                        │  ONE consolidator thread owns every book │
                        │   FlatOrderBook × venues                 │
                        │   consolidated::State (incremental)      │
                        │   BBO, volume bands, price bands         │
                        │  gRPC, per-session conflation             │
                        └──────────────────┬───────────────────────┘
                                           │ server-streaming gRPC
                        client_app  ×  N subscribers
```

### 2.1 Components

`md_provider_app` and `md_core_app` are one binary each, given a role by command-line flags — one provider per `(venue, market)`, one core per market. Process supervision (spawn, restart) is `docker-compose`, not a component in this codebase (§14.1).

| Target | Contents |
|---|---|
| `md_core` | Domain types, `FlatOrderBook`, the consolidator, band math. **No I/O, no networking, no clock.** The tests live here |
| `logger` | ~40-line header wrapper over `fmt`; anything may depend on it |
| `md_provider` | Provider + Binance/OKX/Bybit implementations: WebSocket, REST, parsing, continuity, dedup, watchdog |
| `md_wire` | ZeroMQ codec and both socket ends (`ZmqProviderSink`, `ZmqCoreIngress`) carrying a provider's output to a core (§14.3, §14.4) |
| `md_proto` | Generated protobuf/gRPC stubs |
| `aggregator` | Wiring for both app binaries: the gRPC service, conflated per-session channels, latency recording |
| `client_common` + 3 client binaries | connect, retry, subscribe, render |

**The layering rule that makes the rest work: `md_core` has zero I/O dependencies.** Exchange-specific semantics — Binance's `U`/`u`/`pu`, Bybit's `u`, OKX's `seqId`/`prevSeqId` — are stripped at the provider boundary (§9), so `md_core` consumes only a normalized, validated `BookUpdate`. This is what makes the interesting logic testable with three integers and an expected result.

`Logger` is a thin `fmt` wrapper rather than `spdlog`: with no rotation, async sinks or multi-sink fan-out anywhere in the system, `spdlog` is a large dependency for "format a line, write it to stdout". `spdlog` is the documented fallback if structured/async logging is ever needed.

### 2.2 Data flow

One depth update, end to end:

1. **Provider** receives a WSS frame, TLS-decrypts, `simdjson`-parses it directly into a `BookUpdate` (absolute qty at price, `qty == 0` = remove). Continuity is checked against the venue's own sequence fields; a gap triggers a resync inside the provider (§4.2), invisible to the core.
2. Provider stamps `recv_mono_ns` (staleness) and `recv_ts_ns` / `exch_ts_ns` (drift), then hands the flat-POD update to `ZmqProviderSink`, which writes it on one `DEALER` socket over `ipc://`.
3. **Core** `ZmqCoreIngress` (`ROUTER`) receives it, attributes it to a `venue_slot` by transport identity (§14.3), and pushes it onto that venue's SPSC queue.
4. The **single consolidator thread** drains all queues, applies each delta to the owning `FlatOrderBook`, repairs the incremental `consolidated::State` from the changed venue's level transitions (§5.2), and runs the staleness verdict.
5. It computes BBO and bands once per distinct threshold set, builds the protobuf `Update`, and hands it to each gRPC session's depth-1 conflated slot (§7.4). Slow clients never back-pressure the book.

---

## 3. Domain model

Prices and quantities are **scaled `int64`**, never floating point.

```
PriceTicks   = int64    // price × 1e8, integral on the canonical grid
QtyUnits     = int64    // base quantity × 1e8
Notional     = int128   // ticks × units needs the width
VenueId      = uint8    // dense index, array-friendly
```

Doubles accumulate error across VWAP sums, compare badly for level identity, and read as a red flag in review. `int128` for notional avoids overflow at 50M USDT under 1e8×1e8 scaling; the overflow bound is commented at the multiply site.

**Canonical price grid.** Venue tick sizes differ (Binance spot 0.01, OKX 0.1, Bybit 0.01). The canonical grid is the *minimum* tick across configured venues, so every venue price maps onto it exactly with no rounding. A future venue with an incommensurable tick must be rejected at startup rather than silently rounded — no rounding means no bias to document.

**Normalized update**, produced by every adapter and the only thing that crosses the provider boundary:

```
BookUpdate {
  VenueId  venue
  uint64   seq            // venue-native monotonic sequence
  uint64   recv_mono_ns   // steady_clock, ours — staleness only (§6)
  uint64   recv_ts_ns     // system_clock, ours — drift + logs
  uint64   exch_ts_ns     // venue's — drift estimation only
  bool     is_snapshot    // full replace vs delta
  span<LevelDelta> bids, asks   // qty == 0 means remove
}
```

`LevelDelta { PriceTicks price; QtyUnits qty; }` — absolute qty at price, matching all three venues' semantics. Adapters own every venue-specific quirk; nothing above this line knows a venue's name.

---

## 4. Market data providers

### 4.1 Interface

```
class MDProvider {
  virtual void start(BookUpdateSink&) = 0;
  virtual void stop() = 0;
  virtual VenueStatus status() const = 0;
};
```

The sink is called on the provider's own thread. After warm-up a provider allocates nothing per update: parse buffers, delta vectors and read buffers are reused.

Parsing is a `Parser` base class owning a reused `simdjson::ondemand::parser` and a growable input buffer; `BinanceParser` / `OkxParser` / `BybitParser` derive from it. One instance per parsing thread (the parser is not thread-safe). A venue's depth and fast-BBO streams share one parser on one thread; Binance's detached REST-snapshot fetch uses its own.

### 4.2 Synchronization

```
Disconnected → Connecting → Syncing → Live
                    ▲          │        │
                    │          │        ├─→ Gap    ─→ Resync ─┐
                    └──────────┴────────┴─→ Stale ────────────┘
                             (backoff + jitter)
```

- **Syncing** — subscribe to the delta stream *first*, buffer, *then* fetch the REST snapshot, then discard buffered events older than the snapshot and apply the rest. The other order loses updates.
- **Live** — continuity is asserted on every message; any violation → `Gap`.
- **Gap** — the book is *wrong*, strictly worse than stale: exclude immediately and resync. Exponential backoff with jitter, plus hysteresis so a flapping feed does not resync-storm.
- **Stale** — the book may still be correct but is not current (§6).

### 4.3 Venue-specific sequencing

| | Binance | OKX | Bybit |
|---|---|---|---|
| Depth stream | `btcusdt@depth@100ms` | `books` | `orderbook.50.BTCUSDT` |
| Snapshot | REST `/api/v3/depth` | in-channel | in-channel |
| Continuity | `U ≤ lastUpdateId+1 ≤ u`, then `pu` chaining | `seqId` / `prevSeqId` | `u` monotonic, `type=snapshot` resets |
| Fast BBO | `btcusdt@bookTicker` | `bbo-tbt` | `orderbook.1` |
| Heartbeat | server ping / 3m | `ping` text / 30s | `{"op":"ping"}` / 20s |

**OKX futures quote sizes in contracts, not coins.** At `ctVal 0.01` an OKX SWAP level of `495.94` is 4.9594 BTC; left unconverted, OKX enters the consolidated futures book ~100× oversized and dominates every level. The OKX futures adapter fetches `ctVal` from `/api/v5/public/instruments` (`rest_instruments_path`) and scales at normalization. Spot is unaffected.

### 4.4 Combining fast-BBO and depth streams

Both streams are available per venue. They are **not mutually sequenced**, so fast-BBO ticks are never spliced into the depth book — that would corrupt it. They are used separately, two ways:

1. **Fast-BBO drives the published consolidated BBO.** It is published on its own callback (§7.3), not derived from the merged depth book, and carries its own per-venue staleness verdict (§6). It is a distinct, lower-latency source than the depth book.
2. **Depth-vs-BBO divergence as a correctness oracle** — comparing the depth-derived BBO against the fast-BBO stream and forcing a resync on a persistent mismatch. **Not built (§15.1).** It would be the only *live* cross-check on the depth book: gap detection catches *missed* messages, not *misapplied* ones, and no venue offers a per-message checksum (OKX's CRC32 is deprecated, fixed at 0). Every `RequestResync()` today is driven by a sequence continuity gap or a snapshot-fetch failure, never by a BBO comparison.

---

## 5. Order book & consolidation

### 5.1 Per-venue book

**Flat sorted vector per side** (`md_core/flat_order_book.h`), over `std::map`: contiguous memory, no per-node allocation, and for a few-hundred-level working set a `memmove` on insert beats red-black-tree rebalancing.

**Storage order is reversed from the naive choice: bids ascending, asks descending — best price at `back()`, not `front()`.** Updates cluster at the top of book, and `back()` is the only end of a vector cheap to grow and shrink: a new best price is a `push_back`, removing one a `pop_back`. Best price at `front()` would `memmove` the whole book on every new best bid — ~16 KB at 1000 levels — so the most frequent event would pay the most expensive move. The cost is that readers walk backwards, which is near-free: prefetchers handle descending strides.

Measured (one run, 20 000 iterations): top-of-book churn **11× faster than `std::map`**, and **20× faster than the same churn 500 levels deep** — `std::map` shows only 1.09× between the two, because a node is a node wherever it sits. Erasing the top of book writes zero bytes. Deep structural churn is a genuine loss (1.67× slower than `std::map`) and is recorded rather than hidden; it stays net positive per depth update because each one pays an apply *and* a merge, and the apply is the smaller.

`MergeBooks` and `ComputeBBO` are templated on the book array, so a second book representation can be added as another instantiation without touching their bodies. The next such representation, a tick-indexed ladder, is designed but not built (§15).

**`FlatOrderBook` is the only per-venue book.** `FlatOrderBookTest` cross-checks it against a two-line inline `std::map` reference model (`RefBook`), driven through a randomized multi-level stream. An independent implementation is what catches an aliasing bug a single implementation's own assertions cannot: an in-place delta application that picks its walk direction from the *net* size change is wrong whenever one delta contains both an insert and an erase, and the corrupted result stays sorted and the right length with one level's data merely duplicated — no assertion, no crash, no sanitizer finding. The incremental merge has its own separate oracle, `State::FullRebuild` (§5.2).

### 5.2 Incremental consolidation

- **Consolidated BBO is eager.** Each venue's best bid/ask is cached; consolidated BBO is `max`/`min` over three values — a handful of comparisons per update.
- **Deep merge is incremental.** `Core` keeps one `consolidated::State` per instrument and repairs it from the changed venue's `LevelChange` stream rather than re-deriving the whole output every update.

A full k-way merge on every update — walk all three venue books top-N and rebuild every output level, every time — was the first design, replaced once measured. Core publishes on *every* accepted depth update, so "merge cost paid per publish, not per update" bought nothing, and a full merge re-derived ~3000 output levels every time one venue moved a handful.

**The measured shape of the cost is what makes the incremental design correct.** `MergeBooks` (the full-rebuild path) is **write-bound roughly 4.6:1** — ~352 KB written per merge against ~77 KB of venue book read — and walking the venue books is only ~584 ns of ~9.5 µs. The waste was never re-reading three venues; it was re-deriving output that had not changed. The merge is also compute- and latency-bound, not bandwidth-bound: halving `MergedLevel`'s per-level write (removing a ~257-byte zero-fill, down to ~114) moved per-level cost from 19.3 ns to 18.1 ns — inside run-to-run noise.

Measured, deltas of 5 and 50 levels, medians over 3 runs, both arms including the band walk a client receives:

| | full merge | incremental |
|---|---|---|
| 5-level delta, end to end | ~10 960 ns | **~1670 ns** (6.6×) |
| 50-level delta, end to end | ~11 120 ns | **~2960 ns** (3.8×) |
| consolidation alone | ~9500 ns | **167 ns** (56×) |

`MergeBooks` is not deleted — it is re-roled as `State::FullRebuild`, used for startup, snapshot/resync, a health transition, venue add/remove, and as the differential-test oracle: `incremental == FullRebuild` is asserted after every event in `ConsolidatedStateTest`.

**What the full rebuild gave for free and the incremental path must pay for: excluding a degraded venue.** Under a full rebuild the next rebuild is simply correct. Incrementally, a stale venue's liquidity is already folded into the consolidated levels and it sends nothing more to withdraw it (the same problem the incremental BBO has, §6). The fix: `Core::MarkAllForRebuild()` latches a full rebuild on every instrument when a depth-stream health verdict changes, or when a venue registers or is removed — rare by construction, since health is edge-triggered, so not on the hot path.

**Depth budget N** must cover the deepest question asked — the 50M+ notional band or the 1000 bps price band, whichever reaches further. On BTCUSDT spot, 1000 bps is far deeper than typical book coverage, so bands *will* legitimately exhaust available depth. That is reported as an `insufficient_depth` flag with the partial fill, not silently truncated. N is configured with headroom (~1000 levels max from each venue's stream).

### 5.3 Attribution

```
ConsolidatedLevel {
  PriceTicks price
  QtyUnits   total_qty
  QtyUnits   qty_by_venue[MAX_VENUES]   // attribution, not optional
}
```

Per-venue attribution is carried through the merge at every level. A consolidated book without it is much less useful to a real consumer, and it is what makes the staleness story legible — a client can see which venue a level came from and whether that venue is currently admitted. It is exposed on the wire only for the BBO today (§15).

---

## 6. Staleness & correctness

An order book is **state**, not an event stream. Each venue's book is a replicated state machine driven by its own sequenced feed; the consolidated book is the sum of the latest known states. Latency does not need *alignment* — it needs a *staleness policy*. A venue 100 ms behind contributes liquidity that may no longer exist; the job is to detect that and say so.

### 6.1 Why exclusion, not just reporting

The merge takes `max(bid)` and `min(ask)`. A frozen venue never moves, so when the market falls it **always** looks like the best bid, and when it rises it **always** looks like the best ask. Staleness is not noise that averages out — **the merge actively selects for it**. One frozen venue out of three corrupts the output nearly every time the market moves, not one third of the time.

From `ConsolidatedBookTest.FrozenVenueNoLongerWinsTheBestBid`:

```
BINANCE frozen   bid 50000   ask 50010     (prices from before the move)
BYBIT   live     bid 49900   ask 49910
OKX     live     bid 49899   ask 49911

admitting the frozen venue:
  best bid 50000 (BINANCE), best ask 49910 (BYBIT)  ->  CROSSED
  a phantom 90-tick arbitrage that nobody can trade
```

Reporting the venue as stale while still merging it is not enough. It must be excluded.

### 6.2 The two clocks

Staleness and drift need **different clocks**; one field cannot serve both.

| field | clock | used for |
|---|---|---|
| `recv_ts_ns` | wall (`system_clock`) | drift against `exch_ts_ns`, wire, logs |
| `recv_mono_ns` | monotonic (`steady_clock`) | staleness only |

Staleness is `now_mono − last_mono`: both readings come from the same never-jumping clock, so its arbitrary epoch cancels exactly. A wall clock breaks this both ways — an NTP step **back** blinds the watchdog; a step **forward** marks every venue stale at once and publishes an empty book that reads in the logs like a total exchange outage.

`recv_ts_ns − exch_ts_ns` must **never** be a staleness measure: our clock and the venue's are not synchronized, so that difference is staleness **plus** an unknown clock offset **plus** network delay — three unknowns, one equation. Fit for drift estimation only.

### 6.3 Signals, in order of certainty

1. **Connection state.** Every socket for a venue's stream down → `kDisconnected`, excluded. The only verdict made with certainty — and one-directional: a socket that is `ESTABLISHED` can still be wedged, so **connection state can condemn a venue, never clear one.** That asymmetry is why the timer verdicts still exist.
2. **Venue keepalives.** Two of three venues prove liveness by *republishing an id already seen*, turning silence past a documented interval into evidence rather than a guess (§6.4).
3. **Cross-venue corroboration.** Silence is ambiguous — a dead feed and a quiet market look identical to a timer. The disambiguation: *a quiet market is market-wide, a dead feed is per-venue.* All three silent → market is quiet, nobody stale; two active, one silent → that one is broken. The only sub-minute signal for Binance, which sends no keepalive at all. **Not built (§15).**
4. **Absolute backstop.** A generous per-venue timeout catching the zombie-connection case signal 1 cannot see. A backstop, not a detector — signals 2 and 3 are meant to be faster, so it need not be tuned tightly.
5. **One-way-delay drift detector.** An EWMA of `(recv_ts − exch_ts)` against its running minimum; deviation above that minimum is queueing delay, an early warning. **Diagnostics only — it must never gate admission** (§6.2's three unknowns).
6. **Publish freshness.** Every message carries a `venue_status` block — state, `age_ms`, `last_seq`, `drift_ms`, contributed-or-not — so a client can tell "the bid dropped because the market moved" from "the bid dropped because we excluded Binance".
7. **Admission rule.** A venue contributes iff its verdict is `kLive`, written `== kLive` — an **allow-list, not a deny-list**, so any state added later defaults to *excluded*. Asserted by `VenueHealthTest.AdmissionIsAnAllowListNotADenyList`. Hysteresis on re-admission (healthy for M consecutive ticks) is **not built (§15)**.

### 6.4 Per-venue thresholds are derived, not invented

| venue | stream | keepalive behaviour | backstop |
|---|---|---|---|
| Bybit | L1 (our BBO) | republishes with the **same `u`** after **3 s** of no change | derived from 3 s |
| OKX | `books` (our depth) | `seqId == prevSeqId`, empty sides, after **~60 s** | derived from ~60 s |
| Bybit | depth (L50) | none documented | measure |
| Binance | depth + `@bookTicker` | **none** | measure |
| OKX | `bbo-tbt` | unverified | measure |

"Bybit L1 must speak every 3 s because Bybit documents that it does" is defensible; "250 ms felt right" is not — and 250 ms would mark Bybit L1 stale on every quiet interval. OKX's ~60 s bounds the worst case but is far too slow to be a detector alone, which is why signals 1 and 3 carry the load there.

**The liveness stamp sits in front of the duplicate filter.** Both keepalives carry an id already seen, so `SeqDedup` correctly drops them as book updates — and would destroy the liveness signal with them. `NoteDepthActivity()` / `NoteBboActivity()` are therefore called immediately after a successful parse and **before** `AcceptDepth` / `AcceptBbo`. A duplicate from a redundant connection updates the same stamp, which is correct: for liveness, a duplicate and a keepalive are the same event. Protocol frames (ping, pong, subscribe acks) are rejected by the parser before the stamp: a heartbeat proves the socket is open, not that data is flowing.

### 6.5 Depth and BBO staleness are tracked separately

The depth and fast-BBO streams are separate sockets. A venue can be stale on one and live on the other, so each carries its own stamp and verdict. Consequence for clients: the published BBO and the published Book can legitimately disagree about which venues are included. Correct, not a bug — but it looks like one to anyone not told.

### 6.6 Crossed consolidated books

With three venues, venue A's bid **will** sometimes exceed venue B's ask — genuine cross-venue arbitrage plus propagation delay, not a bug. **Decision: publish as-is, with a `crossed` flag and the crossing magnitude in bps.** It is the true consolidated state and the only reversible choice — a client given the raw book can compute a clean one, never the reverse. Uncrossing by matching the overlap off is a rejected alternative. Band math must tolerate a crossed book: walking bids down from a best bid above the best ask is well-defined and the VWAP results are still correct statements about available liquidity — a required test case.

### 6.7 Where the verdict is made: the provider decides, Core is told

**The provider computes the verdict for its own two streams and pushes it to Core in-band**, on the same channel as the updates. Four reasons:

1. **It closes the total-outage hole for free.** If Core checked health only when an update arrived, all three venues going silent would mean nothing checks anything and the client holds a stale book forever. The provider's `io_context` already runs a `steady_timer` that fires whether or not data arrives — no extra thread.
2. **Nothing on the hot path.** Core reads a stored verdict.
3. **The provider owns the facts.** Sockets are its business.
4. **Staleness is an edge, not a level.** Pushing on change matches the shape of the event.

**In-band, not a side channel.** A real-time health flag read directly by Core, while Core is still draining that venue's 50-ms-old updates from the queue, gives an inconsistent view — excluding Bybit from the merge while still applying Bybit updates. In-band instead:

```
drain: update, update, [BYBIT STALE], update, ...
```

Core learns of the staleness at the right point in that venue's timeline — late, but *consistently* late, with the data it belongs to. Per-venue SPSC (§7.2) gives exactly this ordering: Bybit's updates and Bybit's health event share a queue and are ordered; Bybit's event and Binance's update have no defined order and need none.

| who | knows | decides |
|---|---|---|
| Provider | its own sockets, its own silence | `kDisconnected`, `kNoData`, `kStale` past its own backstop |
| Core | all three venues at once | cross-venue: silent *relative to peers* (signal 3) |

Core takes the pushed verdict as a starting point and may only make it **worse**, never better.

---

## 7. Concurrency & publish path

### 7.1 The parallelism axis is the venue

One thread per venue: WSS receive → TLS decrypt → JSON parse → normalized `BookUpdate` → push to an SPSC queue. Parsing is ~80% of ingest CPU, and this parallelizes it across cores with no coordination. **Rejected: a worker pool parsing one venue's stream** — it breaks message ordering and forces a resequencer, real complexity for negative benefit.

### 7.2 One thread owns all books

**The consolidator thread owns every `FlatOrderBook`.** Venue threads only parse; they never touch a book. Deltas cross into the consolidator through per-venue SPSC ring buffers (`Core` holds `std::array<ProviderQueue, kMaxVenues>`) — one producer, one consumer, the simplest lock-free structure there is.

Consequence: **there is no lock anywhere on the book path** — no mutex, no seqlock, no atomics on book state. The book logic is single-threaded, so it is deterministic, trivially testable with fake input, and TSan-clean by construction rather than by care.

### 7.3 Consolidator thread

Woken by a **coalesced wakeup**: any venue push sets a dirty flag and signals; the consolidator drains, works, and loops. Self-clocking — a single update in a quiet market publishes immediately, and a burst collapses into the next pass. No timer to tune, no idle delay.

Per pass:

1. Drain all SPSC queues; apply deltas to the venue books.
2. Repair the incremental consolidated book from the changed venue's `LevelChange`s (§5.2); a full `MergeBooks` runs only on a rebuild.
3. BBO is published on its own callback from the fast-BBO stream (§4.4), not derived from the merged depth book.
4. Publish: `BookCallback` takes a `const consolidated::Book&` and Core passes its live state directly — **no snapshot copy.** The reference is valid only for the callback's duration; a consumer that needs to keep the book copies it (three test harnesses do).

> **Rejected: copying the finished book into a pooled buffer** so a subscriber could hold it after returning. No production consumer needs it — `PublishBook` computes bands synchronously, pushes protobuf, and drops the book before returning — and the copy cost ~8.2 µs per publish against a 167 ns merge, reproducing almost all the work the incremental merge had just removed. When a genuinely asynchronous consumer appears (a depth feed, or a provider on a separate host, §14), the answer is versioned buffers caught up by replaying `LevelChange`, not a return to copying.

Band math (all bands, both sides) is **memoized on the requested thresholds**: sessions that asked for the same bands get byte-identical messages apart from `seq`, so the walk and protobuf build happen once per distinct threshold set per publish, shared across sessions. The cache is per publish — the book has just changed — and lookup is a linear scan with a vector compare, because the number of distinct threshold sets in practice is small and hashing a vector would cost more than it saves.

Measured live (Docker, three venues, both markets): `book_publish` median **~50 µs**, against a band walk capped at 1500 levels ≈ 1.4 µs — about 3% of live publish cost. The rest is parsing, the ZeroMQ hop and gRPC serialization, none of which a microbenchmark of the book logic sees. Band memoization was worth doing for its *complexity shape* — unmemoized, cost grows with subscriber count — not for a large share of live latency.

### 7.4 Backpressure: per-session conflation

**The #1 systems risk is a slow gRPC client back-pressuring the book.** Each session holds a depth-1 pending slot with overwrite semantics: a new snapshot arriving while the previous write is in flight replaces the pending one, never queues. Nothing upstream ever blocks. This is intentional: **a state-publishing API, not an event log.** Dropping intermediate states is correct by design, and `seq` lets clients observe it happened.

**The session loop's wait on that slot is bounded** — `kCancellationPollInterval` (200 ms) in `aggregator_service.cpp`. `grpc::ClientContext::TryCancel()` sets a flag inside gRPC and cannot wake a thread parked on `ConflatedChannel::WaitAndTake()`, a condition-variable wait outside gRPC's control. An unbounded wait leaked a server thread and its session entry for the life of the process whenever a client disconnected while its channel was idle — the common case in a quiet market — and hung `server->Shutdown()` too. The bounded wait re-checks `IsCancelled()` even with nothing published; it costs no happy-path latency, since a real publish still wakes it immediately. The event-driven fix is gRPC's callback/reactor API where `OnCancel` fires and no thread parks — not built.

### 7.5 Allocation discipline

Hot paths allocate nothing after warm-up: reused simdjson parser and input buffer in `Parser`; `BookUpdate` level vectors reserved to the venue depth tier at construction; pre-sized merge scratch; snapshot objects recycled through a free list; protobuf message reuse via `Arena` on the publish path. A debug-build allocation counter asserts this in tests.

---

## 8. Derived views

Both derived views carry real definitional ambiguity, and pinning it down explicitly is part of the exercise.

### 8.1 Best Bid-Offer

Consolidated best bid = highest bid price across admitted venues; qty = summed qty at exactly that price across venues that quote it, with per-venue attribution. Symmetric for ask. Spread and mid included. `crossed` flag when best bid ≥ best ask.

### 8.2 Volume bands — 1M / 5M / 10M / 25M / 50M+ notional

**VWAP to fill N USDT of notional by sweeping the consolidated book.** Walk from the top accumulating `price × qty` until the band's notional is reached, splitting the final level proportionally. Per band, per side: `vwap`, `worst_price` (last level touched), `filled_notional`, `filled_qty`, and `insufficient_depth` when the book is exhausted first. Notional is in USDT (the quote currency). `50M+` is the terminal band — VWAP over all available depth, flag set when it can't be filled, which on BTCUSDT spot it frequently won't be.

The alternative reading — "the price level at which cumulative notional crosses N" — is a strictly weaker statement than the VWAP and not what an execution consumer wants.

### 8.3 Price bands — BBO + 50 / 100 / 200 / 500 / 1000 bps

**Cumulative liquidity within X bps of the consolidated BBO**, measured from the BBO as the assignment states ("BBO+"): bids from best bid down to `best_bid × (1 − bps/10000)`, asks up from best ask. Per band, per side: `cum_qty`, `cum_notional`, `vwap`, `level_count`, `limit_price`.

### 8.4 Parameterization

The assignment's band values are **defaults, not constants.** Bands arrive as a repeated field in the subscription request. Hardcoding 1M/5M/10M is the obvious extensibility miss on a rubric that names "API/protocol design and extensibility" explicitly.

---

## 9. gRPC API

### 9.1 One RPC, not three

```proto
service Aggregator {
  rpc Subscribe(SubscribeRequest) returns (stream Update);
  rpc GetVenueStatus(VenueStatusRequest) returns (VenueStatusResponse);
}

message SubscribeRequest {
  string symbol = 1;                 // "BTCUSDT"
  MarketType market = 2;             // SPOT | FUTURES — MARKET_UNSPECIFIED rejected
  repeated Feed feeds = 3;           // BBO | VOLUME_BANDS | PRICE_BANDS | DEPTH
  repeated int64 notional_bands = 4; // scaled; defaults applied if empty
  repeated uint32 bps_bands = 5;     // defaults applied if empty
  uint32 max_depth = 6;              // for DEPTH
}

message Update {
  uint64 seq = 1;                    // server-side monotonic; gaps = conflation
  uint64 server_ts_ns = 2;
  string symbol = 3;
  repeated VenueStatus venues = 4;
  uint32 price_scale = 5;            // exponent: divide price fields by 10^price_scale
  uint32 qty_scale = 6;             // exponent: divide qty fields by 10^qty_scale
  oneof payload {
    Bbo bbo = 10;
    VolumeBands volume_bands = 11;
    PriceBands price_bands = 12;
    Depth depth = 13;
  }
}
```

One generic streaming RPC over three specialized ones: a new derived view is a new `oneof` arm and a new `Feed` value, with no service-definition churn and no new connection. It also lets one client subscribe to several feeds over one stream. Feeds are selected by **presence** in the repeated field, not an enum list, because an empty array otherwise cannot distinguish "subscribe with server defaults" from "not subscribed".

`market` has no default: a proto3 enum has no field presence, so "forgot to set it" and "chose zero" are the same bytes — the request rejects `MARKET_UNSPECIFIED` (§1.3).

### 9.2 Wire conventions

- **All prices/quantities are scaled `int64`.** No doubles, no decimal strings, ever, in server-side computation. Band math and every price comparison happen in exact `int64`.
- **Scale is carried on every message**, not just documented in the proto: `price_scale` / `qty_scale` as exponents (`8` = "divide by 10^8") on every `Update`. A client needs zero out-of-band knowledge to interpret a value, and nothing on the wire changes if a future instrument needs a different scale. `uint32` costs 1 byte as a varint regardless of declared width.
- `symbol` present on every message despite one symbol today — multi-symbol needs no protocol change.
- `seq` + `server_ts_ns` on every message so clients detect conflation and measure end-to-end latency.
- `venue_status` on every message — freshness is part of the data, not a side channel.
- Additive-only evolution: never reorder, resize, or reuse field numbers.
- Server-side conflation is documented in the proto comments, not just the README.

### 9.3 Client services

Three thin binaries over a shared `client_common` (connect, retry with backoff, subscribe, render). Each sets a different `Feed` and formats to stdout: aligned columns, one line per update, `--json` alternative for machine consumption. Client-side gap detection on `seq` is reported to stderr — it demonstrates the conflation contract is understood and honored.

---

## 10. Testing

**294 tests across 23 files, ~3 seconds**, plus ASan/UBSan and TSan builds, all runnable with no host toolchain via `docker compose --profile tests`. `md_core`'s no-I/O, no-clock rule is what makes this reachable: staleness classifiers take `now` as a parameter, so every staleness test is a few integers and an expected enum — no sleeping, no injected fake clock.

| Level | What |
|---|---|
| Unit | Flat book vs. an inline `std::map` reference model (`RefBook`): insert/update/delete, top-of-book, empty sides, snapshot replacement |
| Unit | Band math against hand-computed golden cases — exhausted depth, single-level fill, crossed book, zero-liquidity side |
| Unit | Per-venue continuity: Bybit `u+1` / restart, OKX `prevSeqId` chaining incl. keep-alive and maintenance reset, Binance spot reconciliation and futures `pu` (`test_continuity.cpp`) |
| Unit | Dedup, SPSC queue, venue / instrument registries, config parsing, the ZeroMQ wire codec and both socket ends |
| Property | Randomized multi-level delta streams driven through the flat book and its reference model at once |
| Oracle | Incremental consolidated merge vs. `State::FullRebuild`, asserted after **every** event; incremental BBO vs. a full rescan |
| Integration | The gRPC service over a real in-process server, including routing — a spot subscriber never receives a futures update |
| Benchmark | Binance JSON parser latency (`bench_binance_parser`); book apply, k-way merge, traversal-only, BBO, bytes-moved-per-diff (`bench_md_core`). End-to-end tick→client latency is measured live only (`LatencyRecorder("book_publish")`), not by a benchmark |

**The oracles are the load-bearing part**, because the incremental paths are the dangerous ones: a bug in one does not fail loudly, it accumulates silently over thousands of updates (§5.1). A few tests assert **broken behaviour on purpose**, with a comment: the clearest shows that filtering a stale venue's future quotes cannot remove a price already folded into the incremental BBO — it asserts the price is still there, then shows the rescan removing it, and will fail loudly if someone makes the incremental path handle that transition.

Sanitizer builds (ASan/UBSan/TSan) run in CI. The full suite is green under all three, with one suppressed race inside gRPC's `ThreadManager` teardown — an uninstrumented-library false positive, suppressed via the checked-in `tsan.supp` wired into the TSan build only; `print_suppressions=1` confirms it matches exactly the five `RoutingTest` cases that stand up a real gRPC server. Our own code carries no suppression. The Docker ASan run additionally sets `ASAN_OPTIONS=allow_user_poisoning=0` (in `docker-compose.yml`): our ASan-instrumented objects poison the unused bytes of Abseil inline storage, the uninstrumented vcpkg `.so` files touch that storage without unpoisoning it, and the imbalance shows up as a bogus `use-after-poison` in those same five cases. Our code never calls `__asan_poison_memory_region`, so the option loses no coverage of ours.

Optimization claims in this document are either measured (`bench_md_core`, `bench_binance_parser`, or live instrumentation) or explicitly labelled as estimates — never stated as fact without one.

---

## 11. Deployment

One multi-stage `Dockerfile`: a shared builder stage (the vcpkg dependency layer cached separately from source, so a source edit does not rebuild gRPC/Boost/OpenSSL/Protobuf) and a slim runtime stage holding all three app binaries. One image, several entrypoints, chosen per service by `docker-compose.yml`'s `command:`.

`docker-compose.yml` brings up, per market: one `md-core-<market>`, one `md-provider-<venue>-<market>` per venue, and three clients. Spot runs by default; futures is a `futures` profile — the two markets never share a book (§1.3), and this is the compose-level expression of that. A `tests` profile runs the unit/ASan/TSan targets without a host toolchain.

Config is JSON, not YAML: `user_config/server_config_{spot,futures}.json` and `venues_config.json`, bind-mounted so local edits need no rebuild. There is no capture-and-replay profile (§15) — every service dials the live venues, so running end to end needs outbound access to all three exchanges.

### 11.1 Operational requirements the split build exposed

Not transport details, but the difference between the split build running and only appearing to. All three were found by starting it for the first time; none is reachable by a unit test.

- **stdout must be line-buffered explicitly.** `Logger` writes with `fmt::print` to stdout, which the C runtime makes *fully* buffered whenever stdout is not a terminal — `docker logs`, systemd, any redirect. Each `main` calls `std::setvbuf(stdout, nullptr, _IOLBF, 0)` before anything logs. Without it `md_core_app` produced **zero** readable log lines while serving three clients: its startup and per-1000-publish `[latency]` lines never reached the buffer threshold. Client processes masked it, emitting thousands of lines a minute.
- **The Dockerfile COPY list is a second, silent copy of `CMakeLists.txt`.** Every `add_subdirectory` needs a matching `COPY`; the failure lands at CMake *configure*, which runs after the 30–60 minute vcpkg layer, so a cold build pays the full hour before failing. The COPY list is kept in `add_subdirectory` order so the next omission is visible by inspection.
- **Every client service must pass `--market`.** Required, no default, deliberately (§1.3). A compose service that omits it prints usage and exits, and `restart: unless-stopped` turns that into an invisible crash loop.

---

## 12. Performance

Performance decisions in this project are evidence-based: a suspected bottleneck is measured before it is changed, and the change is measured again. Numbers are labelled **measured**, **estimated** or **hypothesized**.

### 12.1 Headline measurements

| Measurement | Value | Detail |
|---|---|---|
| Consolidation, incremental, alone | **167 ns** | §5.2 |
| 5-level delta, end to end (bench) | **~1670 ns** (6.6× the full merge) | §5.2 |
| `book_publish`, live in Docker | **~50 µs** median | §7.3 |
| Flat book, top-of-book churn | 167 ns, **11× `std::map`** | §5.1 |
| Flat book, 1000-level side traversal | 584 ns | §5.1 |
| `merge_full` (`bench_md_core`, 3 venues, 1000 levels) | 8.5 µs median, 12.0 µs p99 | §14.6 |
| Binance 1000-level snapshot parse | ~140 µs | §14.6 |

The gap between the ~1.7 µs benchmarked publish path and the ~50 µs live one is parsing, the ZeroMQ hop and gRPC serialization — not instrumented, and the place any further optimization should start (§14.6).

### 12.2 Bottlenecks the design addresses

| # | Bottleneck | Mitigation | Checked by |
|---|---|---|---|
| 1 | JSON parsing | simdjson, parse directly into deltas, no DOM, no per-field `std::string`, parser and buffers reused per provider | `bench_binance_parser` |
| 2 | Allocation / copying in the hot path | reused buffers, pre-sized vectors, protobuf arenas; a debug-build allocation counter asserts zero after warm-up (§7.5) | allocation counter + benchmark |
| 3 | Slow-client head-of-line blocking | per-session depth-1 conflation, overwrite-pending (§7.4) | integration test with an artificially slow client |
| 4 | Lock contention on books | single owner thread; no lock at all on the book path (§7.2) | live `lock_wait` instrumentation |
| 5 | Publish amplification (venues × rate × subscribers) | derive bands once per distinct threshold set, share across sessions (§7.3) | `bench_md_core` band-walk cost vs. subscriber count |
| 6 | Cache misses in book traversal | flat contiguous structures over `std::map` (§5.1) | `bench_md_core` book-apply, flat vs. map |
| 7 | Resync storms | backoff with jitter + hysteresis (§4.2) | staleness test suite |

---

## 13. Scaling

"How would this scale?" has a real answer in two dimensions — throughput and geography — and two of the obvious answers are wrong in ways worth stating. **None of the scaling work below is built.** The related 24/7 operational question — add a venue, drop a symbol, deploy a fix, all while the market is live — is answered separately in §14.

### 13.1 The shard key is `(symbol, market_type)` — venue is not part of it

Instruments never reference each other: BTCUSDT's book needs nothing from ETHUSDT. So the symbol is an embarrassingly parallel shard key — no cross-shard locking, linear scaling. **Venue must not appear in the key** — the aggregator's whole job is to merge *across* venues for one symbol; split Binance-BTCUSDT and OKX-BTCUSDT into different shards and there is nothing left to consolidate. Market type *is* in the key (§1.3).

```
shard "BTCUSDT/SPOT"   owns Binance + Bybit + OKX spot books
shard "BTCUSDT/PERP"   owns all three perp books, separately
shard "ETHUSDT/SPOT"   fully independent of both
```

### 13.2 Connections scale with venues, not instruments

One connection per venue, carrying **both** the fast-BBO and the differential-depth streams (§14.2), times `N` for redundancy:

```
sockets = N × 3 venues
```

Adding an instrument adds no sockets — it is another topic subscribed on the same venue connections, and all three venues multiplex many symbols on one connection (Binance combined streams, OKX multiple `args`, Bybit multiple topics). What *does* grow is parse work on the single thread demultiplexing a venue's connection to its per-instrument shards; when that thread saturates, the connection is split into buckets — a config change, not a redesign. §14's production topology instead makes the process boundary `(venue, instrument)` at group size 1, trading this socket economy back for connection-lifetime removal semantics (§14.5).

### 13.3 If provider and core are split across processes on one host

Shared memory would beat a socket transport — no serialization, no payload copy, just a slot write and a release store: an estimated ~100–300 ns hop against ~1–5 µs for a socket (in-process, §7.2, is lower still). §14 nonetheless uses a ZeroMQ socket: for the cost of one dependency it provides message framing, fair-queued fan-in, transparent reconnect and a peer-disconnect event — all of which a shared-memory ring would have to hand-build, and none of which sits on a latency budget that 1–5 µs threatens (§14.4).

### 13.4 Across regions, physics dominates

Light in fiber travels ~200,000 km/s (c ÷ silica's refractive index ≈ 1.47), and real routes run ~1.2–1.5× the great-circle distance:

| link | great circle | one-way (derived) | typical real RTT |
|---|---|---|---|
| NY ↔ London | ~5,600 km | ~33 ms | ~70 ms |
| NY ↔ Tokyo | ~10,900 km | ~65 ms | ~150 ms |
| Tokyo ↔ Singapore | ~5,300 km | ~32 ms | ~70 ms |

A Tokyo provider shipping updates to a New York aggregator delivers data **~35 ms old on arrival**. No amount of shared memory or SIMD changes that by 0.01% — optimizing a 1 µs hop while paying 35 ms of propagation is a 35,000× mismatch. **Geography is a constraint to design around, not an engineering problem to optimize.** What it forces:

1. **Put the aggregator next to the venues, not the clients.** One consolidated book cannot be simultaneously fresh for venues on three continents.
2. **Ship the derived output across regions, never the raw feed.** BBO + bands is a few hundred bytes per update; raw depth is megabytes per second. The gRPC `Update` message already *is* the cross-region payload.
3. **Staleness thresholds become per-venue *and* per-region** (§6.4). A cross-region venue is permanently stale against a threshold tuned for a local one.

For crypto this is easier than it sounds — Binance, OKX and Bybit are all commonly reported to run in Asia-Pacific, so one well-placed aggregator is close to all three. **Confirm by measuring RTT to each endpoint, not by assuming.**

---

## 14. Final production topology

§13 answers "how would this scale?" in throughput and geography. This section answers a different question: **how does this run 24/7?** Crypto never closes, so the operations a daily-restart system does at 04:00 — add a venue, add or drop a symbol, deploy a fix — must all happen while the market is live.

**What is built:** provider and core are **separate processes**, connected by the ZeroMQ transport of §14.4 (`ZmqProviderSink` / `ZmqCoreIngress`), and md_core is venue-blind (§14.3). **What is design:** the Control Manager (§14.1) — what ships in its place is `docker-compose`, which supervises a fixed set of provider and core services rather than spawning them on demand.

### 14.0 Why the process boundary is worth its cost

The split serializes every book update — a wire write where a single process would move a pointer, an estimated 1–5 µs hop (§13.3). At one symbol on one host that cost buys nothing; three production requirements are what justify it:

| Requirement | Cost without the split |
|---|---|
| Add a venue | recompile (`VenueId` is an enum) and restart — every venue and every client gaps |
| Add or drop an instrument | restart — same blast radius |
| Put a provider near its exchange (§13.4) | impossible; one process is on one host |

The blast radius is the real problem: one process would hold all venues, all instruments and all clients for its market, so a change to any part stops all of it. The split buys the ability to add, remove and relocate any part of the system without stopping the rest.

The boundary is cheap **because §9 already drew it.** Providers own parsing, continuity and resync; Core consumes only validated normalized `BookUpdate`. Splitting them replaces an in-process callback with a wire write — it moves no decision from one component to another.

### 14.1 Control plane

Three components; no component reaches into another's decisions:

```
                     ┌──────────────────────────┐
   clients ─────────►│    Control Manager       │   control plane
   (subscribe /      │  valid venue registry    │   restartable
    unsubscribe)     │  refcounts + linger      │   NOT on the hot path
                     │  spawn / SIGTERM         │
                     └────────────┬─────────────┘
                                  │ process lifecycle only
                                  ▼
              ┌──────────────────────┐         ┌──────────────────┐
              │   md_provider (xN)   │────────►│     md_core      │──► gRPC ──► clients
              │  one per (venue,     │  one    │  venue-blind     │
              │  instrument)         │  conn   │  books, merge,   │  data plane —
              │  parse, continuity,  │  each   │  bands, publish  │  runs with the
              │  resync, watchdog    │         │                  │  CM down
              └──────────────────────┘         └──────────────────┘
```

| Component | Decides | Never decides |
|---|---|---|
| **Control Manager** | *lifecycle* — what exists, when it starts and stops | anything about a book or a price |
| **md_provider** | *protocol* — parsing, sequencing, gap detection, resync | whether a venue is admitted to the merge |
| **md_core** | *admission* — which venues enter the merge, and the merged output | anything exchange-specific |

**The Control Manager never sends a message to md_core** — not on add, not on remove, not ever. It manages provider processes; the effect on md_core arrives through the data plane as a connection opening or closing (§14.5). This is the single most important property of the topology: the control plane can be down, restarting or misconfigured with no path by which it corrupts the merged book. While the CM is down: no new subscriptions, no removals; every existing feed keeps flowing. Those are different outages and should never be described as one.

**State.** Which `(venue, instrument, sub_type)` are wanted, and by whom — persisted as one JSON file, written to a temp path and `rename()`d over the original (atomic on POSIX, so crash-safety is free). Not SQLite, not etcd: tens of rows, a few writes per minute, and a file an operator can read during an incident is worth more than a query language.

**Holders are stored as rows, not a count.** A persisted integer cannot be corrected — a client that dies while the CM is down leaves the count permanently too high, and a refcount that only grows never unsubscribes anything. With holder identities, a CM restart plus a grace period garbage-collects dead clients as a side effect of their not reconnecting.

**Client interface.** A client holds a long-lived gRPC stream to the CM; the stream *is* the subscription. First holder for a `(venue, instrument)` spawns the provider; refcount zero starts a ~30 s linger, then `SIGTERM`. The linger absorbs reconnect-after-blip without re-paying the REST snapshot and warm-up. An explicit `Unsubscribe` is an optimisation that skips the linger, never the thing correctness depends on — a crashed client never sends one. Cold start (WS subscribe, REST snapshot, buffering, alignment — a few hundred ms) is a deliberate non-goal, visible to the client as `PENDING → WARMING → LIVE`.

**What ships instead:** `docker-compose`, with every provider and core a fixed service. The design above is kept as the production answer because its one load-bearing property holds regardless of implementation — the CM never messages md_core — so its whole job is process supervision, which an orchestrator's restart policy already does. `compose down` sends `SIGTERM`, exercising the same removal path as a crash (§14.5).

### 14.2 Provider

```
md_provider  (binance, BTCUSDT)
  └── one WS connection
        ├── btcusdt@depth@100ms   -> BookUpdate
        └── btcusdt@bookTicker    -> BboQuote
  └── REST client (Binance only, snapshot seeding — §4.3)
  └── venue staleness watchdog (§6.7)
  └── one connection to md_core
```

Parsing, the sync state machine, continuity, gap detection, resync and the health verdict are all unchanged from §4. The only new thing is that the output leaves over a socket instead of a callback.

**Group size is a knob, not an architecture.** The process boundary is `(venue, instrument_group)`, and this design ships with **group size 1** — which is what makes a subscription's lifetime equal its TCP connection's lifetime (§14.5), removing the need for subscription epochs, explicit remove messages, and any cross-component agreement about when a book dies. All three venues support per-symbol `unsubscribe` on a live connection, so grouping is not blocked by protocol; raising the group size is a config change that buys back socket count (§14.6) at the cost of an in-process teardown path that reintroduces the remove/re-add race and needs a subscription epoch on every update to close it. **That cost, not performance, is why group size 1 is the default.**

**One WS connection carries both BBO and depth.** Earlier drafts used two so the streams could fail independently; at group size 1 that is the wrong trade:

- Connection count is the binding constraint (§14.6), not CPU. One connection per provider halves it — 3 venues × 10 instruments = 30 sockets, not 60 — and halves the connection-*attempt* rate, which matters because OKX rate-limits attempts and a 30-process restart storm is what trips that.
- Head-of-line cost is small at group size 1: deep messages arrive ~10/s for one instrument, so few BBO updates sit behind one. At group size 10 the argument reverses and two connections become correct again.

All three venues support both channels on one connection (Binance combined streams, Bybit two topics in one subscribe, OKX two channels in one `args`). **Built for all three.** `Provider::GetCombinedPath()` is what every provider's one socket dials: the default reuses `GetDepthPath()`, already correct for Bybit and OKX because `venues_config.json` points their `depth_path` and `bbo_path` at the same endpoint (the split was only in the subscribe frame); `BinanceProvider` overrides it because Binance's two paths are genuinely different endpoints. Routing a received message to the right stream is content-based: Bybit peeks `topic`, OKX peeks `arg.channel`, Binance unwraps its `{"stream":...,"data":...}` envelope and routes on `stream`. All three then hand off to the venue's unchanged `OnDepthMessage` / `OnBboMessage`.

**One socket is transport only.** §7 forbids feeding fast-BBO into the depth book, and that is untouched: the two message types stay separate objects (`BookUpdate`, `BboQuote`) on separate wire types. Sharing a TCP connection is not sequencing the two streams together. Per-stream health (§6.5) also survives — watchdogs arm on message arrival per stream *type*, not per socket. **Accepted cost:** socket death now takes both streams down at once, so `depth_health_` and `bbo_health_` become correlated in that one case.

### 14.3 Core is venue-blind

md_core keeps per-venue books, health and attribution, but has **no compile-time list of which venues exist**. It keys on an opaque `venue_slot` — a small dense integer assigned at registration — and never learns the string `"binance"`; the venue name travels only as far as the attribution field on the wire.

Storage is sized at compile time, populated at runtime:

```cpp
std::array<std::unique_ptr<FlatOrderBook>, kMaxVenues> books_;  // slot null until registered
uint8_t active_count_ = 0;                                      // loops run to this
```

A fixed `std::array` with an `active_count_`, not a `std::vector`: a vector resize would invalidate references while the lock-free merge is reading them. `kMaxVenues = 8` is a deliberate documented bound. The hot path is a contiguous array indexed by slot — no indirection.

**Built.** This subsection is code, not plan:

- `VenueRegistry` assigns dense `VenueSlot`s by name, release/acquire so one writer registers while readers run.
- `FlatBookArray`, `VenueQuoteArray`, `VenueHealthArray` are sized `kMaxVenues`.
- `Core::RegisterVenue` / `RemoveVenue` exist; `Init` registers nothing — venues appear when a provider connects (§14.5). `RemoveVenue` deactivates without releasing the slot, frees that venue's book for every instrument, clears its quote and forces a BBO rescan.
- **Slot and `VenueId` are independent** — `Core` holds a `venue_id_to_slot_` table and converts once at each of its three entry points. Registering OKX first gives it slot 0 and everything still resolves.
- `MergeBooks` takes `venue_count` and every per-venue loop runs to it.
- **Attribution carries a `VenueSlot`, not a `VenueId`** — `consolidated::VenueQuote` is `{VenueSlot, QtyUnits}` throughout the merge and BBO. `Core::VenueName(VenueSlot)` is the only way a venue's identity leaves md_core, called **once per published message** to build a `VenueWireTable`, never per level. Failure modes resolve to `VENUE_UNSPECIFIED`, never to a wrong venue.

Carrying the slot rather than the name made attribution **free**: the merge loop's index *is* the slot. Three versions were measured — `static_cast<VenueId>(i)` (free, wrong once slots diverge), `books[i]->venue()` per level (correct, ~3.5–4 µs per merge), the same read hoisted into setup (correct, one array read per level) — and carrying the slot beat all three: `merge_full / iterate_only` went 0.97 → 0.90.

**`venue_count()` is a high-water mark and is never decremented.** Slots are dense and assigned in registration order, so removing a venue leaves a *hole*, not a shorter list — decrementing after removing slot 1 of 3 would make every loop stop at 1 and silently drop slot 2 while the published book still looked well-formed. Removal is `books[i] == nullptr` plus `health kNoData`, which every loop already skips.

**md_core never creates state from a data message.** A book exists because a connection registered it (§14.5), never because an update named it; an update for an unknown instrument or inactive slot is dropped and counted. Otherwise an in-flight update arriving just after a connection closed would silently re-create the book that was just freed.

### 14.4 ZeroMQ transport

**md_core binds one `ROUTER`; every provider connects a `DEALER` over `ipc://`.** The dynamic side (providers, spawned and killed constantly) dials the static side (md_core, always at a known address), so md_core needs no service discovery and no provider list.

**Why ZeroMQ over raw TCP:** for one dependency it delivers messages whole and atomically (no framing state machine), fair-queues across peers (no read-budget logic), and retries `connect()` transparently (no reconnect state machine). `DEALER`/`ROUTER` is the pattern chosen — for transport-assigned peer identity, a disconnect event, and backpressure that blocks rather than drops (at the high-water mark, per peer).

**Removal is the socket closing.** `ZMQ_ROUTER_NOTIFY` gives md_core a disconnect notification delivered in order with that peer's own data, which is what §14.5 rests on. Over `ipc://` a peer that stops existing always closes its descriptor, riding the FIN the kernel sends for `SIGKILL`, a segfault or an OOM kill; `kGoodbye` changes no behaviour, only the log line.

**Identity comes from the transport, not the payload.** `ROUTER` prepends an identity frame the peer cannot forge; md_core binds `venue_slot` to that identity once at `kHello` and attributes every later message by frame — stronger than trusting a venue field inside the message.

**Backpressure adopts the policy `md_core.h` already encodes:**

| Message | On failure to hand over | Why |
|---|---|---|
| `BookUpdate` | refuse; **provider resyncs** | one link in a sequenced chain; continuity belongs to the provider |
| `BboQuote` | drop and **count** | a complete top-of-book snapshot, superseded by the next |
| `VenueHealthEvent` | refuse; escalate | a state transition; a lost `kStale` merges a dead venue forever |
| core → client (§7.4) | conflate | clients receive self-contained snapshots |

A provider must never block indefinitely on `send`: that would stop it draining the exchange socket, fill the kernel receive buffer, and convert a bounded queue problem into an unbounded staleness problem. On the ingress→core hop a full queue gets bounded retry (`TryEnqueueBounded`, 64 spins), then logs and counts as resync-required — one ingress thread feeds every venue's queue, so an indefinite block would head-of-line-stall every other venue. True backpressure to the provider on this hop is a documented gap (§15).

**Wire format.** Frame 0 is the `ROUTER` identity (added and stripped by ZeroMQ). Frame 1 is `[ msg_type:u16 | wire_version:u16 | fixed header ][ contiguous PriceLevel run ]`.

| Type | Payload | When |
|---|---|---|
| `kHello` | wire version, venue name, instrument | once, first message on the connection |
| `kBookUpdate` | flat `BookUpdate` | snapshot or delta (`is_snapshot` distinguishes them — no separate type) |
| `kBboQuote` | flat `BboQuote` | fast-BBO |
| `kHealth` | `VenueHealthEvent` | edge-triggered |
| `kGoodbye` | reason code | clean shutdown only |

**Flat POD, not protobuf:** the provider exists to take a JSON parse off the hot path; a protobuf parse at the core's input gives part of that back. The client-facing gRPC boundary is different — lower rate, external consumers, worth the schema safety. `BookUpdate` cannot cross as it stands (it holds `std::vector<PriceLevel>`); the wire form is a fixed header with `bid_count`/`ask_count` plus one contiguous `PriceLevel` run. `static_assert` on `sizeof` and field offsets fails the build rather than corrupting prices. Decode is zero-copy on read plus one copy into a reused `BookUpdate` whose vectors keep capacity — steady state allocates nothing. No `payload_len` field: `zmq_msg_size()` answers that. **Version at the handshake, not per message** — provider and core deploy separately, so skew is normal during every rolling upgrade; `kHello` carries the version and no overlap refuses the connection loudly. **Additive-only evolution**; an older core reads the prefix it understands and skips the tail. **Assumption, stated:** one architecture and endianness across the fleet.

**Registration.** A provider connecting registers its venue slot; disconnecting removes it (§14.5). The registration is applied on the consolidator thread as it drains that connection's queue, so it lands ahead of that connection's first `kBookUpdate`.

### 14.5 Lifecycle: connection lifetime is subscription lifetime

The mechanism that lets the Control Manager stay out of the data plane. **md_core binds one address; providers dial in, one connection per provider process.** The `kHello` handshake says which `(venue, instrument)` that connection carries, so **accept is registration** and **disconnect is removal**. One connection *per process* is what makes it work: if providers shared a connection, one exiting would close nothing.

**Removal is the disconnect event, never silence.**

| md_core observes | Cause | Book |
|---|---|---|
| Connected, no messages past the backstop | quiet or sick venue (§6.4) | **stale** — excluded from the merge, **kept** |
| `ROUTER_NOTIFY` disconnect | provider process gone | **removed** — freed |

Silence is ambiguous — a quiet market, a dead venue and a deliberate removal look identical from the message stream. A closed socket is unambiguous: the process on the other end no longer exists. If silence removed books, a thirty-second network blip would free the book and force a full REST resync on recovery — the exact damage §6 exists to prevent. Nobody *sends* the disconnect: the kernel closes a dead process's descriptors, so it works for `SIGKILL`, a segfault and an OOM kill.

**Removing a venue does not remove the instrument.** A closed connection frees one `(instrument, venue_slot)`; if other venues still supply that instrument the merge continues, thinner — already the correct behaviour, and what the health logic does when a venue goes `kNoData`. Only when the last venue for an instrument goes does the instrument disappear and publishing stop.

**Crash and deliberate removal share one code path.** md_core cannot distinguish them and does not try — one path, exercised on every restart, deploy and crash, so it cannot rot the way a twice-a-year cleanup path does. The distinction lives in one place: the Control Manager knows whether it sent the `SIGTERM`. A close it ordered is routine; one it did not is an incident. Alerting belongs to the component with the intent, not the one observing the effect. The same principle covers the client end: a subscription lives as long as its gRPC stream to the CM.

**Resync is never requested by md_core.** The provider detects its own gap and repairs it alone (REST/in-channel, align, emit `BookUpdate{is_snapshot=true}`); md_core sees only `is_snapshot=true` and replaces the book. It could not ask for a resync even if that were wanted — §9 strips exchange sequence semantics at the provider boundary. The provider→core connection is one-directional: no reverse channel, no request/response. Escalation instead of a reverse call:

| Rung | Owner | Trigger | Action | Cost |
|---|---|---|---|---|
| 1 | md_provider | its own sequence gap | REST resync, emit snapshot | ~200 ms; core never notices |
| 2 | md_core | stale or uncorroborated venue | exclude from the merge, keep the book | none, reversible |
| 3 | Control Manager | provider unhealthy or silent | `SIGTERM` + respawn | full reset of that venue |

Rung 3 is the heavy resync and needs no new mechanism: killing the provider closes the socket, md_core frees the book, the new process reconnects and rebuilds from a fresh snapshot.

### 14.6 Capacity

Measured (`bench_md_core`, 3 venues, 1000 merged levels):

```
merge_full        median  8.5 µs    p99  12.0 µs
merge_depth_400   median  4.0 µs
qty_update_50     median  0.6 µs
bbo_incremental   median  ~0 ns     p99  42 ns
```

md_core merges eagerly on every depth update, so `merges/s = instruments × depth updates/s`. One consolidator thread saturates at roughly `1 s / 8.5 µs ≈ 118,000 merges/s`. At a generous 200 depth updates/s per instrument, ten instruments is ~2,000 merges/s — **~2% of one core. One thread is enough at ten instruments; do not shard.** §13.1's shard key stays the plan for when it is needed; the number says it is not needed yet.

**Caveat:** the benchmark loop reuses its buffers, so output and venue books stay hot in cache; in production there are milliseconds between merges. Ten instruments is ~1 MB of book (estimate) plus merged output, and a cold merge will run somewhat above 8.5 µs — even at 2× it is ~4% of a core, but it should be **measured at ten instruments, not extrapolated.**

**Parsing is not the limit.** A 1000-level snapshot parses in ~140 µs; deep messages arrive ~10/s, so ~0.14% of a core per instrument, ~1.4% for ten. Where 140 µs bites is a **resync storm** — a venue hiccup makes every instrument refetch at once: ten snapshots is ~1.4 ms of serial parse plus ten concurrent REST calls into Binance's weight limit.

**The binding constraint is venue connection limits.**

```
sockets = venues × instruments × 1 connection = 3 × 10 = 30   (60 without §14.2's single connection)
```

§13 and `config/config.h` record that venue limits are unverified and that exceeding one does not degrade gracefully — the venue refuses or bans the IP, taking down every connection to it at once. The ceiling on instruments per host is set by **connection and connection-attempt limits, not CPU**: merge is ~2% of a core, parse ~1.4%; sockets run out first. The dangerous moment is a **deploy or restart storm** — thirty processes connecting at once, OKX rate-limiting attempts, thirty concurrent REST snapshots hitting Binance's weight limit. Mitigations are operational: staggered start, an outbound IP pool. That number also sets group size (§14.2): raise it when sockets, not CPU, become the limit.

---

## 15. Future Improvements

The current system is designed for a single host, defines three symbols
(`BTCUSDT`, `ETHUSDT`, `SOLUSDT`), and exercises `BTCUSDT` spot and perpetual
against live venues. The following are the main areas for future improvement.

### 15.1 Correctness

- **Cross-venue health checks.** Use activity from other venues to better
  distinguish a quiet market from a failed feed.

- **Health hysteresis.** Require a venue to remain healthy for several
  consecutive health checks before adding it back to the consolidated book.
  This prevents unstable feeds from repeatedly entering and leaving the merge.

- **Publish on health changes.** Publish a new book immediately when a venue
  becomes stale or recovers, even if the market itself is quiet.

- **Independent book validation.** Compare the locally reconstructed BBO with
  the venue's BBO stream where available. A persistent mismatch would trigger
  a resync.

- **Ingress backpressure.** Propagate queue pressure back to the provider
  instead of waiting for the queue to overflow and requiring a resync.

### 15.2 Wire and Clients

- **Dynamic venue metadata.** Remove the fixed venue enum from the wire format
  so that new venues can be added without changing the protocol.

- **Venue status.** Expose the venue health status through the wire protocol.

- **Depth-level venue attribution.** Expose venue attribution for all book
  levels, not only the BBO.

- **Snapshot API.** Add a unary `GetSnapshot` RPC for clients that need the
  current book without subscribing to a stream.

### 15.3 Testing

- **Deterministic replay.** Add replay providers that can reproduce captured
  exchange sessions without a live network connection. This would make
  end-to-end testing and demonstrations deterministic.

### 15.4 Performance

The following are possible optimizations if profiling shows they are needed:

- **More compile-time dispatch** on latency-sensitive paths to allow more
  inlining.

- **Lower-latency WebSocket transport**, such as evaluating uWebSockets against
  the current Boost.Beast implementation.

- **Shared-memory transport** between providers and the core to reduce IPC
  overhead.

- **Tick-indexed order book** if update rates or depth grow beyond the current
  flat-vector design.

### 15.5 Scaling and Operations

- **Sharding.** Split consolidation by `(symbol, market_type)` when measured
  CPU usage justifies it.

- **TCP transport.** Support providers running on other hosts when deployment
  requires it.

- **Control Manager HA.** Add leader election and persistent state if the
  control plane becomes critical.

- **Rolling core upgrades.** Use a blue/green deployment with a warm-up period
  for the new core.

- **Venue maintenance state.** Distinguish planned maintenance from an
  unexpected feed failure.

- **Long-running memory tests.** Measure memory growth over long periods,
  especially because the authoritative consolidated book is not depth-capped.

- **Verify connection limits.** Validate exchange connection limits before
  increasing the number of instruments or venues.

- **Remove unused build configuration.** Retire the obsolete
  `BUILD_SERVER` CMake option.
