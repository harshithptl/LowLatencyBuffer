#pragma once
#include <cstdint>
#include <type_traits>
#include "order.hpp"

namespace llb {

// Message flowing producer → matching engine through the work ring.
//
// Single tagged variant (not one ring per message type) because event
// ordering must be preserved: a cancel arriving after a submit MUST be
// processed after that submit. Splitting into per-type rings breaks
// that invariant.
//
// Layout (24 bytes):
//   [0]     type discriminator (1 byte + 7 bytes pad → 8 aligned)
//   [8]     start_tsc — producer-side ARM system-counter reading,
//           stamped just before try_push. Used by the consumer to
//           compute submit-to-book latency for both message types.
//   [16]    payload union — 8 bytes, either an Order* (new order) or
//           a uint64_t order id (cancel target).
struct IngressMessage {
    enum class Type : uint8_t {
        NEW_ORDER = 0,   // payload: new_order
        CANCEL    = 1,   // payload: cancel_id
    };

    Type     type;
    uint8_t  _pad[7];
    uint64_t start_tsc;
    union {
        Order*   new_order;
        uint64_t cancel_id;
    };
};

static_assert(sizeof(IngressMessage) == 24,
              "IngressMessage layout drift — check padding.");
static_assert(std::is_trivially_copyable_v<IngressMessage>,
              "IngressMessage must be trivially copyable to ride in SPSCBuffer.");

} // namespace llb
