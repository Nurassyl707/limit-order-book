// Throughput benchmark: replays the SAME pre-generated order stream through the
// std::map-based book and the tick-indexed-array book.
//
//   ./bench_book [--orders N] [--reps R] [--seed S] [--only map|array]
//
// The stream mimics a liquid instrument: a random-walking mid price, passive
// orders clustered around it (exponentially distributed distance, so the book
// holds thousands of price levels), some marketable limits, market orders and
// cancels. Generation happens outside the timed region.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "lob/order_book.hpp"
#include "naive_book.hpp"

using namespace lob;

namespace {

enum class OpType : std::uint8_t { Limit, Market, Cancel };

struct Op {
    OpType  type;
    Side    side;
    Price   price;
    Qty     qty;
    OrderId id;  // order id (Limit/Market) or target id (Cancel)
};

constexpr Price kMinPrice = 1;
constexpr Price kMaxPrice = 100000;

std::vector<Op> generate(std::size_t n, std::uint32_t seed, double mean_depth) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::exponential_distribution<double> dist(1.0 / mean_depth);  // mean distance (ticks) from mid
    std::uniform_int_distribution<int> qty_d(1, 100);

    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<OrderId> issued;  // limit ids we may later cancel
    issued.reserve(n);

    Price mid = 50000;
    OrderId next_id = 1;
    for (std::size_t i = 0; i < n; ++i) {
        // mid price random walk
        const double w = u01(rng);
        if (w < 0.10) mid += 1; else if (w < 0.20) mid -= 1;
        mid = std::clamp<Price>(mid, 5000, kMaxPrice - 5000);

        const double r = u01(rng);
        const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        const Qty q = static_cast<Qty>(qty_d(rng));

        if (r < 0.60) {  // passive limit, rests behind the touch
            const Price off = 1 + static_cast<Price>(dist(rng));
            const Price px = side == Side::Buy ? mid - off : mid + off;
            const OrderId id = next_id++;
            ops.push_back({OpType::Limit, side, std::clamp(px, kMinPrice, kMaxPrice), q, id});
            issued.push_back(id);
        } else if (r < 0.70) {  // aggressive limit: crosses into the other side
            const Price off = static_cast<Price>(dist(rng) / 4.0);
            const Price px = side == Side::Buy ? mid + off : mid - off;
            const OrderId id = next_id++;
            ops.push_back({OpType::Limit, side, std::clamp(px, kMinPrice, kMaxPrice), q, id});
            issued.push_back(id);
        } else if (r < 0.75) {  // market order
            ops.push_back({OpType::Market, side, 0, q, next_id++});
        } else if (!issued.empty()) {  // cancel a random earlier order (may be filled already)
            const std::size_t k = rng() % issued.size();
            ops.push_back({OpType::Cancel, Side::Buy, 0, 0, issued[k]});
        } else {
            ops.push_back({OpType::Market, side, 0, q, next_id++});
        }
    }
    return ops;
}

struct Result {
    double        seconds;
    std::uint64_t trades;
    std::uint64_t traded_qty;
    std::size_t   resting;
};

template <class Book>
Result run(const std::vector<Op>& ops) {
    Book book(kMinPrice, kMaxPrice, ops.size());
    std::uint64_t trades = 0, traded_qty = 0;
    book.set_trade_handler([&](const Trade& t) { ++trades; traded_qty += t.qty; });

    const auto t0 = std::chrono::steady_clock::now();
    for (const Op& op : ops) {
        switch (op.type) {
            case OpType::Limit:  book.add_limit(op.id, op.side, op.price, op.qty); break;
            case OpType::Market: book.add_market(op.id, op.side, op.qty); break;
            case OpType::Cancel: book.cancel(op.id); break;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    return {std::chrono::duration<double>(t1 - t0).count(), trades, traded_qty, book.order_count()};
}

template <class Book>
Result best_of(const std::vector<Op>& ops, int reps) {
    Result best = run<Book>(ops);  // first run doubles as warm-up
    for (int i = 1; i < reps; ++i) {
        Result r = run<Book>(ops);
        if (r.seconds < best.seconds) best = r;
    }
    return best;
}

void report(const char* name, const Result& r, std::size_t n) {
    std::printf("%-22s %8.1f ms  %7.2f M ops/s  %6.1f ns/op  trades=%llu resting=%zu\n", name,
                r.seconds * 1e3, n / r.seconds / 1e6, r.seconds * 1e9 / n,
                static_cast<unsigned long long>(r.trades), r.resting);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 1'000'000;
    int reps = 5;
    std::uint32_t seed = 12345;
    std::string only;
    double depth = 150.0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--orders") && i + 1 < argc) n = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--depth") && i + 1 < argc) depth = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else { std::fprintf(stderr, "usage: %s [--orders N] [--reps R] [--seed S] [--depth TICKS] [--only naive|map|array]\n", argv[0]); return 2; }
    }

    std::printf("generating %zu orders (seed %u, mean distance from mid %.0f ticks)...\n", n, seed, depth);
    const std::vector<Op> ops = generate(n, seed, depth);
    std::printf("best of %d runs\n", reps);

    using lob_bench::NaiveOrderBook;
    Result v{}, m{}, a{};
    if (only.empty() || only == "naive") { v = best_of<NaiveOrderBook>(ops, reps); report("naive (map+list+umap)", v, n); }
    if (only.empty() || only == "map")   { m = best_of<MapOrderBook>(ops, reps);   report("std::map levels", m, n); }
    if (only.empty() || only == "array") { a = best_of<ArrayOrderBook>(ops, reps); report("tick-array levels", a, n); }

    if (only.empty()) {
        const bool same = v.trades == m.trades && m.trades == a.trades &&
                          v.traded_qty == m.traded_qty && m.traded_qty == a.traded_qty &&
                          v.resting == m.resting && m.resting == a.resting;
        if (!same) { std::fprintf(stderr, "MISMATCH between implementations!\n"); return 1; }
        std::printf("all three implementations produced identical results\n");
        std::printf("speedup  array vs map levels : %.2fx   (isolates the price-level container)\n", m.seconds / a.seconds);
        std::printf("speedup  array vs naive      : %.2fx   (full stack: pool + flat id map + array)\n", v.seconds / a.seconds);
    }
    return 0;
}
