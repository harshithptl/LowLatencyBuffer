#pragma once
#include <atomic>
#include <cstddef>
#include <cstdlib>  
#include <stdexcept> 

namespace llb {

template<typename T>
class SPSCBuffer {
public:
    explicit SPSCBuffer(size_t cap) 
        : capacity(cap), 
          mask(cap - 1), 
          buffer(static_cast<T*>(std::aligned_alloc(128, cap * sizeof(T)))) {
        
        // Safety check: Capacity must be power of 2 for bitwise math
        if ((cap & (cap - 1)) != 0) {
            throw std::runtime_error("Capacity must be a power of 2");
        }

        write_idx.store(0, std::memory_order_relaxed);
        read_idx.store(0, std::memory_order_relaxed);
    }

    ~SPSCBuffer() {
        std::free(buffer);
    }

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

    bool empty() const {
        return write_idx.load(std::memory_order_relaxed) == read_idx.load(std::memory_order_relaxed);
    }

    SPSCBuffer(const SPSCBuffer&) = delete;
    SPSCBuffer& operator=(const SPSCBuffer&) = delete;

private:
    const size_t capacity;
    const size_t mask;
    T* const buffer;

    // Producer zone
    alignas(128) std::atomic<size_t> write_idx;
    size_t read_idx_shadow = 0;

    // Consumer zone
    alignas(128) std::atomic<size_t> read_idx;
    size_t write_idx_shadow = 0;
};

} // namespace llb