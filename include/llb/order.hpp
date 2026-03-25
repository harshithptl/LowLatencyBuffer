#pragma once
#include <cstdint>

namespace llb {
    
struct alignas(128) Order {
    uint64_t id;
    double price;
    uint32_t quantity;
    char side;
    uint64_t timestamp_ns;

    Order(uint64_t id_, double price_, uint32_t qty_, char side_, uint64_t ts_ = 0)
        : id(id_), price(price_), quantity(qty_), side(side_), timestamp_ns(ts_) {}

    Order() = default;
};

}
