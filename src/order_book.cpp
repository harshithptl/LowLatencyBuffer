#include "../include/llb/order_book.hpp"
#include <algorithm>
#include <cmath>

namespace llb {

OrderBook::OrderBook(double  reference_price_,
                     double  tick_size_,
                     int     ticks_each_side_,
                     size_t  max_order_id,
                     SPSCBuffer<Order*>& recycle_ring_)
  : reference_price(reference_price_),
    tick_size(tick_size_),
    ticks_each_side(ticks_each_side_),
    total_levels(2 * ticks_each_side_ + 1),
    bids(total_levels),                 // value-initialized → all fields zero
    asks(total_levels),
    best_ask_idx(total_levels),         // sentinel: no asks
    id_map(max_order_id, nullptr),      // direct-indexed, all slots null
    recycle_ring(recycle_ring_)
{
    // best_bid_idx defaults to -1 (see header NSDMI).
}

// ─── public API ──────────────────────────────────────────────────────────────

uint32_t OrderBook::submit(Order* ord) {
    const int idx = price_to_idx(ord->price);
    if (idx < 0) {
        // Price outside book's range — reject and reclaim the slot.
        recycle_order(ord);
        return 0;
    }

    uint32_t fills = 0;
    if (ord->side == 'B') {
        // Aggressive buy: consume asks up to and including ord->price.
        fills = match_bid(ord);
        if (ord->quantity > 0) {
            // Unfilled remainder rests at ord->price as a resting bid.
            insert_into_level(bids[idx], ord);
            id_map[ord->id] = ord;
            ++resting_count_;
            if (idx > best_bid_idx) best_bid_idx = idx;
        } else {
            recycle_order(ord);
        }
    } else {  // 'S'
        fills = match_ask(ord);
        if (ord->quantity > 0) {
            insert_into_level(asks[idx], ord);
            id_map[ord->id] = ord;
            ++resting_count_;
            if (idx < best_ask_idx) best_ask_idx = idx;
        } else {
            recycle_order(ord);
        }
    }

    fills_count += fills;
    flush_recycles();
    return fills;
}

bool OrderBook::cancel(uint64_t order_id) {
    if (order_id >= id_map.size()) return false;
    Order* ord = id_map[order_id];
    if (!ord) return false;   // already cancelled or matched, or never submitted

    const int idx = price_to_idx(ord->price);

    if (ord->side == 'B') {
        unlink_from_level(bids[idx], ord);
        if (idx == best_bid_idx && bids[idx].head == nullptr) {
            retreat_best_bid();
        }
    } else {
        unlink_from_level(asks[idx], ord);
        if (idx == best_ask_idx && asks[idx].head == nullptr) {
            advance_best_ask();
        }
    }

    id_map[order_id] = nullptr;
    --resting_count_;
    recycle_order(ord);
    flush_recycles();
    return true;
}

double OrderBook::best_bid_price() const {
    return has_bids() ? idx_to_price(best_bid_idx) : -1.0;
}

double OrderBook::best_ask_price() const {
    return has_asks() ? idx_to_price(best_ask_idx) : -1.0;
}

// ─── private helpers ─────────────────────────────────────────────────────────

int OrderBook::price_to_idx(double price) const {
    const double ticks_from_ref = (price - reference_price) / tick_size;
    const int idx = static_cast<int>(std::round(ticks_from_ref)) + ticks_each_side;
    if (idx < 0 || idx >= total_levels) return -1;
    return idx;
}

double OrderBook::idx_to_price(int idx) const {
    return reference_price + (idx - ticks_each_side) * tick_size;
}

// Append to FIFO tail. O(1). Uses the intrusive next/prev in Order.
void OrderBook::insert_into_level(PriceLevel& level, Order* ord) {
    ord->next = nullptr;
    ord->prev = level.tail;
    if (level.tail) {
        level.tail->next = ord;
    } else {
        level.head = ord;    // empty level → ord is head as well
    }
    level.tail = ord;
    level.order_count++;
    level.total_qty += ord->quantity;
}

// Push a done-with Order onto the recycle ring. The producer thread
// drains this ring and returns the Order to the slab. If the recycle
// ring is full, the producer's Active-Drain is expected to make room
// shortly — we spin with a yield hint.
// Append a done-with Order to the local pending buffer. Cheap — no
// atomics, just a bounded array append. flush_recycles() empties the
// buffer to the recycle_ring at the end of each book op (or mid-op if
// the buffer fills, though PENDING_MAX = 128 is well above realistic
// per-op recycles).
void OrderBook::recycle_order(Order* ord) {
    pending_recycles[pending_count++] = ord;
    if (pending_count == PENDING_MAX) {
        flush_recycles();
    }
}

// Drain the pending buffer into the recycle_ring in one burst. Each
// push is still one atomic operation (try_push is per-slot), but the
// producer sees the whole burst arrive together, which drains cleanly
// on the next iteration of its main loop.
void OrderBook::flush_recycles() {
    for (size_t i = 0; i < pending_count; ++i) {
        while (!recycle_ring.try_push(pending_recycles[i])) {
            asm volatile("yield" ::: "memory");
        }
    }
    pending_count = 0;
}

// Splice out of FIFO. O(1). Handles head, tail, and middle positions.
void OrderBook::unlink_from_level(PriceLevel& level, Order* ord) {
    if (ord->prev) {
        ord->prev->next = ord->next;
    } else {
        level.head = ord->next;   // ord was head
    }
    if (ord->next) {
        ord->next->prev = ord->prev;
    } else {
        level.tail = ord->prev;   // ord was tail
    }
    level.order_count--;
    level.total_qty -= ord->quantity;
}

// Aggressive buy: sweep asks from best_ask_idx (lowest ask) upward, consuming
// resting sells whose price is ≤ aggressive->price. Each match generates a
// fill; a fully-consumed resting order is unlinked and freed.
uint32_t OrderBook::match_bid(Order* aggressive) {
    uint32_t fills = 0;
    const int max_price_idx = price_to_idx(aggressive->price);
    if (max_price_idx < 0) return 0;

    while (best_ask_idx <= max_price_idx && aggressive->quantity > 0) {
        PriceLevel& level = asks[best_ask_idx];

        while (level.head && aggressive->quantity > 0) {
            Order* resting = level.head;
            const uint32_t match_qty = std::min(resting->quantity, aggressive->quantity);

            resting->quantity   -= match_qty;
            aggressive->quantity -= match_qty;
            level.total_qty     -= match_qty;
            fills++;

            if (resting->quantity == 0) {
                unlink_from_level(level, resting);   // subtracts 0 from total_qty
                id_map[resting->id] = nullptr;
                --resting_count_;
                recycle_order(resting);
            }
        }

        if (level.head == nullptr) advance_best_ask();
    }

    return fills;
}

// Aggressive sell: sweep bids from best_bid_idx (highest bid) downward.
uint32_t OrderBook::match_ask(Order* aggressive) {
    uint32_t fills = 0;
    const int min_price_idx = price_to_idx(aggressive->price);
    if (min_price_idx < 0) return 0;

    while (best_bid_idx >= min_price_idx && aggressive->quantity > 0) {
        PriceLevel& level = bids[best_bid_idx];

        while (level.head && aggressive->quantity > 0) {
            Order* resting = level.head;
            const uint32_t match_qty = std::min(resting->quantity, aggressive->quantity);

            resting->quantity   -= match_qty;
            aggressive->quantity -= match_qty;
            level.total_qty     -= match_qty;
            fills++;

            if (resting->quantity == 0) {
                unlink_from_level(level, resting);
                id_map[resting->id] = nullptr;
                --resting_count_;
                recycle_order(resting);
            }
        }

        if (level.head == nullptr) retreat_best_bid();
    }

    return fills;
}

// Walk best_ask_idx forward until we hit a non-empty level or the array end
// (sentinel = total_levels, meaning no asks).
void OrderBook::advance_best_ask() {
    while (best_ask_idx < total_levels && asks[best_ask_idx].head == nullptr) {
        best_ask_idx++;
    }
}

// Symmetric walk for bids. Sentinel = -1 (no bids).
void OrderBook::retreat_best_bid() {
    while (best_bid_idx >= 0 && bids[best_bid_idx].head == nullptr) {
        best_bid_idx--;
    }
}

} // namespace llb
