#pragma once
#include <cstdint>

namespace llb {
    
struct alignas(128) Order {
    // Intrusive links: free-list pointer while in the slab pool,
    // FIFO pointers at a price level once the book is wired up.
    Order* next = nullptr;
    Order* prev = nullptr;

    uint64_t id;
    double price;
    uint32_t quantity;
    char side;
    uint64_t timestamp_ns;
    uint64_t start_tsc;

    Order(uint64_t id_, double price_, uint32_t qty_, char side_)
        : id(id_), price(price_), quantity(qty_), side(side_),
          timestamp_ns(0), start_tsc(0) {}

    Order() = default;
};

}
