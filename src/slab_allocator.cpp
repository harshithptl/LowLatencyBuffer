#include "../include/llb/slab_allocator.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace llb {

SlabAllocator::SlabAllocator(size_t num_orders) : total_orders(num_orders) {
    free_list.reserve(num_orders);

    size_t total_size = num_orders * sizeof(Order);
    int result = posix_memalign((void**)&memory_block, 128, total_size);

    if (result != 0) {
        throw std::runtime_error("Failed to allocate aligned memory");
    }
    
    for (size_t i = 0; i < num_orders; i++) {
        free_list.push_back(memory_block + i);
    }
}

SlabAllocator::~SlabAllocator() {
    if (memory_block == nullptr) {
        return;
    }
    free(memory_block);
}

Order* SlabAllocator::allocate() {
    if (free_list.empty()) {
        return nullptr;
    }
    Order* free_spot = free_list.back();
    free_list.pop_back();
    return free_spot;
}

void SlabAllocator::deallocate(Order* ptr) {
    if (!ptr) {
        return;
    }
    ptr->~Order();
    free_list.push_back(ptr);
}

}