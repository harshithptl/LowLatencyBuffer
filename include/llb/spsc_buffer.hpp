#pragma once
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>

namespace llb {

// Lock-free single-producer, single-consumer ring buffer.
//
// Contract:
//   - Exactly ONE thread may call try_push (the "producer").
//   - Exactly ONE thread may call try_pop  (the "consumer").
//   - Behavior is undefined if either operation is called from
//     multiple threads concurrently.
//
// Type requirements on T:
//   - Trivially copyable. Slots are raw memory from aligned_alloc; they
//     are never default-constructed, so assignment into a slot must not
//     rely on prior construction of the destination. Enforced by
//     static_assert below. (Note: trivially copyable also implies
//     trivially destructible per the standard, so the buffer destructor
//     can std::free without running per-element destructors.)
//
// Capacity requirements:
//   - Must be a power of two (checked at construction).
//   - Usable capacity is capacity - 1. One slot is always reserved to
//     distinguish "full" (next_write == read) from "empty"
//     (write == read).
//
// Memory ordering:
//   - try_push writes the slot, then RELEASE-stores write_idx.
//   - try_pop  ACQUIRE-loads write_idx, then reads the slot.
//   - This release/acquire pair establishes happens-before: every write
//     the producer made before its release (including writes reachable
//     through pointers stored in the slot) is visible to the consumer
//     after a successful try_pop.
//
// Layout:
//   - write_idx and read_idx are alignas(128) so the producer's and
//     consumer's atomic indices sit on distinct cache lines. Prevents
//     ping-pong on the index atomics themselves.
//   - Each thread keeps a non-atomic "shadow" of the other side's index
//     in its own cache line. try_push/try_pop only fall back to an
//     ACQUIRE load of the real atomic when the shadow says the ring
//     looks full/empty. Under steady state this reduces cross-core
//     atomic loads to near-zero.
template<typename T>
class SPSCBuffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SPSCBuffer<T>: T must be trivially copyable; slots are "
                  "raw memory and are assigned into without prior construction.");
public:
    // Construct a ring of the given capacity. Capacity MUST be a power
    // of two; throws std::runtime_error otherwise. Usable capacity is
    // cap - 1 (one slot reserved for the full/empty distinction).
    //
    // The underlying storage is allocated with aligned_alloc(128, ...)
    // so the buffer starts on a cache-line boundary. Slots are NOT
    // constructed; correctness relies on T being trivially copyable.
    explicit SPSCBuffer(size_t cap)
        : capacity(cap),
          mask(cap - 1),
          buffer(static_cast<T*>(std::aligned_alloc(128, cap * sizeof(T)))) {

        if ((cap & (cap - 1)) != 0) {
            throw std::runtime_error("Capacity must be a power of 2");
        }

        write_idx.store(0, std::memory_order_relaxed);
        read_idx.store(0, std::memory_order_relaxed);
    }

    ~SPSCBuffer() {
        std::free(buffer);
    }

    // Attempt to push `item`. Returns true on success, false if the ring
    // is full.
    //
    // Fast path: read producer-local write_idx (relaxed), compute
    // next_write, compare against the shadow copy of read_idx. If the
    // shadow says there is space, write the slot and release-store
    // write_idx. No cross-core atomic operation on the fast path.
    //
    // Slow path (shadow says full): acquire-load the real read_idx to
    // refresh the shadow. This is the only place the producer pays a
    // cross-core load; it happens only when the ring is genuinely near
    // capacity.
    bool try_push(const T& item) {
        const size_t current_write = write_idx.load(std::memory_order_relaxed);
        const size_t next_write = (current_write + 1) & mask;

        if (next_write == read_idx_shadow) {
            read_idx_shadow = read_idx.load(std::memory_order_acquire);
            if (next_write == read_idx_shadow) return false;
        }

        buffer[current_write] = item;
        write_idx.store(next_write, std::memory_order_release);
        return true;
    }

    // Attempt to pop into `item`. Returns true on success, false if the
    // ring is empty.
    //
    // Symmetric to try_push: fast path reads consumer-local read_idx
    // and shadow copy of write_idx; falls back to an acquire-load of
    // the real write_idx only when the shadow suggests empty. The
    // acquire-load synchronizes with the producer's release-store,
    // making all producer writes prior to the release visible here.
    bool try_pop(T& item) {
        const size_t current_read = read_idx.load(std::memory_order_relaxed);

        if (current_read == write_idx_shadow) {
            write_idx_shadow = write_idx.load(std::memory_order_acquire);
            if (current_read == write_idx_shadow) return false;
        }

        item = buffer[current_read];
        read_idx.store((current_read + 1) & mask, std::memory_order_release);
        return true;
    }

    // Snapshot check. Returns true if write_idx == read_idx at the
    // moment of the call. Both loads are relaxed; the result may be
    // stale before the caller acts on it. Intended for shutdown /
    // drain-loop termination, not for coordinating pushes and pops.
    bool empty() const {
        return write_idx.load(std::memory_order_relaxed) == read_idx.load(std::memory_order_relaxed);
    }

    SPSCBuffer(const SPSCBuffer&) = delete;
    SPSCBuffer& operator=(const SPSCBuffer&) = delete;

private:
    const size_t capacity;
    const size_t mask;    // capacity - 1; used for (i + 1) & mask
    T* const buffer;      // aligned_alloc'd, not per-slot constructed

    // Producer's cache line: producer-owned atomic + producer-local
    // shadow of the consumer's index. Producer writes write_idx on
    // every successful push and reads/updates read_idx_shadow only on
    // the slow path. Consumer never touches this line during steady
    // state.
    alignas(128) std::atomic<size_t> write_idx;
    size_t read_idx_shadow = 0;

    // Consumer's cache line: mirror of the above. Consumer writes
    // read_idx, reads/updates write_idx_shadow. Producer never touches
    // this line during steady state.
    alignas(128) std::atomic<size_t> read_idx;
    size_t write_idx_shadow = 0;
};

} // namespace llb