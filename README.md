# Limit Order Book & Matching Engine

[![CI](https://github.com/Nurassyl707/limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/Nurassyl707/limit-order-book/actions)

A header-only **C++17** limit order book and matching engine with strict
**price-time priority**, tested with **GoogleTest** and benchmarked/profiled on
Linux with **perf**.

* Price priority: better prices match first. Time priority: FIFO within a price.
* Trades execute at the **resting** order's price.
* Order types: GTC limit, IOC limit, market. Operations: add, cancel, modify.
* Two interchangeable price-level containers behind one matching engine, so the
  optimization is measurable in isolation:
  * `MapOrderBook` - `std::map<Price, Level>` (baseline)
  * `ArrayOrderBook` - tick-indexed flat array with a cached best price (fast)

## Quick start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # unit tests
./build/demo                                 # tiny walkthrough
./build/bench_book                           # 1M-order benchmark
```

GoogleTest is used from the system if installed, otherwise fetched automatically.
Add `-DLOB_SANITIZE=ON` for AddressSanitizer + UBSan.

```cpp
#include "lob/order_book.hpp"
using namespace lob;

ArrayOrderBook book(/*min_price=*/1, /*max_price=*/100000);   // prices are integer ticks
book.set_trade_handler([](const Trade& t) { /* t.maker_id, t.taker_id, t.price, t.qty */ });

book.add_limit(1, Side::Sell, 10100, 50);
book.add_limit(2, Side::Buy,  10100, 20);       // trades 20 @ 10100 against order 1
book.modify(1, 10100, 10);                      // reduce qty in place (keeps queue position)
book.cancel(1);
```

## Design

```
OrderBook<Policy>
 ├─ bids_, asks_ : price levels  (Map: std::map  |  Array: Level[max-min+1])
 │     Level { head, tail, total }  ── FIFO queue as an intrusive doubly linked list
 ├─ nodes_       : vector<Node> order pool, indices instead of pointers, free list reuse
 └─ ids_         : IdMap (open-addressing hash, OrderId -> pool index) for O(1) cancel
```

| Operation        | `std::map` levels | tick-array levels |
|------------------|-------------------|-------------------|
| find/create level| O(log L)          | O(1)              |
| best price       | O(1) (begin)      | O(1) (cached)     |
| best level empties | O(log L) erase  | O(gap) scan toward spread |
| cancel           | O(log L)          | O(1)              |

`L` = number of populated price levels. The array trades memory
(`12 B x ticks x 2 sides`) and a bounded price range for constant-time access and
far better cache behaviour.

Other deliberate choices: integer prices (no floating point), a single-threaded
core (the usual one-writer-per-instrument design, no locks on the hot path), and
order nodes stored contiguously in a pool to avoid per-order heap allocation.

## Testing

`tests/test_order_book.cpp` contains **20 GoogleTest cases**, each instantiated
for *both* book implementations (40 runs). They cover price/time priority,
partial fills, multi-level sweeps, IOC/market semantics, cancel (including
mid-queue), modify (priority rules), validation/rejects, sparse-level best-price
tracking, and a **randomized property test** that replays 20k-order streams
against a naive O(n) reference matcher and requires identical trades.

The suite also passes under AddressSanitizer + UBSan, and I verified it fails when
bugs are deliberately injected (non-crossing trades, stale best-price index).

## Benchmark

`bench/bench_book.cpp` replays the same pre-generated 1M-order stream (60% passive
limits, 10% marketable limits, 5% market, 25% cancels; random-walk mid price)
through three implementations and checks that all produce identical trades:

* **naive** - textbook `std::map<Price, std::list<Order>>` + `std::unordered_map` (`bench/naive_book.hpp`)
* **map** - this library with `std::map` levels (pool + flat id map)
* **array** - this library with tick-indexed levels

Measured on a single shared vCPU (GCC 13, `-O3`, best of 5; expect +/-10% noise
and different absolute numbers on your hardware). "Depth" is the mean distance of
resting orders from the mid price, i.e. how spread out the book is:

| depth (ticks) | naive M ops/s | map M ops/s | array M ops/s | array vs map | array vs naive |
|---:|---:|---:|---:|---:|---:|
| 20   | 9.03 | 11.65 | 19.72 | 1.69x | 2.18x |
| 150  | 6.75 | 8.76  | 15.48 | 1.77x | 2.29x |
| 600  | 5.37 | 6.73  | 14.27 | 2.12x | 2.66x |
| 2000 | 4.24 | 5.71  | 13.25 | 2.32x | 3.13x |

The advantage of the array grows as the book gets deeper because `std::map` pays
O(log L) pointer-chasing per level operation while the array stays O(1).
Reproduce with `./build/bench_book --depth 600`.

## Profiling with perf

```bash
scripts/profile.sh map     # then
scripts/profile.sh array   # compare IPC, cache-misses, and hot symbols
```

The script runs `perf stat` (cycles, IPC, branch/L1/LLC misses) and
`perf record -g` + `perf report`, writing results to `perf-results/`.
The benchmark is built with `-fno-omit-frame-pointer -g` so call stacks resolve.
See `docs/PROFILING.md` for how to interpret the output.

## Layout

```
include/lob/   types.hpp  id_map.hpp  price_levels.hpp  order_book.hpp
tests/         test_order_book.cpp
bench/         bench_book.cpp  naive_book.hpp
examples/      demo.cpp
scripts/       profile.sh
```

## Limitations / ideas for extension

Single instrument and thread; no FOK/post-only/stop orders or self-trade
prevention; array range is fixed at construction; trade callback is a
`std::function` (a template parameter would remove that indirection). Natural next
steps: depth snapshots, a binary feed replayer, multi-instrument sharding, and a
bitset hierarchy over the tick array to make best-price scans O(1).

## License

MIT
