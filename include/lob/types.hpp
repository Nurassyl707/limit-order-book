#pragma once
#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;  // 0 is reserved as "invalid"
using Price   = std::int64_t;   // integer ticks (never floating point)
using Qty     = std::uint32_t;
using SeqNo   = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class TimeInForce : std::uint8_t {
    GTC,  // good-till-cancel: unfilled remainder rests on the book
    IOC   // immediate-or-cancel: unfilled remainder is discarded
};

enum class RejectReason : std::uint8_t {
    None,
    DuplicateId,
    InvalidId,
    InvalidQty,
    InvalidPrice,  // outside the configured tick range
    UnknownOrder
};

struct Trade {
    SeqNo   seq;       // monotonically increasing trade number
    OrderId taker_id;  // aggressing (incoming) order
    OrderId maker_id;  // resting order
    Price   price;     // always the resting order's price
    Qty     qty;
    Side    taker_side;
};

struct AddResult {
    bool         accepted = false;
    RejectReason reason   = RejectReason::None;
    Qty          filled   = 0;  // executed immediately on arrival
    Qty          resting  = 0;  // left on the book (always 0 for IOC)
};

}  // namespace lob
