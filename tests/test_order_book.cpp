#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

// Every TYPED_TEST runs against BOTH the std::map book and the tick-array book,
// so the optimized implementation is held to the exact same behaviour.
template <class Book>
class OrderBookTest : public ::testing::Test {
protected:
    OrderBookTest() : book(1, 1000) {
        book.set_trade_handler([this](const Trade& t) { trades.push_back(t); });
    }
    Book book;
    std::vector<Trade> trades;
};

using BookTypes = ::testing::Types<MapOrderBook, ArrayOrderBook>;
TYPED_TEST_SUITE(OrderBookTest, BookTypes);

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

// 1
TYPED_TEST(OrderBookTest, EmptyBookHasNoBestPrices) {
    EXPECT_FALSE(this->book.best_bid().has_value());
    EXPECT_FALSE(this->book.best_ask().has_value());
    EXPECT_EQ(this->book.order_count(), 0u);
}

// 2
TYPED_TEST(OrderBookTest, RestingOrdersSetBestBidAndAsk) {
    auto r = this->book.add_limit(1, B, 99, 10);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.resting, 10u);
    this->book.add_limit(2, S, 101, 5);
    EXPECT_EQ(this->book.best_bid(), 99);
    EXPECT_EQ(this->book.best_ask(), 101);
    EXPECT_EQ(this->book.volume_at(B, 99), 10u);
    EXPECT_EQ(this->book.volume_at(S, 101), 5u);
}

// 3
TYPED_TEST(OrderBookTest, BestBidIsHighestPrice) {
    this->book.add_limit(1, B, 95, 1);
    this->book.add_limit(2, B, 98, 1);
    this->book.add_limit(3, B, 96, 1);
    EXPECT_EQ(this->book.best_bid(), 98);
}

// 4
TYPED_TEST(OrderBookTest, BestAskIsLowestPrice) {
    this->book.add_limit(1, S, 105, 1);
    this->book.add_limit(2, S, 102, 1);
    this->book.add_limit(3, S, 104, 1);
    EXPECT_EQ(this->book.best_ask(), 102);
}

// 5
TYPED_TEST(OrderBookTest, NonCrossingOrdersDoNotTrade) {
    this->book.add_limit(1, S, 100, 10);
    auto r = this->book.add_limit(2, B, 99, 10);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_TRUE(this->trades.empty());
    EXPECT_EQ(this->book.order_count(), 2u);
}

// 6
TYPED_TEST(OrderBookTest, FullMatchExecutesAtRestingPrice) {
    this->book.add_limit(1, S, 100, 10);
    auto r = this->book.add_limit(2, B, 105, 10);  // willing to pay more
    EXPECT_EQ(r.filled, 10u);
    EXPECT_EQ(r.resting, 0u);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->trades[0].price, 100);  // maker's price, not taker's
    EXPECT_EQ(this->trades[0].maker_id, 1u);
    EXPECT_EQ(this->trades[0].taker_id, 2u);
    EXPECT_EQ(this->book.order_count(), 0u);
    EXPECT_FALSE(this->book.best_ask().has_value());
}

// 7
TYPED_TEST(OrderBookTest, PartialFillRestsRemainderAndReducesMaker) {
    this->book.add_limit(1, S, 100, 4);
    auto r = this->book.add_limit(2, B, 100, 10);  // takes 4, rests 6
    EXPECT_EQ(r.filled, 4u);
    EXPECT_EQ(r.resting, 6u);
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.volume_at(B, 100), 6u);
    EXPECT_FALSE(this->book.best_ask().has_value());

    // Opposite case: the resting order is only partially consumed.
    this->book.add_limit(3, S, 105, 10);
    this->book.add_limit(4, B, 105, 3);
    EXPECT_EQ(this->book.remaining_qty(3), 7u);
    EXPECT_EQ(this->book.volume_at(S, 105), 7u);
    EXPECT_FALSE(this->book.contains(4));  // fully filled taker never rests
}

// 8
TYPED_TEST(OrderBookTest, PricePriorityBetterPriceMatchesFirst) {
    this->book.add_limit(1, S, 102, 5);
    this->book.add_limit(2, S, 100, 5);  // better price, arrived later
    this->book.add_limit(3, S, 101, 5);
    this->book.add_limit(4, B, 102, 5);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->trades[0].maker_id, 2u);
    EXPECT_EQ(this->trades[0].price, 100);
}

// 9
TYPED_TEST(OrderBookTest, TimePriorityIsFifoWithinPriceLevel) {
    this->book.add_limit(1, S, 100, 5);
    this->book.add_limit(2, S, 100, 5);
    this->book.add_limit(3, S, 100, 5);
    this->book.add_limit(4, B, 100, 12);
    ASSERT_EQ(this->trades.size(), 3u);
    EXPECT_EQ(this->trades[0].maker_id, 1u);
    EXPECT_EQ(this->trades[1].maker_id, 2u);
    EXPECT_EQ(this->trades[2].maker_id, 3u);
    EXPECT_EQ(this->trades[2].qty, 2u);
    EXPECT_EQ(this->book.remaining_qty(3), 3u);
}

// 10
TYPED_TEST(OrderBookTest, AggressiveOrderSweepsMultipleLevels) {
    this->book.add_limit(1, S, 100, 5);
    this->book.add_limit(2, S, 101, 5);
    this->book.add_limit(3, S, 102, 5);
    auto r = this->book.add_limit(4, B, 101, 20);  // limit stops at 101
    EXPECT_EQ(r.filled, 10u);
    EXPECT_EQ(r.resting, 10u);
    EXPECT_EQ(this->book.best_ask(), 102);
    EXPECT_EQ(this->book.best_bid(), 101);
    ASSERT_EQ(this->trades.size(), 2u);
    EXPECT_EQ(this->trades[0].price, 100);
    EXPECT_EQ(this->trades[1].price, 101);
}

// 11
TYPED_TEST(OrderBookTest, IocOrderNeverRests) {
    this->book.add_limit(1, S, 100, 4);
    auto r = this->book.add_limit(2, B, 100, 10, TimeInForce::IOC);
    EXPECT_EQ(r.filled, 4u);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_FALSE(this->book.contains(2));
    EXPECT_FALSE(this->book.best_bid().has_value());
}

// 12
TYPED_TEST(OrderBookTest, MarketOrderSweepsBookAndDiscardsRemainder) {
    this->book.add_limit(1, S, 100, 5);
    this->book.add_limit(2, S, 500, 5);
    auto r = this->book.add_market(3, B, 50);
    EXPECT_EQ(r.filled, 10u);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_FALSE(this->book.contains(3));
    EXPECT_FALSE(this->book.best_ask().has_value());
    EXPECT_EQ(this->book.order_count(), 0u);
}

// 13
TYPED_TEST(OrderBookTest, CancelRemovesOrderAndUpdatesBest) {
    this->book.add_limit(1, B, 100, 5);
    this->book.add_limit(2, B, 99, 5);
    EXPECT_TRUE(this->book.cancel(1));
    EXPECT_FALSE(this->book.contains(1));
    EXPECT_EQ(this->book.best_bid(), 99);
    EXPECT_EQ(this->book.volume_at(B, 100), 0u);
    EXPECT_TRUE(this->book.cancel(2));
    EXPECT_FALSE(this->book.best_bid().has_value());
}

// 14
TYPED_TEST(OrderBookTest, CancelUnknownOrFilledOrderReturnsFalse) {
    EXPECT_FALSE(this->book.cancel(42));
    this->book.add_limit(1, S, 100, 5);
    this->book.add_limit(2, B, 100, 5);  // fully fills order 1
    EXPECT_FALSE(this->book.cancel(1));
    EXPECT_FALSE(this->book.cancel(2));
}

// 15
TYPED_TEST(OrderBookTest, CancelFromMiddleOfQueuePreservesOtherPriorities) {
    this->book.add_limit(1, S, 100, 5);
    this->book.add_limit(2, S, 100, 5);
    this->book.add_limit(3, S, 100, 5);
    EXPECT_TRUE(this->book.cancel(2));
    EXPECT_EQ(this->book.volume_at(S, 100), 10u);
    this->book.add_limit(4, B, 100, 10);
    ASSERT_EQ(this->trades.size(), 2u);
    EXPECT_EQ(this->trades[0].maker_id, 1u);
    EXPECT_EQ(this->trades[1].maker_id, 3u);
}

// 16
TYPED_TEST(OrderBookTest, ModifyReduceQtyKeepsTimePriority) {
    this->book.add_limit(1, S, 100, 10);
    this->book.add_limit(2, S, 100, 10);
    auto r = this->book.modify(1, 100, 4);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(this->book.volume_at(S, 100), 14u);
    this->book.add_limit(3, B, 100, 5);
    ASSERT_EQ(this->trades.size(), 2u);
    EXPECT_EQ(this->trades[0].maker_id, 1u);  // still first in the queue
    EXPECT_EQ(this->trades[0].qty, 4u);
    EXPECT_EQ(this->trades[1].maker_id, 2u);
}

// 17
TYPED_TEST(OrderBookTest, ModifyPriceLosesPriorityAndCanCross) {
    this->book.add_limit(1, S, 100, 5);
    this->book.add_limit(2, S, 100, 5);
    this->book.modify(1, 100, 8);  // qty increase => back of the queue
    this->book.add_limit(3, B, 100, 5);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->trades[0].maker_id, 2u);

    // Re-pricing a resting bid through the spread executes immediately.
    this->trades.clear();
    this->book.add_limit(4, B, 90, 5);
    auto r = this->book.modify(4, 100, 5);
    EXPECT_EQ(r.filled, 5u);
    ASSERT_EQ(this->trades.size(), 1u);
    EXPECT_EQ(this->trades[0].maker_id, 1u);
    EXPECT_FALSE(this->book.contains(4));
}

// 18
TYPED_TEST(OrderBookTest, RejectsInvalidOrders) {
    this->book.add_limit(1, B, 100, 5);
    EXPECT_EQ(this->book.add_limit(1, B, 100, 5).reason, RejectReason::DuplicateId);
    EXPECT_EQ(this->book.add_limit(0, B, 100, 5).reason, RejectReason::InvalidId);
    EXPECT_EQ(this->book.add_limit(2, B, 100, 0).reason, RejectReason::InvalidQty);
    EXPECT_EQ(this->book.add_limit(3, B, 0, 5).reason, RejectReason::InvalidPrice);
    EXPECT_EQ(this->book.add_limit(4, S, 1001, 5).reason, RejectReason::InvalidPrice);
    EXPECT_EQ(this->book.modify(99, 100, 5).reason, RejectReason::UnknownOrder);
    EXPECT_FALSE(this->book.add_limit(1, B, 100, 5).accepted);
    EXPECT_EQ(this->book.order_count(), 1u);
}

// 19
TYPED_TEST(OrderBookTest, BestPriceAdvancesAcrossGapsAndSurvivesReuse) {
    // Sparse levels force the tick-array best-price scan to skip empty ticks.
    this->book.add_limit(1, S, 10, 1);
    this->book.add_limit(2, S, 500, 1);
    this->book.add_limit(3, S, 900, 1);
    this->book.add_market(4, B, 1);
    EXPECT_EQ(this->book.best_ask(), 500);
    this->book.cancel(2);
    EXPECT_EQ(this->book.best_ask(), 900);
    this->book.add_limit(5, S, 20, 1);  // new best re-appears below old one
    EXPECT_EQ(this->book.best_ask(), 20);
    this->book.add_market(6, B, 5);
    EXPECT_FALSE(this->book.best_ask().has_value());
    // Filled ids are free to be reused.
    EXPECT_TRUE(this->book.add_limit(1, B, 50, 1).accepted);
}

// ---------------------------------------------------------------------------
// Property test: both books must agree, trade-for-trade, with a deliberately
// naive O(n) reference matcher over a long random order stream.
// ---------------------------------------------------------------------------
struct RefOrder { OrderId id; Side side; Price price; Qty qty; std::uint64_t seq; };

struct ReferenceBook {
    std::vector<RefOrder> orders;
    std::vector<Trade>    trades;
    std::uint64_t         seq = 0;

    void submit(OrderId id, Side side, Price limit, Qty qty, bool rest) {
        const std::uint64_t my_seq = ++seq;
        while (qty > 0) {
            int best = -1;
            for (std::size_t i = 0; i < orders.size(); ++i) {
                const RefOrder& o = orders[i];
                if (o.side == side) continue;
                if (side == B ? o.price > limit : o.price < limit) continue;
                if (best < 0) { best = static_cast<int>(i); continue; }
                const RefOrder& b = orders[best];
                const bool better = side == B ? (o.price < b.price)
                                              : (o.price > b.price);
                if (better || (o.price == b.price && o.seq < b.seq)) best = static_cast<int>(i);
            }
            if (best < 0) break;
            RefOrder& m = orders[best];
            const Qty q = std::min(qty, m.qty);
            trades.push_back(Trade{trades.size() + 1, id, m.id, m.price, q, side});
            m.qty -= q;
            qty -= q;
            if (m.qty == 0) orders.erase(orders.begin() + best);
        }
        if (qty > 0 && rest) orders.push_back(RefOrder{id, side, limit, qty, my_seq});
    }
    void cancel(OrderId id) {
        for (std::size_t i = 0; i < orders.size(); ++i)
            if (orders[i].id == id) { orders.erase(orders.begin() + i); return; }
    }
};

template <class Book>
void run_random_against_reference(std::uint32_t seed) {
    Book book(1, 200);
    ReferenceBook ref;
    std::vector<Trade> got;
    book.set_trade_handler([&](const Trade& t) { got.push_back(t); });

    std::mt19937 rng(seed);
    std::vector<OrderId> live;
    OrderId next_id = 1;
    for (int i = 0; i < 20000; ++i) {
        const int op = static_cast<int>(rng() % 100);
        if (op < 70) {  // limit GTC (a few IOC)
            const Side side = (rng() & 1) ? B : S;
            const Price px = 90 + static_cast<Price>(rng() % 21);  // tight => lots of crossing
            const Qty q = 1 + rng() % 20;
            const bool ioc = (rng() % 10) == 0;
            const OrderId id = next_id++;
            book.add_limit(id, side, px, q, ioc ? TimeInForce::IOC : TimeInForce::GTC);
            ref.submit(id, side, px, q, !ioc);
            live.push_back(id);
        } else if (op < 80) {  // market
            const Side side = (rng() & 1) ? B : S;
            const Qty q = 1 + rng() % 40;
            const OrderId id = next_id++;
            book.add_market(id, side, q);
            ref.submit(id, side, side == B ? std::numeric_limits<Price>::max()
                                           : std::numeric_limits<Price>::min(), q, false);
        } else if (!live.empty()) {  // cancel (may already be filled; both must agree)
            const std::size_t k = rng() % live.size();
            book.cancel(live[k]);
            ref.cancel(live[k]);
            live[k] = live.back();
            live.pop_back();
        }
    }
    ASSERT_EQ(got.size(), ref.trades.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        ASSERT_EQ(got[i].taker_id, ref.trades[i].taker_id);
        ASSERT_EQ(got[i].maker_id, ref.trades[i].maker_id);
        ASSERT_EQ(got[i].price, ref.trades[i].price);
        ASSERT_EQ(got[i].qty, ref.trades[i].qty);
    }
    EXPECT_EQ(book.order_count(), ref.orders.size());
}

// 20
TYPED_TEST(OrderBookTest, RandomStreamMatchesNaiveReferenceModel) {
    for (std::uint32_t seed : {1u, 2u, 3u}) run_random_against_reference<TypeParam>(seed);
}
}  // namespace
