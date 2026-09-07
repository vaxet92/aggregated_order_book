# Consolidated Market Data

Aggregates BTCUSDT, ETHUSDT and SOLUSDT market data from three exchanges — Binance, OKX and Bybit — into one consolidated order book, and publishes derived views over gRPC: consolidated BBO, notional volume bands (VWAP to fill N USDT), and bps price bands (liquidity within X bps of the BBO). Spot and futures both run, as two fully independent stacks that never share a book. Providers own all exchange-specific protocol handling — sequencing, gap detection, resynchronization — so the core consumes only normalized, validated updates and contains no networking, no I/O and no clock. The whole system runs under Docker Compose against live exchange data.

C++20 · CMake + vcpkg · Boost.Beast/Asio + OpenSSL · simdjson · ZeroMQ · gRPC + Protobuf · GoogleTest.

> **Status: working end-to-end against live exchange data.** All three feeds publish, spot and futures both run.

### → To build and run it: [QuickStart.md](QuickStart.md)

Requirements, `git clone` through a running client, configuration, every Compose service, client output examples, how to run the tests and sanitizers, and the native build. **This file is the reasoning**: what was measured, what was chosen, and what is knowingly missing.

---

## Contents

[Architecture](#architecture) · [Supported venues](#supported-venues) · [Performance](#performance) · [Design](#design) · [Test coverage](#test-coverage) · [Known limitations](#known-limitations) · [Project structure](#project-structure)

Also: [QuickStart.md](QuickStart.md) · [DESIGN.md](DESIGN.md)

---

## Architecture

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
                        │  gRPC, per-session conflation            │
                        └──────────────────┬───────────────────────┘
                                           │ server-streaming gRPC
                        client_app  ×  N subscribers
```

Three binaries, one image:

| Binary | What it does |
|---|---|
| `md_core_app` | Consolidation and publish: `Core` + gRPC server + one ZeroMQ ROUTER. **One per market.** No exchange connections of its own |
| `md_provider_app` | One provider for one `(venue, market)`: connects to the exchange, dials a core's ROUTER over `ipc://` |
| `client_app` | The client. Any combination of feeds, selected by flags |

**The layering rule that makes everything else work: `md_core` has no networking, no I/O and no clock.** Exchange-specific semantics — Binance's `U`/`u`/`pu`, Bybit's `u`, OKX's `seqId`/`prevSeqId` — are stripped at the provider boundary. The core consumes only a normalized, validated `BookUpdate`, which is why the interesting logic is testable with three integers and an expected result.

---

## Supported venues

All three venues are used for **both** markets. `BTCUSDT` spot and the `BTCUSDT` perpetual are different instruments and never enter the same book. Three symbols are defined — `BTCUSDT`, `ETHUSDT`, `SOLUSDT` — selected per stack in config; `BTCUSDT` is the one run against live data.

| Venue | Spot | Futures | Depth stream | Fast-BBO stream | REST needed | Depth tiers |
|---|:---:|:---:|---|---|---|---|
| **Binance** | ✅ | ✅ | `@depth@100ms` (differential only) | `@bookTicker` | **Yes** — snapshot, `/api/v3/depth` | 5, 10, 20, 50, 100, 500, 1000, 5000 |
| **Bybit** | ✅ | ✅ | `orderbook.N` on `/v5/public/{spot,linear}` | `orderbook.1` | No | 1, 50, 200, 1000 |
| **OKX** | ✅ | ✅ | `books` on `/ws/v5/public` | `bbo-tbt` | Futures only — `ctVal` from `/api/v5/public/instruments` | 1, 5, 400 |

Two venue-specific traps the code handles explicitly:

- **Binance never sends a snapshot on the depth stream.** The book is seeded from REST, and the stream is buffered *before* the fetch so nothing in between is lost.
- **OKX quotes SWAP sizes in contracts, not coins.** At `ctVal 0.01` a level of `495.94` is 4.9594 BTC. Left unconverted, OKX enters the consolidated futures book 100× oversized and dominates every price level — which is why `rest_instruments_path` is fetched for OKX futures.

**Depth is not a free parameter.** Each venue publishes at fixed tiers and `depth` rounds *up* to the smallest tier that covers the request. OKX effectively has one usable tier — nothing between 5 and 400, nothing above 400 without VIP4 — so `depth: 800` gives Binance 1000, Bybit 1000 and OKX still 400. A venue that cannot honour the request logs it rather than hiding it.

---

## Performance

`bench_md_core`, Apple M4 Pro, Release, medians over 20 000 iterations.

| Consolidation (incremental) | |
|---|---|
| 5-level delta, end to end | ~1 670 ns |
| consolidation alone | 167 ns |

| Per-venue book operation | Flat book |
|---|---|
| traversal of a 1000-level side | 584 ns |
| top-of-book churn, 20 erase + 20 insert | 167 ns |
| the same churn 500 levels deep | 3 333 ns |
| quantity-only delta, 50 levels | 84 ns |

### What the 6.6× is, and what it is not

The consolidated book used to be re-derived from scratch on every update: a k-way merge across every venue book, producing every output level, every time. The incremental path instead *repairs* the consolidated book from the level transitions the one changed venue reported. Five levels moved means five levels reconsidered, not three thousand.

**The measurement that drove this said the opposite of the assumption.** The intuition was that the merge was read-bound — walking three venue books is the visible work. It is not: traversing the venue books is 584 ns of a 9 500 ns merge, and the merge is **write-bound roughly 4.6:1**. The waste was re-deriving ~3000 output levels when one venue moved five, not re-reading the input. That is also why **prefix sums were removed**: every level used to carry `cum_qty`/`cum_notional`, read only by the band walks — which already traverse forward from index zero, so the stored prefixes served a random-access query nobody made. Free under a full merge, prohibitive under an incremental one, because a change at the *top* invalidates every prefix below it and updates cluster at the top. Bands now accumulate as they walk.

**The 6.6× is real in the benchmark and did not show up live.** Live `book_publish` is **~50 µs median**; the optimization removed about 1.7 µs of that path. Three caveats are stated rather than discovered: the benchmark's tight loop keeps data in cache while live there are ~40 ms between merges, so it flatters every hot-path number; live samples taken on different days are not controlled comparisons (three medians vary 40% among themselves); and `peak_levels` is a lifetime high-water mark that `Report()` never resets, so a rising sequence is a maximum converging, not a growth curve.

**The live bottleneck is therefore not identified, and it is upstream of everything optimized so far.** Two independent pointers agree: `book_publish` is ~50 µs live against ~1.7 µs benchmarked, and `book_apply` is 4.15 µs live against ~100 ns benchmarked for deltas of the observed 8–11 levels. That gap is parsing and transport, and it is not instrumented. The `fast_path=` counter narrows it: Binance sends `qty=0` removals constantly, and any level entering or leaving the book routes to the region rebuild rather than the in-place apply, which is the likeliest reason live applies miss the fast path.

The real argument for the flat book is **shape, not size**: applying a delta is O(delta), not O(book). The live Binance book grows without bound — `peak_levels` over one run: `580 604 621 … 913`, monotonic — so an apply that scales with book size degrades indefinitely on that feed. Deep structural churn is the one case that is genuinely worse (3 333 ns, 20× the shallow case), and it is measured and stated rather than left out.

---

## Design

Full reasoning, rejected alternatives and the experiment log:

→ **[DESIGN.md](DESIGN.md)** — detailed architecture, invariants, implementation status, rejected alternatives and the experiment log

### Key design decisions

- **Numbers are scaled integers — `uint64` × 1e8, no floating point anywhere** in the book or the protocol. Consolidating two venues at "the same price" is an *equality* comparison, and `78310.10` is not exactly representable as a double. Band accumulation runs in `unsigned __int128`: a 50M USDT band at 1e8×1e8 is ~5e23, past `uint64` and past a double's exact-integer range alike.

- **The per-venue book is two reverse-ordered flat vectors — worst price first, best at `back()`.** `back()` is the only end of a vector that is cheap to grow and shrink, and nearly every update lands at the top of book. At `front()` a new best bid memmoves the whole book (~16 KB at 1000 levels); at `back()` it is a `push_back`. Readers walk backwards, which prefetchers handle for free.

- **A stale venue is excluded from the merge, not merely reported.** The merge takes `max(bid)` and `min(ask)`, so a frozen venue *always* looks like the best bid when the market falls and *always* like the best ask when it rises. Staleness is not noise that averages out — the merge actively selects for it. A test shows a frozen venue at 50000 against live venues at 49900 producing a **crossed** book: a phantom 90-tick arbitrage nobody can trade. Admission is written `== kLive`, an allow-list not a deny-list, so any state added later defaults to excluded.

- **Binance futures chains on `pu`, not `U`, and the reason is measured.** Futures `U`/`u` come from a counter shared across *every* symbol. Live, 2026-09-05: consecutive BTCUSDT messages showed `U` 68–323 above `last_u + 1` with nothing missing — the spot rule read each as a gap, producing **14 resyncs in 14 seconds**. `pu` answers "what did I last send you for *this* symbol".

- **A gap means the book is *wrong*, which is worse than stale**, so the provider drops the book and resyncs alone; the core is never asked. Resync deliberately does not consume the reconnect-attempt budget — it is a normal operational event, not a connection failure.

- **Conflation: newest state wins.** Each session holds a depth-one pending slot with overwrite semantics — a new push replaces an unread one, nothing queues, nothing allocates, nothing upstream blocks on a slow reader. This is safe only because an order book is a **state, not an event log**. It forces the related choice that **every message carries full state, not a delta**, since a client that skipped one could not apply the next. `seq` is per session, so a gap means *this* client conflated something.

- **Crossed consolidated books are published with a `crossed` flag, not silently uncrossed.** It is the true consolidated state and the signal an aggregator exists to show — and the only reversible choice, since a client given the raw book can compute a clean one but never the reverse. Band math is written to tolerate a crossed book, and it is a required test case.

- **Price bands are measured from the BBO, not the mid**, because the assignment says "BBO+"; there is deliberately no flag for the alternative. Volume bands are the **VWAP to fill N notional**, not "the price at which cumulative notional crosses N" — the latter is a strictly weaker statement and not what an execution consumer wants.

- **Spot and futures separation is structural, not a check that could be forgotten.** Every book, health array and queue is keyed by `InstrumentKey`, packing symbol *and* market into one `uint32_t`, so no code path could merge them. The gRPC request rejects `MARKET_UNSPECIFIED`, because a proto3 enum has no presence — "forgot to set it" and "chose zero" are the same bytes.

- **One `Subscribe` streaming RPC with a `oneof` payload**, not one RPC per view: a new derived view becomes a new `oneof` arm and nothing else changes. Feeds are selected by **presence**, not an enum list, because an empty array otherwise cannot distinguish "subscribe with server defaults" from "not subscribed".

- **There is no lock on the book path** — no mutex, no seqlock, no atomics on book state. Exactly one consolidator thread is ever inside the book logic, so it is deterministic, testable with fake input, and TSan-clean by construction rather than by care. The two remaining mutexes are deliberate and elsewhere: the client list during fan-out, and the condition variable that lets an idle gRPC writer sleep.

---

## Test coverage

**294 test cases across 23 files, ~3 seconds**, plus ASan/UBSan and TSan builds — all three runnable with no toolchain via `docker compose --profile tests run --rm unit-tests` ([how](QuickStart.md#running-the-tests)). `md_core`'s no-I/O, no-clock rule is what makes this reachable: staleness classifiers take `now` as a parameter, so every staleness test is three integers and an expected enum — no sleeping, no injected fake clock.

| Level | What |
|---|---|
| Unit | Flat book vs. an inline `std::map` reference model: insert/update/delete, top-of-book, empty sides, snapshot replacement |
| Unit | Band math against hand-computed golden cases — exhausted depth, single-level fill, crossed book, zero-liquidity side |
| Unit | Per-venue continuity: Bybit `u+1`/restart, OKX `prevSeqId` chaining incl. keep-alive and maintenance reset, Binance spot reconciliation and futures `pu` |
| Unit | Dedup, SPSC queue, venue/instrument registries, config parsing, the ZeroMQ wire codec and both socket ends |
| Property | Randomised multi-level delta streams driven through both implementations at once |
| Oracle | Incremental consolidated merge vs. `State::FullRebuild`, asserted after **every** event; incremental BBO vs. a full rescan |
| Integration | The gRPC service over a real in-process server, including routing — a spot subscriber never receives a futures update |

**The oracles are the load-bearing part**, because the incremental paths are the dangerous ones: a bug in one does not fail loudly, it accumulates silently over thousands of updates. The flat book's oracle paid for itself on the first day of optimization — the first in-place delta application chose its walk direction from the *net* size change, which is wrong when one delta contains both an insert and an erase. The corrupted book was still sorted and still exactly the right length, with one level duplicated. No assertion, no crash, no sanitizer finding: the write was in bounds. Only a full-sequence comparison against an independent implementation can see that.

A few tests assert **broken behaviour on purpose**, with a comment saying why — the clearest shows that filtering a stale venue's future quotes cannot remove a price already folded into the incremental BBO. It asserts the price is still there, then shows the rescan removing it, and it will fail loudly if someone ever makes the incremental path handle transitions.

---

## Known limitations

**Not built:**

- **Cross-venue corroboration** — the disambiguation that a quiet market is market-wide while a dead feed is per-venue. It is the only sub-minute staleness signal for Binance, which sends no keepalive at all. **The main correctness gap.**
- **Hysteresis on re-admission**; recovery from stale waits for the next watchdog tick.
- **Republish on a health change** — a venue going stale in a *quiet* market leaves the client holding a book that still includes it, because nothing triggers the next merge.
- **`VenueStatus` on the wire.** The field exists and is never populated: bands and clients were finished before the staleness policy.
- **The fast-BBO oracle** — forcing a resync when the depth-derived BBO disagrees with the venue's own BBO stream. This matters more than planned: OKX **deprecated its CRC32** (the field remains, fixed at 0), so no venue now offers a per-message check against its own book. Gap detection catches *missed* messages, not *misapplied* ones.
- **A shared tick grid.** Nothing validates that venue ticks divide evenly onto a canonical grid.
- **Record and replay** — dropped deliberately, since `md_core`'s no-I/O rule already gives deterministic testing for the logic that matters. The cost is that nothing exercises the whole pipeline deterministically.
- **Per-session reconnect.** With `connections > 1`, a dead socket is replaced only once *every* socket is gone. The loss is logged; nothing acts on it.
- **Unary `GetSnapshot`**, and `throttle_ms` — the latter removed from the proto rather than left inert.

**Working, with caveats:**

- `connections > 1` has not been run against live venues; venue connection limits are unverified. Default is 1 so the shipped configuration cannot trip one.
- Single symbol in practice, though the code and protocol are parameterized for more.
- The 1000 bps band cannot be covered by public depth channels at any setting — reported with `insufficient_depth` rather than presented as complete.
- Venue attribution is carried at *every* merged level but only exposed for the BBO.
- The published book is a `const` reference valid **only during the callback**, not a snapshot. Correct for every consumer today, and a real constraint on the next one; the migration path — versioned buffers caught up by replaying `LevelChange` — is designed, not built.
- Consolidated state is deliberately not depth-capped (capping would silently shrink the book after an erase), so memory tracks the union of the venue books and Binance's grows without bound. **Not measured over a long run** — the first thing to watch in a soak test.
- No authentication or TLS on the gRPC connection. No persistence. Exchange fees are not modelled, though taker fees are often larger than the cross-venue spread.

---

## Project structure

| Directory | Contents | Depends on |
|---|---|---|
| [types/](types/) | `VenueId`, `MarketType`, `InstrumentKey`, registries, staleness constants | nothing |
| [md_core/](md_core/) | Domain: `FlatOrderBook`, `consolidated::State`, BBO, bands, `VenueHealth`, SPSC queue, `Core` | `types` only — **no I/O, no networking, no clock** |
| [md_provider/](md_provider/) | WebSocket, REST, the three parsers, continuity, dedup, watchdog | Boost, OpenSSL, simdjson |
| [md_wire/](md_wire/) | ZeroMQ codec and both socket ends (`ZmqProviderSink`, `ZmqCoreIngress`) | ZeroMQ |
| [md_proto/](md_proto/) | The `.proto` and generated stubs | Protobuf, gRPC |
| [aggregator/](aggregator/) | `md_core_app`, `md_provider_app`, the gRPC service, conflated channel, latency recorder | all of the above |
| [client/](client/) | `client_app` and `client_common` — connect, retry, subscribe, format | gRPC |
| [config/](config/) | JSON config **loading and validation** (C++, not the JSON itself) | nothing |
| [user_config/](user_config/) | The three runtime JSON files, bind-mounted into every core and provider | — |
| [tests/](tests/), [benchmarks/](benchmarks/) | GoogleTest suite; `bench_md_core`, `bench_binance_parser` | |

### Threads, and what each may touch

| Thread | How many | Owns / may touch |
|---|---|---|
| Venue `io_context` | one per provider process | Sockets, parser, dedup state, continuity state. **Never touches a book** |
| Provider watchdog | the same `io_context`, a 1 s `steady_timer` | Liveness stamps, health verdicts |
| Core ingress | one per core | Decodes wire frames, pushes into the per-venue SPSC queues |
| **Consolidator** | **exactly one per core** | **Every book**: venue `FlatOrderBook`s, `consolidated::State`, BBO, health array. Runs the merge and the publish callback |
| gRPC session | one per subscribed client | Its own `ConflatedChannel` slot and its own `seq` |
