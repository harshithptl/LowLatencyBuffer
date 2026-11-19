# LowLatencyBuffer

A price-time priority matching engine fed through a lock-free single-producer single-consumer (SPSC) ring buffer, with a slab allocator and a pointer-recycle ring keeping the hot path free of `malloc` and `new`.

Built and benchmarked on an Apple M4 under macOS. The data structures are portable C++; the benchmark's timer (`cntvct_el0`) and thread-placement code are ARM/macOS-specific.

## Result

Submit-to-book latency, P50: **5.2 to 8.7 µs**, best run 5.2 µs. Average time inside the book per operation: **1.1 to 1.6 µs**.

| Run | P50 | P99 | Book op mean |
|---|---|---|---|
| 1 | 2,030 µs (outlier, see below) | 4,021 µs | 1.59 µs |
| 2 | 6.9 µs | 942 µs | 1.18 µs |
| 3 | 8.7 µs | 247 µs | 1.50 µs |
| 4 | 6.9 µs | 741 µs | 1.23 µs |
| 5 | 5.2 µs | 591 µs | 1.12 µs |

What these numbers are:

- **Submit-to-book** is measured from the producer's timestamp, taken just before it pushes into the ring, to the moment the consumer finishes the book operation. It includes the cross-core handoff, which dominates it. It is not the cost of matching alone.
- **Book op** is `submit()` or `cancel()` timed by itself on the consumer thread. That is the matching cost.
- The timer ticks at 24 MHz (41.67 ns per tick), so every number is quantized to that. 5.2 µs is exactly 125 ticks.
- P99 and max come from the macOS scheduler preempting a thread, not from the code. The first run after a build landed in the milliseconds the same way. macOS has no hard thread pinning (see Measurement notes).
- Every run processes the same deterministic order flow (seed 12345) and ends with the same book: 32,527 fills, 23,451 resting orders.

Setup for the table above: Apple M4 (4 performance + 6 efficiency cores), 16 GB, macOS 15.7.3, Homebrew Clang 22.1.0, CMake 4.2.3, Release build, commit `1c5ad00`, 5 consecutive runs.

### The id lookup fix

The first version mapped order ids to orders with `std::unordered_map`, and the P50 was about 2.4 ms: each insert allocated a hash node, and lookups walked cold buckets. Replacing it with a `std::vector<Order*>` indexed directly by id (one allocation at startup, one load per lookup) brought the P50 down to 5.2 µs. That matches the ring alone with no book behind it, so the book adds no visible latency at this timer's resolution.

## What's in the repo

```
include/llb/
  spsc_buffer.hpp       SPSC ring (header-only template)
  slab_allocator.hpp    fixed-size Order pool with an intrusive free list
  order.hpp             Order: 128-byte aligned, carries its own list links
  order_book.hpp        price-time priority order book
  ingress_message.hpp   24-byte NEW_ORDER / CANCEL message for the work ring
src/
  slab_allocator.cpp
  order_book.cpp
main.cpp                producer (order flow generator) / consumer (matching engine) benchmark
CMakeLists.txt
```

## Architecture

```
 producer thread                                   consumer thread
 ───────────────                                   ───────────────
 slab.allocate() ──> IngressMessage ── work_ring ──> OrderBook.submit() / cancel()
       ^             (NEW_ORDER | CANCEL)                  │
       │                                                   │ filled / cancelled / rejected
 slab.deallocate() <──────────── recycle_ring <────────────┘ (batched)
```

- **One work ring for both message types.** A cancel must be processed after the submit it targets. Separate rings per type would let a cancel overtake its order.
- **The slab is touched only by the producer.** The book never frees memory. Orders it is done with go back through the recycle ring and the producer deallocates them, so the allocator needs no locks.

### SPSC ring

- **Power-of-two capacity**: the index wraps with `& mask` instead of `%`.
- **Cache-line isolation**: `write_idx` and `read_idx` are `alignas(128)`, so producer and consumer never write the same coherency line.
- **Shadow indices**: each side keeps a plain local copy of the other side's index and only does an `acquire` load when that copy says the ring looks full or empty.
- **One slot reserved** to tell full from empty, so usable capacity is `capacity - 1`.

### Order book

- **Flat arrays per side.** `bids[]` and `asks[]` each hold `2 * ticks_each_side + 1` levels (1,001 in the benchmark: plus or minus $5 at a $0.01 tick around $100). A price maps to an index by its tick distance from the reference price, so finding a level is one array load. Prices out of range are rejected.
- **Price-time priority.** Each level is an intrusive doubly-linked FIFO, built from the `next`/`prev` links inside `Order`. Matching consumes from the head (oldest first) and new orders append at the tail. Cancels unlink in O(1).
- **Top of book** is tracked as `best_bid_idx` / `best_ask_idx` and walked past empty levels after a level drains.
- **O(1) cancel by id** through the direct-indexed `id_map` described above. Ids must be below `max_order_id`.
- **Batched recycles.** Freed orders collect in a local 128-entry buffer and are pushed to the recycle ring once at the end of each operation, not one atomic push per order.
- Each level is 24 bytes (head, tail, order count, total quantity); both sides together are about 48 KB.

### Slab allocator

- One contiguous arena, allocated once with `posix_memalign(128, ...)`.
- Free orders are threaded into a singly-linked list through `Order::next`, so allocating and freeing are each a pointer swap with no extra storage.
- Sized at 131,072 orders in the benchmark so the book can grow before the producer has to wait on recycles.

### Producer back-pressure

When the work ring is full, the producer drains the recycle ring while it waits. Otherwise both rings could fill and neither thread could make progress.

## Build and run

Requires a C++26-capable compiler (recent Clang or GCC) and CMake 3.25 or newer. The benchmark must run on Apple Silicon (it reads `cntvct_el0` and uses Mach thread APIs).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/LowLatencyBuffer
```

Release builds use `-O3 -mcpu=apple-m4 -ffast-math -flto` on Apple ARM, `-march=native` elsewhere.

The benchmark runs 10,000 warm-up messages, then measures 100,000. The order flow is 60% passive new orders (1 to 15 cents from mid), 30% cancels of a random earlier id, and 10% aggressive orders that cross the spread with larger size. It prints:

```
--- ORDER BOOK BENCHMARK (NS) ---
Samples, Mean, P50, P99, Max      submit-to-book latency
--- BOOK STATS ---
Fills, Resting orders             final book state (same every run)
Book op mean, Book op max         time inside submit()/cancel()
Best bid, Best ask
```

### Measurement notes

- Threads get `QOS_CLASS_USER_INTERACTIVE` and `THREAD_AFFINITY_POLICY` tags. On macOS these are hints that steer toward performance cores. They do not pin a thread to a core the way Linux does.
- The consumer hot-spins when the ring is empty. Yielding on idle made macOS deschedule it between bursts.
- `thread_time_constraint_policy` (macOS real-time scheduling) is requested but currently fails with `KERN_INVALID_ARGUMENT` and prints a warning. It does not affect results.

## Limitations

- The benchmark is macOS on Apple Silicon only.
- No unit tests in this repo. The order book is also used, with tests, in [Clearline](https://github.com/harshithptl/clearline), which adds per-fill reporting and serves it over gRPC.
- `Order::timestamp_ns` is declared but unused.
- Prices are `double` and mapped to a level with `std::round`, so a price between two ticks is silently rounded to the nearest tick instead of rejected.

## Next

- Linux `PREEMPT_RT` port with real core pinning, to measure the ring without macOS scheduler noise.
- Move the benchmark out of `main.cpp` into separate scenarios (ring alone, varying ring sizes, slab pressure).
