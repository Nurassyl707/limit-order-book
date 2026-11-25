#pragma once
// Price-level containers ("book side" policies).
//
// The order book is templated on one of these, so the SAME matching logic runs
// over either:
//   * MapLevels   - std::map<Price, Level>   (baseline, O(log L) per level op)
//   * ArrayLevels - tick-indexed flat array  (optimized, O(1) per level op)
//
// A Level is a FIFO queue (intrusive doubly linked list of pool indices), which
// is what gives price-time priority within a price.
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <vector>

#include "lob/types.hpp"

namespace lob {

inline constexpr std::uint32_t kNil = 0xFFFFFFFFu;

struct Level {
    std::uint32_t head  = kNil;  // oldest order (matched first)
    std::uint32_t tail  = kNil;  // newest order
    Qty           total = 0;     // aggregate resting quantity at this price
};

// ---------------------------------------------------------------------------
// Baseline: ordered map. `Better` decides which end is "best":
//   bids -> std::greater (highest first), asks -> std::less (lowest first).
// ---------------------------------------------------------------------------
template <class Better>
class MapLevels {
public:
    MapLevels(Price /*min_price*/, Price /*max_price*/) {}

    static constexpr bool in_range(Price) noexcept { return true; }

    bool  empty() const noexcept { return levels_.empty(); }
    Price best_price() const { return levels_.begin()->first; }
    Level& best_level() { return levels_.begin()->second; }

    Level& level(Price p) { return levels_[p]; }  // creates on demand
    const Level* find(Price p) const {
        auto it = levels_.find(p);
        return it == levels_.end() ? nullptr : &it->second;
    }

    void activate(Price) noexcept {}  // map creates the node in level()
    void deactivate(Price p) { levels_.erase(p); }

private:
    std::map<Price, Level, Better> levels_;
};

// ---------------------------------------------------------------------------
// Optimized: one Level per tick in a contiguous array. A cached best index
// makes best-price lookup O(1); when the best level empties we scan toward the
// spread to the next non-empty level (bounded by `active_` so we never scan an
// empty side).
// ---------------------------------------------------------------------------
template <bool IsBid>
class ArrayLevels {
public:
    ArrayLevels(Price min_price, Price max_price)
        : min_(min_price), max_(max_price) {
        if (max_price < min_price) throw std::invalid_argument("max_price < min_price");
        levels_.resize(static_cast<std::size_t>(max_price - min_price + 1));
        best_ = IsBid ? -1 : static_cast<std::int64_t>(levels_.size());
    }

    bool in_range(Price p) const noexcept { return p >= min_ && p <= max_; }

    bool  empty() const noexcept { return active_ == 0; }
    Price best_price() const noexcept { return best_ + min_; }
    Level& best_level() noexcept { return levels_[static_cast<std::size_t>(best_)]; }

    Level& level(Price p) noexcept { return levels_[static_cast<std::size_t>(p - min_)]; }
    const Level* find(Price p) const noexcept {
        if (!in_range(p)) return nullptr;
        const Level& l = levels_[static_cast<std::size_t>(p - min_)];
        return l.head == kNil ? nullptr : &l;
    }

    void activate(Price p) noexcept {
        ++active_;
        const std::int64_t idx = p - min_;
        if (IsBid ? idx > best_ : idx < best_) best_ = idx;
    }

    void deactivate(Price p) noexcept {
        --active_;
        if (active_ == 0) {
            best_ = IsBid ? -1 : static_cast<std::int64_t>(levels_.size());
            return;
        }
        const std::int64_t idx = p - min_;
        if (idx != best_) return;
        // Walk toward the spread until the next populated level.
        if (IsBid) { do { --best_; } while (levels_[static_cast<std::size_t>(best_)].head == kNil); }
        else       { do { ++best_; } while (levels_[static_cast<std::size_t>(best_)].head == kNil); }
    }

private:
    Price              min_, max_;
    std::vector<Level> levels_;
    std::int64_t       best_   = 0;
    std::size_t        active_ = 0;  // number of non-empty levels
};

// Policy bundles so the book can be written as OrderBook<MapPolicy>.
struct MapPolicy {
    using Bids = MapLevels<std::greater<Price>>;
    using Asks = MapLevels<std::less<Price>>;
};
struct ArrayPolicy {
    using Bids = ArrayLevels<true>;
    using Asks = ArrayLevels<false>;
};

}  // namespace lob
