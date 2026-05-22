# LowLatencyBuffer

A lock-free Single-Producer Single-Consumer (SPSC) ring buffer for inter-core message passing on Apple Silicon (M-series). Paired with a slab allocator and a pointer-recycle ring to keep the hot path free of `malloc` / `new`.

Designed and benchmarked on an Apple M4 under macOS. The architecture is portable, but the thread-pinning code and CPU-timer code (`cntvct_el0`) are ARM/macOS-specific.

## What's in the repo

```
include/llb/
  spsc_buffer.hpp     Templated SPSC ring (header-only)
  slab_allocator.hpp  Slab interface
  order.hpp           Example payload type
src/
  slab_allocator.cpp  Slab implementation
main.cpp              Producer / consumer benchmark driver
CMakeLists.txt
```

`apps/`, `benchmarks/`, `tests/`, `cmake/`, `extern/` are placeholders for planned work — currently empty.

## Architecture

Two rings between the producer and consumer:

- **`work_ring`** — producer → consumer. Carries pointers to `Order` objects allocated from the slab.
- **`recycle_ring`** — consumer → producer. Returns used pointers so the producer can `deallocate` and reuse them.

The slab is touched only by the producer thread (alloc + dealloc); the consumer never calls into it.

### SPSC ring design

- **Power-of-two capacity** — index wrap is `(i + 1) & mask` instead of `% capacity`.
- **Cache-line isolation** — `write_idx` and `read_idx` are `alignas(128)` so the producer and consumer never share a coherency line.
- **Shadow indices** — each thread keeps a non-atomic local copy of the other side's index and only does an `acquire` load when the shadow says the ring looks full/empty.
- **Full-detection by reserving one slot** — usable capacity is `capacity - 1`.

### Active-Drain producer

When the `work_ring` is full, the producer doesn't just spin — it drains the `recycle_ring` inside the wait loop ([main.cpp:113](main.cpp#L113)). This avoids the deadlock where both rings are full and neither thread can make progress.

### Slab allocator

- Single contiguous arena, allocated once at startup via `posix_memalign(128, ...)`.
- Free list is a pre-`reserve()`d `std::vector<Order*>`. Because dealloc count never exceeds the initial reserve, the hot path stays malloc-free in practice.
- Slab capacity is sized **32× the ring** (32,768 vs. 1024) to give the producer burst headroom when the consumer falls behind.

## Build

Requires a C++26-capable compiler (recent Clang or GCC) and CMake ≥ 3.25.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/LowLatencyBuffer
```

Release builds apply `-O3 -mcpu=apple-m4 -ffast-math -flto` on Apple ARM, or `-march=native` elsewhere.

## Running the benchmark

`main.cpp` runs 100k warm-up iterations followed by 1M measured iterations. It prints mean, P50, P99, and max in nanoseconds.

```
--- HARDWARE-CORRECTED SLAB REPORT (NS) ---
Mean : ... ns
P50  : ... ns
P99  : ... ns
Max  : ... ns
```

### Measurement notes

- Timestamps come from the ARM system counter `cntvct_el0`, which on Apple Silicon ticks at **24 MHz** → 41.67 ns per tick. Any reported latency below ~42 ns is below the timer's resolution.
- `start_tsc` is stamped on the producer **before** `try_push` (which may spin under back-pressure). The reported latency is therefore producer-stamp → consumer-pop, not pure inter-core transit. Tail latency reflects both OS scheduling jitter and producer-side stalls.
- Threads are tagged with `THREAD_AFFINITY_POLICY` (tags 1 and 2) and `QOS_CLASS_USER_INTERACTIVE`. On macOS this is **advisory** — it nudges the scheduler toward P-cores but does not pin in the Linux sense. Expect periodic context-switch spikes.

### Observed behavior (M4, macOS)

- **Hardware floor**: a cache-line handoff between two P-cores via the L3 fabric is ~800–1200 ns.
- **P50** sits in the low tens of microseconds and is dominated by macOS scheduler jitter, not the ring code.
- **P99** ranges from sub-millisecond into the low milliseconds when the kernel preempts a thread.

For tighter tails you'd need a real-time kernel (e.g. Linux `PREEMPT_RT`) or kernel bypass — neither is in scope here.

## Limitations / current state

- macOS / Apple ARM only for the timing and pinning paths.
- Slab is single-threaded by construction (producer-side only).
- `Order::timestamp_ns` is declared but unused.
- No unit tests yet; `tests/` is a placeholder.

## Roadmap

- Order book integration (price-time priority matching engine) on top of the buffer.
- Linux `PREEMPT_RT` port to characterize the buffer without macOS scheduler noise.
- Move the inline benchmark in `main.cpp` into `benchmarks/` with multiple scenarios (pre-allocated array baseline, varying ring sizes, varying slab pressure).
