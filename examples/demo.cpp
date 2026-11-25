#include <cstdio>

#include "lob/order_book.hpp"

using namespace lob;

static const char* side_str(Side s) { return s == Side::Buy ? "BUY " : "SELL"; }

int main() {
    ArrayOrderBook book(/*min_price=*/1, /*max_price=*/100000);
    book.set_trade_handler([](const Trade& t) {
        std::printf("  TRADE #%llu  %s taker=%llu maker=%llu  %u @ %lld\n",
                    (unsigned long long)t.seq, side_str(t.taker_side),
                    (unsigned long long)t.taker_id, (unsigned long long)t.maker_id, t.qty,
                    (long long)t.price);
    });

    auto show = [&] {
        auto bid = book.best_bid(), ask = book.best_ask();
        std::printf("  book: bid=");
        bid ? std::printf("%lld", (long long)*bid) : std::printf("-");
        std::printf(" ask=");
        ask ? std::printf("%lld", (long long)*ask) : std::printf("-");
        std::printf("  resting orders=%zu\n\n", book.order_count());
    };

    std::puts("1) Three sell orders: two at 10100 (FIFO), one at 10200");
    book.add_limit(1, Side::Sell, 10100, 50);
    book.add_limit(2, Side::Sell, 10100, 30);
    book.add_limit(3, Side::Sell, 10200, 40);
    show();

    std::puts("2) Buy 100 @ 10150 -> sweeps 10100 level in time order, rest bids");
    book.add_limit(4, Side::Buy, 10150, 100);
    show();

    std::puts("3) Cancel the resting bid, then market-buy 60");
    book.cancel(4);
    book.add_market(5, Side::Buy, 60);
    show();
    return 0;
}
