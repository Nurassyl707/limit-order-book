#pragma once
// A deliberately "textbook" order book used ONLY as a benchmark baseline:
//   std::map<Price, std::list<Order>> per side + std::unordered_map for cancels.
// This is the kind of first implementation most people write. It has the same
// semantics as lob::OrderBook (price-time priority, trade at resting price).
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <unordered_map>

#include "lob/types.hpp"

namespace lob_bench {

using namespace lob;

class NaiveOrderBook {
public:
    NaiveOrderBook(Price, Price, std::size_t expected = 0) { index_.reserve(expected); }
    void set_trade_handler(std::function<void(const Trade&)> h) { on_trade_ = std::move(h); }

    AddResult add_limit(OrderId id, Side side, Price price, Qty qty) {
        if (index_.count(id)) return {};
        return submit(id, side, price, qty, true);
    }
    AddResult add_market(OrderId id, Side side, Qty qty) {
        const Price lim = side == Side::Buy ? std::numeric_limits<Price>::max()
                                            : std::numeric_limits<Price>::min();
        return submit(id, side, lim, qty, false);
    }
    bool cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        Loc& l = it->second;
        if (l.side == Side::Buy) erase_from(bids_, l); else erase_from(asks_, l);
        index_.erase(it);
        return true;
    }
    std::size_t order_count() const { return index_.size(); }

private:
    struct Order { OrderId id; Qty qty; };
    using Queue = std::list<Order>;
    struct Loc { Side side; Price price; Queue::iterator it; };

    template <class M> void erase_from(M& m, Loc& l) {
        auto lv = m.find(l.price);
        lv->second.erase(l.it);
        if (lv->second.empty()) m.erase(lv);
    }

    AddResult submit(OrderId id, Side side, Price limit, Qty qty, bool rest) {
        Qty filled = side == Side::Buy ? match(asks_, id, side, limit, qty)
                                       : match(bids_, id, side, limit, qty);
        qty -= filled;
        AddResult r; r.accepted = true; r.filled = filled;
        if (qty > 0 && rest) {
            Queue& q = side == Side::Buy ? bids_[limit] : asks_[limit];
            q.push_back({id, qty});
            index_[id] = {side, limit, std::prev(q.end())};
            r.resting = qty;
        }
        return r;
    }

    template <class M>
    Qty match(M& opp, OrderId taker, Side ts, Price limit, Qty want) {
        Qty filled = 0;
        while (want > 0 && !opp.empty()) {
            auto lv = opp.begin();
            const Price px = lv->first;
            if (ts == Side::Buy ? px > limit : px < limit) break;
            Queue& q = lv->second;
            while (want > 0 && !q.empty()) {
                Order& m = q.front();
                const Qty f = std::min(want, m.qty);
                m.qty -= f; want -= f; filled += f;
                if (on_trade_) on_trade_(Trade{++seq_, taker, m.id, px, f, ts});
                if (m.qty == 0) { index_.erase(m.id); q.pop_front(); }
            }
            if (q.empty()) opp.erase(lv);
        }
        return filled;
    }

    std::map<Price, Queue, std::greater<Price>> bids_;
    std::map<Price, Queue, std::less<Price>>    asks_;
    std::unordered_map<OrderId, Loc>            index_;
    SeqNo seq_ = 0;
    std::function<void(const Trade&)> on_trade_;
};

}  // namespace lob_bench
