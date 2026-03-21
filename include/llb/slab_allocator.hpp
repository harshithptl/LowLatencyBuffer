#pragma once
#include <vector>
#include <cstdlib>
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
        return new (ptr) Order{std::forward<Args>(args)...};
    }

private:
    size_t total_orders;
    Order* memory_block = nullptr;
    std::vector<Order*> free_list;

};

} 