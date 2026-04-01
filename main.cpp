#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <thread>
#include <atomic>
#include <pthread.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>

#include "llb/spsc_buffer.hpp"
#include "llb/slab_allocator.hpp"
#include "llb/order.hpp"

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

int main() {
    const size_t ring_capacity = 1024; 
    const size_t warm_up = 100'000;
    const size_t iters = 1'000'000;
    
    // Increased to 32k to ensure we never run out of pointers during a burst
    llb::SlabAllocator allocator(32768);
    llb::SPSCBuffer<llb::Order*> work_ring(ring_capacity);
    llb::SPSCBuffer<llb::Order*> recycle_ring(ring_capacity);

    std::vector<double> latencies;
    latencies.reserve(iters);
    
    std::atomic<bool> start_measuring{false};
    std::atomic<bool> producer_done{false};

    // --- CONSUMER ---
    std::thread consumer([&]() {
        pin_thread_to_core(1);
        llb::Order* ord = nullptr;
        
        while (true) {
            if (work_ring.try_pop(ord)) {
                if (start_measuring.load(std::memory_order_relaxed)) {
                    latencies.push_back((double)(get_tsc() - ord->start_tsc) * NS_PER_TICK);
                }
                
                // NON-BLOCKING RECYCLE: If recycle_ring is full, 
                // we spin a little but don't hang the whole system.
                size_t retry = 0;
                while (!recycle_ring.try_push(ord)) {
                    cpu_relax();
                    if (++retry > 1000) {
                        // High-pressure safety: if we can't recycle, 
                        // something is wrong with the producer's drain.
                        std::this_thread::yield(); 
                        retry = 0;
                    }
                }
            } else {
                if (producer_done.load(std::memory_order_acquire) && work_ring.empty()) break;
                cpu_relax();
            }
        }
    });

    // --- PRODUCER ---
    std::thread producer([&]() {
        pin_thread_to_core(2);
        
        auto drain_recycle = [&]() {
            llb::Order* rec = nullptr;
            // Drain the entire ring in one go
            while (recycle_ring.try_pop(rec)) {
                allocator.deallocate(rec);
            }
        };

        for (size_t i = 0; i < (warm_up + iters); ++i) {
            if (i == warm_up) {
                std::cout << "Warm-up complete. Measuring..." << std::endl;
                start_measuring.store(true, std::memory_order_release);
            }

            // 1. Allocate with absolute drain
            llb::Order* ord = allocator.create_order(i, 100.0, 10, 'B');
            while (!ord) {
                drain_recycle(); // Must get pointers back to continue
                cpu_relax();
                ord = allocator.create_order(i, 100.0, 10, 'B');
            }

            // 2. Push with "Active Draining"
            ord->start_tsc = get_tsc();
            while (!work_ring.try_push(ord)) {
                // While waiting for the work_ring to have space, 
                // the producer MUST keep draining the recycle_ring.
                // This prevents the "Double-Full" Deadlock.
                drain_recycle(); 
                cpu_relax();
            }

            // 3. Keep the pipeline moving
            if (i % 8 == 0) drain_recycle();
        }
        producer_done.store(true, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    
    std::sort(latencies.begin(), latencies.end());
    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    std::cout << "\n--- HARDWARE-CORRECTED SLAB REPORT (NS) ---" << std::endl;
    std::cout << "Mean : " << sum / iters << " ns" << std::endl;
    std::cout << "P50  : " << latencies[iters * 0.5] << " ns" << std::endl;
    std::cout << "P99  : " << latencies[iters * 0.99] << " ns" << std::endl;
    std::cout << "Max  : " << latencies.back() << " ns" << std::endl;

    return 0;
}