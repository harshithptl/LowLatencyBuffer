#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include "order.hpp"
#include "spsc_buffer.hpp"

namespace llb {

// A single price level in the book: a doubly-linked intrusive FIFO of
// resting Orders at one price, plus aggregate counters for O(1) depth
// queries. Head is oldest (time priority — matches consume from here);
// tail is newest (inserts append here).
struct PriceLevel {
    Order*   head;
    Order*   tail;
    uint32_t order_count;   // resting orders at this price
    uint32_t total_qty;     // aggregate resting quantity at this price
};

static_assert(sizeof(PriceLevel) == 24, "PriceLevel layout drift.");
static_assert(std::is_trivially_copyable_v<PriceLevel>);


// Central-limit order book with price-time priority.
//
// Storage: two flat arrays (bids and asks), each of size
// (2 * ticks_each_side + 1). Price is mapped to an integer index by
// ticks from a fixed reference price. Level lookup is a single load;
// out-of-range prices are rejected.
//
// Ownership: OrderBook is single-threaded — the consumer thread owns
// the book. It also owns the slab (via reference), so orders that get
// fully filled or cancelled are deallocated locally with no recycle
// ring. Producers push into a work ring; the consumer pops and calls
// submit()/cancel() on this class.
class OrderBook {
public:
    // reference_price: the array's midpoint, at index ticks_each_side.
    // tick_size:       gap between adjacent price levels.
    // ticks_each_side: how many ticks the book extends on either side.
    //                  Total levels = 2 * ticks_each_side + 1.
    // max_order_id:    upper bound on order IDs that will ever be
    //                  submitted. Sizes the id → Order* lookup array.
    //                  Callers must ensure Order::id < max_order_id.
    // recycle_ring:    when an Order is fully filled, cancelled, or
    //                  rejected (out-of-range), it is pushed here.
    //                  The producer thread drains this ring and calls
    //                  allocator.deallocate() from its own thread —
    //                  the slab stays single-threaded.
    OrderBook(double  reference_price,
              double  tick_size,
              int     ticks_each_side,
              size_t  max_order_id,
              SPSCBuffer<Order*>& recycle_ring);

    // Submit a new order. Attempts to match against the opposite side
    // as an aggressive order; any unfilled remainder rests in the book
    // at ord->price. Returns the number of fills generated (0 or more).
    // Orders with a price outside the book's range are rejected and
    // returned to the slab.
    uint32_t submit(Order* ord);

    // Cancel a resting order by ID. Returns true on success, false if
    // the id is unknown (already filled, already cancelled, or never
    // submitted). O(1) via id_map + intrusive unlink.
    bool cancel(uint64_t order_id);

    // Stats & introspection (not on the hot path).
    uint64_t total_fills() const { return fills_count; }
    size_t   resting_count() const { return resting_count_; }
    bool     has_bids() const { return best_bid_idx >= 0; }
    bool     has_asks() const { return best_ask_idx < total_levels; }
    double   best_bid_price() const;
    double   best_ask_price() const;

private:
    const double reference_price;
    const double tick_size;
    const int    ticks_each_side;
    const int    total_levels;

    // bids[i] holds resting buy orders at idx_to_price(i).
    // asks[i] holds resting sell orders at idx_to_price(i).
    // At any single index, only one of bids[i] or asks[i] has resting
    // orders — a crossing incoming order would match immediately.
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;

    // Top-of-book indices. best_bid_idx is the LARGEST i with resting
    // bids (highest price a buyer will pay); best_ask_idx is the
    // SMALLEST i with resting asks (lowest price a seller will accept).
    // Sentinel values represent an empty side.
    int best_bid_idx = -1;             // no bids
    int best_ask_idx;                  // = total_levels (no asks); set in ctor

    // O(1) cancel-by-id lookup. Direct-indexed vector: id_map[id] holds
    // either the live Order* or nullptr. IDs must be < max_order_id;
    // sized once at construction. Cache-friendlier than unordered_map:
    // one load per lookup with no hash calc, no bucket walk, no
    // per-node heap allocation.
    std::vector<Order*> id_map;

    // Tracked separately since id_map has a fixed size (unlike the
    // unordered_map's size() which reflected live count).
    size_t resting_count_ = 0;

    SPSCBuffer<Order*>& recycle_ring;
    uint64_t fills_count = 0;

    // Batched recycles: instead of pushing each freed Order to the
    // recycle_ring one at a time (each push a separate atomic), we
    // accumulate pointers in a local buffer and flush at the end of
    // each submit()/cancel() call. This turns N atomic writes into 1
    // amortized-N-slots write path and lets the producer's drain see
    // a burst arrive rather than a trickle.
    static constexpr size_t PENDING_MAX = 128;
    Order* pending_recycles[PENDING_MAX];
    size_t pending_count = 0;

    // Helpers.
    int    price_to_idx(double price) const;
    double idx_to_price(int idx) const;

    void     insert_into_level(PriceLevel& level, Order* ord);
    void     unlink_from_level(PriceLevel& level, Order* ord);
    void     recycle_order(Order* ord);      // append to pending buffer
    void     flush_recycles();               // push all pending to recycle_ring
    uint32_t match_bid(Order* aggressive);   // aggressive buy consumes asks
    uint32_t match_ask(Order* aggressive);   // aggressive sell consumes bids
    void     advance_best_ask();             // walk best_ask_idx forward past empty levels
    void     retreat_best_bid();             // walk best_bid_idx backward past empty levels
};

} // namespace llb
