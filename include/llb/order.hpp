#include <cstdint>

namespace llb {
    
struct alignas(128) Order {
    uint64_t id;
    double price;
    uint32_t quantity;
    char side;
};

}
