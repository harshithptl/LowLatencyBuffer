#include "../include/llb/slab_allocator.hpp"
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace llb {

SlabAllocator::SlabAllocator(size_t num_orders) {
    size_t total_size = num_orders * sizeof(Order);
    if (posix_memalign(reinterpret_cast<void**>(&memory_block), 128, total_size) != 0) {
        throw std::runtime_error("Failed to allocate aligned memory");
    }

    // Begin Order lifetimes (trivial ctor, just zeros next/prev),
    // then thread them into a singly-linked free list via Order::next.
    for (size_t i = 0; i < num_orders; ++i) {
        new (&memory_block[i]) Order();
        memory_block[i].next = (i + 1 < num_orders) ? &memory_block[i + 1] : nullptr;
    }
    free_head = memory_block;
}

SlabAllocator::~SlabAllocator() {
    if (memory_block) std::free(memory_block);
}

Order* SlabAllocator::allocate() {
    Order* p = free_head;
    if (!p) return nullptr;
    free_head = p->next;
    return p;
}

void SlabAllocator::deallocate(Order* p) {
    if (!p) return;
    // Order is trivially destructible; no destructor call needed.
    p->next = free_head;
    free_head = p;
}

}
