#pragma once
#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>
#include "order.hpp"

namespace llb {

class SlabAllocator {
public:
    explicit SlabAllocator(size_t num_orders);
    ~SlabAllocator();

    Order* allocate();
    void deallocate(Order* ptr);

    template<typename... Args>
    Order* create_order(Args&&... args) {
        Order* ptr = allocate();
        if (!ptr) return nullptr;
        return new (ptr) Order(std::forward<Args>(args)...);
    }

private:
    Order* memory_block = nullptr;
    Order* free_head = nullptr;
};

}
