# Quick Start

Build, run, configure and test. For the design reasoning and the measurements, see [README.md](README.md).

[Requirements](#requirements) · [1. Clone](#1-clone) · [2. Configure](#2-configure) · [3. Build](#3-build) · [4. Run spot](#4-run-spot) · [5. Run futures](#5-run-futures) · [6. Run a client](#6-run-a-client) · [Configuration](#configuration) · [Services](#services) · [Client examples](#client-examples) · [Running the tests](#running-the-tests) · [Development](#development)

---

## Requirements

**To run (recommended):**

- Docker Engine ≥ 20.10
- Docker Compose v2 (`docker compose`, not `docker-compose`)
- ~8 GB free disk for the build image
- Outbound HTTPS (443) to `binance.com`, `bybit.com`, `okx.com`

Nothing else — no compiler, no vcpkg, no local gRPC.

**To build natively (optional, for development):**

- CMake ≥ 3.20, Ninja, a C++20 compiler
- [vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set


### 1. Clone

```bash
git clone git@github.com:vaxet92/consolidated_book.git
cd consolidated_book
```

### 2. Configure

Nothing has to change to run the defaults — spot BTCUSDT across all three venues, depth 100. To check or change what will be subscribed, edit the JSON under [user_config/](user_config/) before building. See [Configuration](#configuration).

### 3. Build

```bash
docker compose build
```

**The first build takes 30–60 minutes:** gRPC, Boost, OpenSSL and Protobuf all compile from source inside the image. Later builds reuse that layer. The [Dockerfile](Dockerfile) installs vcpkg dependencies *before* copying any source, so editing a `.cpp` never rebuilds gRPC, and a BuildKit cache mount keeps vcpkg's binary cache outside the layer so even a `vcpkg.json` change rebuilds only the new package.

All services share one build definition and one image tag (`consolidated-book:latest`), so the image is built exactly once.

### 4. Run spot

```bash
docker compose up
```

This starts **seven** containers: one core, three providers, three clients. Add `-d` to detach.

```bash
docker compose ps                      # what is up, and what is restart-looping
docker compose logs -f client-bbo      # follow one client
docker compose down                    # clean stop (SIGTERM)
```

### 5. Run futures

Futures live behind a Compose profile, so a plain `up` leaves them out:

```bash
docker compose --profile futures up
```

This starts the spot stack **and** a second, fully independent futures stack on gRPC `50052` — its own core, own providers, own clients, own config file. The two share the image and the socket volume and **nothing else**: no book, no health state, no queue.

### 6. Run a client

Three clients already run inside Compose. To add another without touching the file:

```bash
# against the containerized spot core, on the compose network
docker compose run --rm client-bbo ./client_app \
    --server=md-core-spot:50051 --market=spot --notional_band=100K,1M

# from the host - ports 50051 / 50052 are published
./build/client/client_app --server=localhost:50051 --market=spot --bbo
```

`--market` is **required** and has no default. Spot and futures are separate subscriptions, and the server validates the subscription against the market it serves, so pointing a `--market=spot` client at the futures core is rejected rather than silently served.

---

## Configuration

Runtime configuration is **mounted from the host and is not baked into the image**. Three JSON files, read by fixed name from the working directory — there is no `--config=` flag, because the market is the only thing that selects a file.

Edit:

```
user_config/server_config_spot.json
user_config/server_config_futures.json
user_config/venues_config.json
```

Then restart the affected service — **no image rebuild is required**:

```bash
docker compose restart md-core-spot
docker compose restart md-core-futures
```

**Which service reads which key matters.** Restarting only the core is not always enough:

| Key changed | Restart |
|---|---|
| `venues`, `instruments`, `grpc_port` | the core — `md-core-spot` / `md-core-futures` |
| `depth`, `connections` | the **providers** — `md-provider-*-spot` |
| anything in `venues_config.json` | the providers (and the core if venues changed) |

To restart every service for one market at once:

```bash
docker compose restart $(docker compose ps --services | grep -- -spot)
```

### `server_config_{spot,futures}.json` — the session

```json
{ "venues": ["binance", "bybit", "okx"],
  "depth": 100,
  "connections": 1,
  "grpc_port": 50051,
  "instruments": [ { "symbol": "BTCUSDT", "market": ["spot"] } ] }
```

| Key | Default | Meaning |
|---|---|---|
| `venues` | — | Which venues this stack consolidates |
| `instruments` | — | Symbol + market list. A provider requires **exactly one** of each |
| `depth` | `500` | Desired per-venue depth, rounded **up** to the venue's nearest tier |
| `connections` | `1` | Redundant WebSocket connections per stream, per venue. Capped at 8 |
| `grpc_port` | `50051` | Overridable with `--grpc_port=` |

### `venues_config.json` — the endpoints

One block per `(venue, market)`, so spot and futures never share an endpoint even where the values happen to match today:

```json
"binance": {
  "spot": {
    "ws_host": "stream.binance.com", "ws_port": "443",
    "depth_path": "/ws/{symbol}@depth@100ms",
    "bbo_path":   "/ws/{symbol}@bookTicker",
    "rest_host": "api.binance.com", "rest_port": "443",
    "rest_depth_path": "/api/v3/depth"
  },
  "futures": { "ws_host": "fstream.binance.com", "...": "..." }
}
```

`{symbol}` is replaced with the **venue-formatted** symbol — Binance wants `btcusdt`, OKX futures wants `BTC-USDT-SWAP` — so the formatting stays venue-specific code while the substitution is shared. Bybit and OKX use one fixed path and subscribe over the socket, so their paths carry no placeholder. `rest_*` is empty where a venue needs no HTTP.

The file is **required**. There are no built-in host constants, so a missing file is a startup error rather than a silent fall-back to something stale.

### How the mount works

Every core and provider service (not the clients, which read no config file) bind-mounts the whole directory read-only at the path the image bakes it into:

```yaml
    volumes:
      - ./user_config:/app/user_config:ro
```

A bind mount **replaces** the directory at that path rather than merging with it, so this only works because `user_config/` is checked in with all three files present — a host directory missing one would hide the image's copy of it too, not fall back to it. The image still bakes its own copy (`COPY user_config/ user_config/`), which is what keeps a plain `docker run` with no mount self-contained.

**One trap:** the venue set is expressed twice — as the set of `md-provider-*` services and as the `venues` array. A provider whose venue is *missing from the array* exits with code 2, and `restart: unless-stopped` turns that into an invisible crash loop. The reverse is harmless.

### Command-line arguments

Everything not listed here is a config key.

```
md_core_app     [core_endpoint] --market=spot|futures [--grpc_port=N]
md_provider_app <venue> <market> [core_endpoint]          # positional only, no flags
client_app      --market=spot|futures  <at least one feed flag>
```

---

## Services

Seventeen Compose services, but only three shapes. **A reviewer needs the first block only** — `docker compose up` starts exactly those seven and builds nothing else.

### Default profile — started by `docker compose up`

| Service | Role |
|---|---|
| `md-core-spot` | The spot core: consolidation, gRPC on **:50051** (published to the host), ZeroMQ ROUTER on `ipc:///run/md/spot.ipc` |
| `md-provider-binance-spot` | Binance spot: WS depth + bookTicker, REST snapshot, sequencing, resync |
| `md-provider-bybit-spot` | Bybit spot, same job |
| `md-provider-okx-spot` | OKX spot, same job |
| `client-bbo` | `client_app --bbo` — consolidated best bid/ask with per-venue attribution |
| `client-volume-bands` | `client_app --notional_band=1M,5M,10M,25M,50M` |
| `client-price-bands` | `client_app --price_band=50,100,200,500,1000` |

### `futures` profile — added by `docker compose --profile futures up`

| Service | Role |
|---|---|
| `md-core-futures` | The futures core: gRPC on **:50052**, ROUTER on `ipc:///run/md/futures.ipc`, reads `server_config_futures.json` |
| `md-provider-{binance,bybit,okx}-futures` | Three providers, the perpetual feeds |
| `client-{bbo,volume-bands,price-bands}-futures` | The same three clients, same thresholds, `--server=md-core-futures:50052 --market=futures` |

### `tests` profile — run with `docker compose --profile tests run --rm <service>`

Run-to-completion, not daemons. See [Running the tests](#running-the-tests).

| Service | Role |
|---|---|
| `unit-tests` | `ctest` over the Release build — 284 cases, no extra compilation |
| `asan-tests` | The same suite under AddressSanitizer + UBSan |
| `tsan-tests` | The same suite under ThreadSanitizer, with `tsan.supp` wired in |

These deliberately do **not** use the shared `x-app` anchor: it carries `restart: unless-stopped`, which for a test container would restart a *passing* run forever the moment it exits 0. They set `restart: "no"` and carry their own image tags.

The futures clients mirror the spot three exactly — same binary, same flags, only `--server` and `--market` differ. That symmetry *is* the worked example: pointing a client at the other market costs two flags and no code. Thresholds deliberately match, so the two markets are compared on identical bands.

### Volume

`md-sock` is shared **only** so a core and its providers see the same `ipc://` socket file (two files live in it, `spot.ipc` and `futures.ipc`). It is not for persistence — each socket is recreated on its core's start.

### Operating notes

- **Start order is free.** `depends_on` waits only for the core to *start*, not for its ROUTER to bind. A provider that comes up first queues to the ZeroMQ high-water mark and delivers once the core binds. Clients retry with exponential backoff, so a few seconds of connection errors in client logs at startup are expected.
- **Stopping one provider is a supported operation, not a failure.** `docker compose stop md-provider-okx-spot` closes its socket; the core sees the disconnect, drops that venue from the merge, and re-registers it on `start`. The other two keep publishing, thinner.
- **`docker compose ps`** is where a container in a restart loop shows up.
- Every `main` calls `std::setvbuf(stdout, nullptr, _IOLBF, 0)` — stdout is *fully* buffered when it is a pipe, which is what `docker logs` gives it, and without this the core logged nothing at all while demonstrably serving three clients.
- **Markets cannot be crossed by accident.** A core serves exactly one `InstrumentKey`; `ZmqCoreIngress` refuses a `kHello` for any other market, and the gRPC service rejects a mismatched subscription.

### Reading the core's self-instrumentation

Two lines go to `docker compose logs md-core-spot`:

```
[latency] book_publish n=1000 min=.. median=.. p99=.. max=.. mean=..
          peak_levels[slot0=.. slot1=.. slot2=..] (warmup_discarded=200 unstamped=0 negative=0)
[timing]  n=2000 lock_wait[med/p99/max] book_apply[..] merge[..]
          merged_depth_peak=.. delta_levels avg=.. peak=.. fast_path=..% (n/m)
```

The cadence is **counted in samples, not seconds** — 1000 publishes for `[latency]`, 2000 for `[timing]`, after a 200-sample warm-up each. At roughly 30 updates/second that is about one line a minute; a busier market prints more often, and a dead feed prints nothing at all, which is itself the signal.

- **median / p99 / max, never a mean alone** — the question is where the tail comes from, and a mean hides exactly the outliers being hunted.
- **`unstamped` and `negative` are the instrument checking itself.** They should be `0`.
- **`peak_levels` is reported by slot, not by venue name.** Slots are assigned in registration order, so a name here would eventually label the wrong exchange — match `slotN` against the `venue 'X' registered in slot N` line at startup. It is a **lifetime high-water mark that is never reset**, so a rising sequence is a maximum converging, not a book growing.
- **`fast_path=`** is the share of delta-carrying sides that took the in-place apply rather than a region rebuild — the counter that answers whether the O(delta) fast path is actually being hit live.

---

## Client examples

All three run the **same binary** with different flags. Feeds combine — one client can take every feed over a single stream:

```bash
client_app --market=spot --bbo --volume_bands --price_bands
```

| Flag | Feed |
|---|---|
| `--bbo` | Consolidated best bid/ask |
| `--notional_band=1M,5M,10M` | Volume bands at these thresholds (`K`/`M` suffixes; a bare number is **dollars**) |
| `--volume_bands` | Volume bands at the server's defaults |
| `--price_band=50,100,500` | Price bands at these bps |
| `--price_bands` | Price bands at the server's defaults |
| `--server=host:port` | Default `localhost:50051` |
| `--symbol=BTCUSDT` | Default `BTCUSDT` |

> The output below shows the **format** produced by [client_common.cpp:262-334](client/client_common.cpp#L262-L334) — field order, units and markers — with representative values. It is not a captured run.

### BBO

```bash
client_app --server=md-core-spot:50051 --market=spot --bbo
```

```
seq=41822 BTCUSDT  bid 3.1204 [BINANCE:1.8020,OKX:0.9100,BYBIT:0.4084] : 78310.10 | 78310.60 : 1.7739 [BINANCE:1.2200,BYBIT:0.5539] ask
```

The ladder is mirrored the way an exchange depth display is drawn: the bid side reads *inward* toward the spread (qty, then price), the ask side *outward* from it (price, then qty). That puts the two best prices adjacent on either side of the `|`, so the spread — and a crossed book — is read at a glance. A crossed consolidated book appends `  CROSSED` and is published, not hidden.

Prices print to 2 decimals, quantities to 4. Per-venue attribution is carried at every merged level but only exposed for the BBO.

### Volume bands — VWAP to fill N USDT

```bash
client_app --server=md-core-spot:50051 --market=spot --notional_band=1M,5M,10M,25M,50M
```

```
seq=1873 BTCUSDT  volume bands  (best bid 78310.10 / ask 78310.60)
  BID     1M  vwap 78308.44  worst 78305.20  qty 12.7712  lvls   14  slip 0.2bps
  BID     5M  vwap 78299.10  worst 78288.00  qty 63.8571  lvls   61  slip 1.4bps
  BID    50M  vwap 78201.55  worst 77990.10  qty 639.3702  lvls  418  slip 13.8bps  INSUFFICIENT DEPTH (filled 41.2M)
  ASK     1M  vwap 78312.05  worst 78315.40  qty 12.7706  lvls   11  slip 0.1bps
  ASK     5M  vwap 78321.66  worst 78334.90  qty 63.8404  lvls   57  slip 1.4bps
```

`slip` is the VWAP's distance from the BBO in bps — the cost of sweeping that much size. `filled_qty` is only shown as a shortfall when the book ran out: it equals the target by construction otherwise, so it would be a column of noise. **`INSUFFICIENT DEPTH` is not optional** — on the wire a truncated result is otherwise indistinguishable from a complete one.

### Price bands — liquidity within X bps of the BBO

```bash
client_app --server=md-core-spot:50051 --market=spot --price_band=50,100,200,500,1000
```

```
seq=1871 BTCUSDT  price bands
  BID     50bps  limit 77918.55  vwap 78240.31  qty 401.2210  lvls  287  notional 31.3M
  BID    100bps  limit 77527.00  vwap 78180.44  qty 588.7741  lvls  402  notional 46.0M  BOOK EXHAUSTED (lower bound)
  ASK     50bps  limit 78702.15  vwap 78380.90  qty 377.0044  lvls  266  notional 29.5M
  ASK   1000bps  limit 86141.66  vwap 78455.12  qty 611.3388  lvls  431  notional 47.9M  BOOK EXHAUSTED (lower bound)
```

The 1000 bps band is 10% from the BBO — deeper than any public channel reaches — so wide bands are *always* truncated and marked. Bands are measured **from the BBO**, not the mid; see [Key design decisions](README.md#key-design-decisions).

### Detecting conflation

Clients track their own `seq` and report gaps on stderr, because a gap is information rather than an error:

```
[gap] 6343 update(s) conflated away over 10015 published (63%)
```

---

## Running the tests

**In Docker — no toolchain required.** Three run-to-completion services behind a `tests` profile. A profile gates the *build* as well as the run, so `docker compose build` and `docker compose up` never touch them:

```bash
docker compose --profile tests run --rm unit-tests    # 284 cases, ~3 s
docker compose --profile tests run --rm asan-tests    # ASan + UBSan
docker compose --profile tests run --rm tsan-tests    # TSan
```

Each exits with the suite's status, so they drop straight into CI.

### What each one costs

| Service | Image | Extra build |
|---|---|---|
| `unit-tests` | `consolidated-book-tests:latest` | **none** — see below |
| `asan-tests` | `consolidated-book-asan:latest` | full Debug + ASan rebuild of the project sources |
| `tsan-tests` | `consolidated-book-tsan:latest` | full Debug + TSan rebuild of the project sources |

**`unit-tests` is free.** `BUILD_TESTS` defaults to `ON` and `tests/` is copied into the builder, so the Release pass that produces `md_core_app` *already* builds `unit_tests` — it simply never reaches the slim runtime image, which carries three app binaries and nothing else. The `tests` stage is `FROM builder` plus a command. If the main image is built, this starts in seconds.

**The sanitizer services do pay.** ASan and TSan are mutually exclusive compile flags, so neither can share a build tree with the other or with Release — each needs its own configure and a full recompile of the project's translation units at `-O0 -g`. What they *do* reuse is the expensive part: both stages sit on the `builder`, so the 30–60 minute vcpkg layer is untouched and gRPC, Boost and OpenSSL are never rebuilt.

All four images come from the one `Dockerfile` and the one build context.

### Two things that differ from the local sanitizer builds

**Leak detection is on here and off locally.** LeakSanitizer is unsupported on macOS arm64, so `cmake --preset vcpkg-arm64-asan` finds memory *errors* but never leaks. On Linux it is on by default, so `asan-tests` checks something the local build cannot — and may report leaks inside the uninstrumented vcpkg libraries. It is left on rather than quietly disabled. For memory errors only:

```bash
docker compose --profile tests run --rm -e ASAN_OPTIONS=detect_leaks=0 asan-tests
```

**Both sanitizer services relax two container defaults**, via `security_opt: seccomp:unconfined` and `cap_add: SYS_PTRACE` in the Compose file. The sanitizer runtimes map a large fixed shadow region and call `personality()` to disable ASLR; Docker's default seccomp profile blocks that, and the result is an immediate `unexpected memory mapping` at startup rather than a test failure.

They run under `ctest` rather than the gtest binary directly because [tests/unit_tests/CMakeLists.txt](tests/unit_tests/CMakeLists.txt) attaches `TSAN_OPTIONS=suppressions=.../tsan.supp` as a ctest *property* when `SANITIZER=thread`. Invoking `./unit_tests` by hand drops that and reports gRPC's `ThreadManager` teardown — an uninstrumented-library false positive — as a real race.

### Filtering

`run` takes a replacement command, so any gtest or ctest filter works:

```bash
docker compose --profile tests run --rm unit-tests \
    ctest --test-dir build --output-on-failure -R FlatOrderBook

docker compose --profile tests run --rm unit-tests \
    ./build/tests/unit_tests/unit_tests --gtest_filter='FlatOrderBookTest.*'
```

### Cleaning up

The three test images are large — they keep the builder filesystem, because `ctest` needs CMake and the build tree:

```bash
docker image rm consolidated-book-tests:latest \
                consolidated-book-asan:latest \
                consolidated-book-tsan:latest
```

---

## Development

### Native build

```bash
export VCPKG_ROOT=~/vcpkg          # after bootstrapping vcpkg
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build -j
```

Or via the checked-in presets (Ninja):

```bash
cmake --preset vcpkg-arm64          # also: vcpkg-arm64-debug, -asan, -tsan
cmake --build build -j
```

`md_core` and `types` depend on nothing from vcpkg — that is what keeps the domain logic testable — but the top-level build always configures the network components. To build the domain library alone:

```bash
cmake --build build --target md_core
```

Running it without Docker:

```bash
./build/aggregator/md_core_app     ipc:///tmp/md.ipc --market=spot
./build/aggregator/md_provider_app binance spot ipc:///tmp/md.ipc
./build/client/client_app          --market=spot --bbo
```

### Tests

`BUILD_TESTS` defaults to `ON`. 284 cases across 23 files, about 3 seconds.

```bash
cmake --build build --target unit_tests -j
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -R FlatOrderBook

./build/tests/unit_tests/unit_tests --gtest_filter='FlatOrderBookTest.*'
./build/tests/unit_tests/unit_tests --gtest_filter=FlatOrderBookTest.RandomMultiLevelDeltasMatchTheOracle
```

### Sanitizers

```bash
cmake --preset vcpkg-arm64-asan && cmake --build build_asan -j
ctest --test-dir build_asan --output-on-failure     # ASan + UBSan

cmake --preset vcpkg-arm64-tsan && cmake --build build_tsan -j
ctest --test-dir build_tsan --output-on-failure     # TSan
```

TSan is clean apart from one suppressed race inside gRPC's `ThreadManager` teardown — an uninstrumented-library false positive, suppressed via the checked-in [tsan.supp](tsan.supp), wired into the tsan build only. `print_suppressions=1` confirms it matches exactly the five `RoutingTest` cases that stand up a real gRPC server. Our own code carries no suppression.

### Benchmarks

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON \
      -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build-bench -j
./build-bench/benchmarks/bench_md_core
./build-bench/benchmarks/bench_binance_parser
```