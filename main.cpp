#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>
#include <thread>
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <mach/mach_time.h>
#include <mach/mach.h>

#include "llb/spsc_buffer.hpp"
#include "llb/slab_allocator.hpp"
#include "llb/order.hpp"
#include "llb/order_book.hpp"
#include "llb/ingress_message.hpp"

const double NS_PER_TICK = 41.6666666667;

inline uint64_t get_tsc() {
    uint64_t t;
    asm volatile("mrs %0, cntvct_el0" : "=r" (t));
    return t;
}

inline void cpu_relax() {
    asm volatile("yield" ::: "memory");
}

void pin_thread_to_core(int tag) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    thread_affinity_policy_data_t policy = { tag };
    thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
    thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY,
                      (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT);
}

// Ask macOS for a real-time scheduling guarantee: at least `computation_ns`
// of CPU within every `constraint_ns` window, with a nominal period of
// `period_ns`. This is the closest thing macOS offers to "keep this thread
// running, don't preempt it aggressively." preemptible=0 means the thread
// runs to completion within its budget before other RT work can steal it.
//
// Values passed are in nanoseconds; converted to mach absolute time units
// via mach_timebase_info (1 tick ≈ 41.67 ns on Apple Silicon, but we let
// the API do the math so the code is portable).
void set_realtime_thread(uint32_t period_ns,
                         uint32_t computation_ns,
                         uint32_t constraint_ns) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    auto ns_to_abs = [&](uint64_t ns) -> uint32_t {
        return (uint32_t)(ns * tb.denom / tb.numer);
    };

    thread_time_constraint_policy_data_t policy;
    policy.period      = ns_to_abs(period_ns);
    policy.computation = ns_to_abs(computation_ns);
    policy.constraint  = ns_to_abs(constraint_ns);
    policy.preemptible = 0;

    kern_return_t r = thread_policy_set(
        pthread_mach_thread_np(pthread_self()),
        THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&policy,
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    if (r != KERN_SUCCESS) {
        std::cerr << "warning: thread_time_constraint_policy failed: " << r << std::endl;
    }
}

int main() {
    // ─── Configuration ────────────────────────────────────────────────
    const size_t ring_capacity   = 1024;
    const size_t recycle_capacity= 8192;      // wider than work_ring so aggressive sweeps don't stall the consumer
    const size_t warm_up         = 10'000;
    const size_t iters           = 100'000;
    const size_t total_msgs      = warm_up + iters;
    const size_t slab_size       = 131'072;   // 128k — book can grow before hitting slab pressure

    const double ref_price       = 100.00;
    const double tick_size       = 0.01;
    const int    ticks_each_side = 500;   // ±$5 range → 1001 levels per side

    llb::SlabAllocator                    allocator(slab_size);
    llb::SPSCBuffer<llb::IngressMessage>  work_ring(ring_capacity);
    llb::SPSCBuffer<llb::Order*>          recycle_ring(recycle_capacity);
    llb::OrderBook                        book(ref_price, tick_size, ticks_each_side,
                                                total_msgs, recycle_ring);

    std::vector<double> latencies;
    latencies.reserve(iters);

    std::atomic<bool> producer_done{false};
    size_t   dropped_samples = 0;
    uint64_t book_op_tick_sum = 0;
    uint64_t book_op_tick_max = 0;

    // ─── CONSUMER (matching engine) ──────────────────────────────────
    std::thread consumer([&]() {
        pin_thread_to_core(1);
        // Ask for ~1 ms of CPU every 2 ms — coarse but macOS accepts
        // values in this range. Smaller values (5 µs / 2.5 µs) fail
        // KERN_INVALID_ARGUMENT on M4.
        set_realtime_thread(/*period*/ 2'000'000, /*computation*/ 1'000'000, /*constraint*/ 2'000'000);
        llb::IngressMessage msg;
        uint64_t msg_count = 0;

        while (true) {
            if (work_ring.try_pop(msg)) {
                ++msg_count;

                // Time the book op in isolation.
                const uint64_t before_book = get_tsc();
                if (msg.type == llb::IngressMessage::Type::NEW_ORDER) {
                    book.submit(msg.new_order);
                } else {
                    book.cancel(msg.cancel_id);
                }
                const uint64_t after_book = get_tsc();
                const uint64_t book_ticks = after_book - before_book;

                if (msg_count > warm_up) {
                    book_op_tick_sum += book_ticks;
                    if (book_ticks > book_op_tick_max) book_op_tick_max = book_ticks;

                    if (after_book >= msg.start_tsc) {
                        const uint64_t delta_ticks = after_book - msg.start_tsc;
                        latencies.push_back((double)delta_ticks * NS_PER_TICK);
                    } else {
                        ++dropped_samples;
                    }
                }

                if (msg_count == warm_up) {
                    std::cout << "Warm-up complete. Measuring..." << std::endl;
                }
            } else {
                if (producer_done.load(std::memory_order_acquire) && work_ring.empty()) break;
                // Hot-spin (no yield hint) to stay resident on the pinned core.
                // Hypothesis: cpu_relax() cues macOS to deschedule the thread.
            }
        }
    });

    // ─── PRODUCER (order-flow generator) ─────────────────────────────
    std::thread producer([&]() {
        pin_thread_to_core(2);
        set_realtime_thread(/*period*/ 2'000'000, /*computation*/ 1'000'000, /*constraint*/ 2'000'000);
        std::mt19937_64 rng(12345);
        uint64_t next_id = 0;

        // Drain any orders the book has recycled. Called opportunistically
        // and inside push-wait loops (Active Drain).
        auto drain_recycle = [&]() {
            llb::Order* rec = nullptr;
            while (recycle_ring.try_pop(rec)) {
                allocator.deallocate(rec);
            }
        };

        // Allocate + placement-construct an Order. If the slab is empty,
        // drain recycled orders and retry.
        auto make_order = [&](double price, uint32_t qty, char side) -> llb::Order* {
            llb::Order* ord = allocator.create_order(next_id, price, qty, side);
            while (!ord) {
                drain_recycle();
                cpu_relax();
                ord = allocator.create_order(next_id, price, qty, side);
            }
            ++next_id;
            return ord;
        };

        for (uint64_t i = 0; i < total_msgs; ++i) {
            const uint32_t roll = rng() % 100;
            const bool do_cancel     = (roll >= 60 && roll < 90 && next_id > 0);
            const bool do_aggressive = (roll >= 90);
            // Otherwise: passive NEW_ORDER.

            llb::IngressMessage msg;

            if (do_cancel) {
                msg.type      = llb::IngressMessage::Type::CANCEL;
                msg.cancel_id = rng() % next_id;
            } else {
                const bool buy = (rng() & 1u);
                double   price;
                uint32_t qty;

                if (do_aggressive) {
                    // Cross the spread; larger qty sweeps more depth.
                    price = buy ? 100.10 : 99.90;
                    qty   = 30;
                } else {
                    // Passive: bids below mid, asks above mid; 1-15 cent offset.
                    const double offset = 0.01 * ((rng() % 15) + 1);
                    price = buy ? (100.00 - offset) : (100.00 + offset);
                    qty   = 10;
                }

                llb::Order* ord = make_order(price, qty, buy ? 'B' : 'S');
                msg.type      = llb::IngressMessage::Type::NEW_ORDER;
                msg.new_order = ord;
            }

            // Stamp as late as possible so latency measures ring transit
            // + book work, not message construction.
            msg.start_tsc = get_tsc();
            while (!work_ring.try_push(msg)) {
                drain_recycle();          // Active Drain — breaks Double-Full
                cpu_relax();
            }

            // Drain every iteration. When the ring is empty this is a
            // single relaxed atomic load (shadow-index fast path); cheap
            // enough to always do. When the consumer is bursting recycles
            // (aggressive sweeps), this keeps the ring from filling.
            drain_recycle();
        }

        // Final drain before exit — the consumer might still be processing
        // and generating recycles.
        while (!work_ring.empty() ||
               !recycle_ring.empty()) {
            drain_recycle();
            cpu_relax();
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    // Drain any tail recycles the producer missed (best-effort cleanup).
    llb::Order* rec = nullptr;
    while (recycle_ring.try_pop(rec)) {
        allocator.deallocate(rec);
    }

    // ─── Report ──────────────────────────────────────────────────────
    std::sort(latencies.begin(), latencies.end());
    const double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);
    const size_t n = latencies.size();

    std::cout << "\n--- ORDER BOOK BENCHMARK (NS) ---" << std::endl;
    std::cout << "Samples : " << n << " (dropped " << dropped_samples << " on TSC skew)" << std::endl;
    if (n > 0) {
        std::cout << "Mean : " << sum / n << " ns" << std::endl;
        std::cout << "P50  : " << latencies[n * 0.5] << " ns" << std::endl;
        std::cout << "P99  : " << latencies[n * 0.99] << " ns" << std::endl;
        std::cout << "Max  : " << latencies.back() << " ns" << std::endl;
    }
    std::cout << "\n--- BOOK STATS ---" << std::endl;
    std::cout << "Fills          : " << book.total_fills() << std::endl;
    std::cout << "Resting orders : " << book.resting_count() << std::endl;
    if (n > 0) {
        std::cout << "Book op mean   : " << (double)book_op_tick_sum / n * NS_PER_TICK << " ns" << std::endl;
        std::cout << "Book op max    : " << (double)book_op_tick_max * NS_PER_TICK << " ns" << std::endl;
    }
    if (book.has_bids()) {
        std::cout << "Best bid       : " << book.best_bid_price() << std::endl;
    }
    if (book.has_asks()) {
        std::cout << "Best ask       : " << book.best_ask_price() << std::endl;
    }

    return 0;
}
