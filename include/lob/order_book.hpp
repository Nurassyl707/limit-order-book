#pragma once
// Limit order book + matching engine with strict price-time priority.
//
//  * Orders at a better price always match first; within a price, the order
//    that arrived first matches first (FIFO).
//  * Trades execute at the RESTING order's price.
//  * Supported: GTC/IOC limit orders, market orders, cancel, modify.
//  * Single-threaded by design (typical for matching engines: one writer per
//    instrument avoids locking on the hot path).
//
// The book is a template over a "price level" policy (see price_levels.hpp) so
// the identical matching logic can run on std::map or a tick-indexed array.
#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

#include "lob/id_map.hpp"
#include "lob/price_levels.hpp"
#include "lob/types.hpp"

namespace lob {

template <class Policy>
class OrderBook {
public:
    using TradeHandler = std::function<void(const Trade&)>;

    // [min_price, max_price] is the accepted tick range (enforced by the array
    // policy; the map policy accepts any price but is given the same range so
    // both books behave identically in tests/benchmarks).
    OrderBook(Price min_price, Price max_price, std::size_t expected_orders = 0)
        : min_(min_price), max_(max_price),
          bids_(min_price, max_price), asks_(min_price, max_price),
          ids_(expected_orders ? expected_orders : 1024) {
        nodes_.reserve(expected_orders);
    }

    void set_trade_handler(TradeHandler h) { on_trade_ = std::move(h); }

    // ---- order entry -------------------------------------------------------

    AddResult add_limit(OrderId id, Side side, Price price, Qty qty,
                        TimeInForce tif = TimeInForce::GTC) {
        if (auto r = validate(id, qty); r != RejectReason::None) return reject(r);
        if (price < min_ || price > max_) return reject(RejectReason::InvalidPrice);
        return submit(id, side, price, qty, tif);
    }

    // Market order: sweeps the opposite side at any price; remainder discarded.
    AddResult add_market(OrderId id, Side side, Qty qty) {
        if (auto r = validate(id, qty); r != RejectReason::None) return reject(r);
        const Price limit = side == Side::Buy ? std::numeric_limits<Price>::max()
                                              : std::numeric_limits<Price>::min();
        return submit(id, side, limit, qty, TimeInForce::IOC);
    }

    // Returns false if the order is not resting on the book.
    bool cancel(OrderId id) {
        const std::uint32_t* p = ids_.find(id);
        if (!p) return false;
        const std::uint32_t idx = *p;
        const Node& n = nodes_[idx];
        if (n.side == Side::Buy) remove_resting(bids_, idx);
        else                     remove_resting(asks_, idx);
        ids_.erase(id);
        free_node(idx);
        return true;
    }

    // Replace price and/or quantity of a resting order.
    //  * same price, smaller qty  -> reduced in place, KEEPS time priority
    //  * anything else            -> cancel + re-enter (new time priority, and
    //                                may match immediately if it now crosses)
    AddResult modify(OrderId id, Price new_price, Qty new_qty) {
        const std::uint32_t* p = ids_.find(id);
        if (!p) return reject(RejectReason::UnknownOrder);
        if (new_qty == 0) return reject(RejectReason::InvalidQty);
        if (new_price < min_ || new_price > max_) return reject(RejectReason::InvalidPrice);

        const std::uint32_t idx = *p;
        Node& n = nodes_[idx];
        if (new_price == n.price && new_qty <= n.qty) {
            Level& lv = n.side == Side::Buy ? bids_.level(n.price) : asks_.level(n.price);
            lv.total -= (n.qty - new_qty);
            n.qty = new_qty;
            AddResult r;
            r.accepted = true;
            r.resting  = new_qty;
            return r;
        }
        const Side side = n.side;
        cancel(id);
        return submit(id, side, new_price, new_qty, TimeInForce::GTC);
    }

    // ---- queries -----------------------------------------------------------

    std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.best_price());
    }
    std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.best_price());
    }
    // Total resting quantity at a price on one side (0 if none).
    Qty volume_at(Side side, Price price) const {
        const Level* l = side == Side::Buy ? bids_.find(price) : asks_.find(price);
        return l ? l->total : 0;
    }
    std::optional<Qty> remaining_qty(OrderId id) const {
        const std::uint32_t* p = ids_.find(id);
        return p ? std::optional<Qty>(nodes_[*p].qty) : std::nullopt;
    }
    bool        contains(OrderId id) const { return ids_.find(id) != nullptr; }
    std::size_t order_count() const noexcept { return ids_.size(); }
    SeqNo       trade_count() const noexcept { return trade_seq_; }

private:
    struct Node {
        OrderId       id;
        Price         price;
        Qty           qty;
        Side          side;
        std::uint32_t prev;  // intrusive FIFO links; `next` doubles as the
        std::uint32_t next;  // free-list link when the node is unused
    };

    static AddResult reject(RejectReason r) {
        AddResult a;
        a.reason = r;
        return a;
    }

    RejectReason validate(OrderId id, Qty qty) const {
        if (id == 0) return RejectReason::InvalidId;
        if (qty == 0) return RejectReason::InvalidQty;
        if (ids_.find(id)) return RejectReason::DuplicateId;
        return RejectReason::None;
    }

    AddResult submit(OrderId id, Side side, Price limit, Qty qty, TimeInForce tif) {
        Qty filled = side == Side::Buy
                         ? match(asks_, id, side, limit, qty)
                         : match(bids_, id, side, limit, qty);
        qty -= filled;

        AddResult r;
        r.accepted = true;
        r.filled   = filled;
        if (qty > 0 && tif == TimeInForce::GTC) {
            if (side == Side::Buy) rest(bids_, id, side, limit, qty);
            else                   rest(asks_, id, side, limit, qty);
            r.resting = qty;
        }
        return r;
    }

    // Walk the opposite side from the best price while the incoming order
    // crosses. Returns the quantity filled.
    template <class Opp>
    Qty match(Opp& opp, OrderId taker, Side taker_side, Price limit, Qty want) {
        Qty filled = 0;
        while (want > 0 && !opp.empty()) {
            const Price best = opp.best_price();
            if (taker_side == Side::Buy ? best > limit : best < limit) break;

            Level& lv = opp.best_level();
            while (want > 0 && lv.head != kNil) {
                Node& maker = nodes_[lv.head];
                const Qty q = std::min(want, maker.qty);
                maker.qty -= q;
                lv.total  -= q;
                want      -= q;
                filled    += q;
                emit(Trade{++trade_seq_, taker, maker.id, best, q, taker_side});

                if (maker.qty == 0) {  // fully filled: pop from the FIFO head
                    const std::uint32_t idx = lv.head;
                    lv.head = maker.next;
                    if (lv.head != kNil) nodes_[lv.head].prev = kNil;
                    else                 lv.tail = kNil;
                    ids_.erase(maker.id);
                    free_node(idx);
                }
            }
            if (lv.head == kNil) opp.deactivate(best);
        }
        return filled;
    }

    template <class Levels>
    void rest(Levels& side_levels, OrderId id, Side side, Price price, Qty qty) {
        const std::uint32_t idx = alloc_node();
        Node& n = nodes_[idx];
        n = Node{id, price, qty, side, kNil, kNil};
        ids_.insert(id, idx);

        Level& lv = side_levels.level(price);
        const bool was_empty = lv.head == kNil;
        n.prev = lv.tail;  // append at the back of the FIFO
        if (lv.tail != kNil) nodes_[lv.tail].next = idx;
        else                 lv.head = idx;
        lv.tail = idx;
        lv.total += qty;
        if (was_empty) side_levels.activate(price);
    }

    // Unlink a node from its level; caller erases the id and frees the node.
    template <class Levels>
    void remove_resting(Levels& side_levels, std::uint32_t idx) {
        Node& n = nodes_[idx];
        Level& lv = side_levels.level(n.price);
        if (n.prev != kNil) nodes_[n.prev].next = n.next; else lv.head = n.next;
        if (n.next != kNil) nodes_[n.next].prev = n.prev; else lv.tail = n.prev;
        lv.total -= n.qty;
        if (lv.head == kNil) side_levels.deactivate(n.price);
    }

    std::uint32_t alloc_node() {
        if (free_head_ != kNil) {
            const std::uint32_t idx = free_head_;
            free_head_ = nodes_[idx].next;
            return idx;
        }
        nodes_.emplace_back();
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }
    void free_node(std::uint32_t idx) {
        nodes_[idx].next = free_head_;
        free_head_ = idx;
    }

    void emit(const Trade& t) {
        if (on_trade_) on_trade_(t);
    }

    Price min_, max_;
    typename Policy::Bids bids_;
    typename Policy::Asks asks_;
    IdMap                 ids_;
    std::vector<Node>     nodes_;       // order pool (stable indices)
    std::uint32_t         free_head_ = kNil;
    SeqNo                 trade_seq_ = 0;
    TradeHandler          on_trade_;
};

using MapOrderBook   = OrderBook<MapPolicy>;    // baseline
using ArrayOrderBook = OrderBook<ArrayPolicy>;  // tick-indexed (fast)

}  // namespace lob
