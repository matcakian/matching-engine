#include "matching_engine/order_book.hpp"

#include <cassert>
#include <stdexcept>

using namespace matching_engine;

int main() {
    // Two orders at one price must fill in arrival order (price-time priority).
    {
        OrderBook book;
        book.submit({1, Side::Sell, 10'100, 10});
        book.submit({2, Side::Sell, 10'100, 15});
        const auto result = book.submit({3, Side::Buy, 10'100, 20});

        // The 20-unit buy consumes all 10 units of order 1, then 10 of order 2.
        assert(result.trades.size() == 2);
        assert(result.trades[0].resting_order_id == 1);
        assert(result.trades[0].quantity == 10);
        assert(result.trades[1].resting_order_id == 2);
        assert(result.trades[1].quantity == 10);
        assert(result.remaining_quantity == 0);
        assert(book.asks().size() == 1);
        assert(book.asks()[0].quantity == 5);
    }

    // A partially filled incoming order should rest at its own limit price.
    {
        OrderBook book;
        book.submit({10, Side::Sell, 10'100, 5});
        book.submit({11, Side::Sell, 10'200, 10});
        const auto result = book.submit({12, Side::Buy, 10'150, 9});

        assert(result.trades.size() == 1);
        assert(result.trades[0].price == 10'100);
        assert(result.remaining_quantity == 4);
        assert(result.resting);
        assert(book.best_bid() == 10'150);
        assert(book.best_ask() == 10'200);
    }

    // Cancelling removes the order from both its price level and the ID index.
    {
        OrderBook book;
        book.submit({20, Side::Buy, 9'900, 8});
        assert(book.cancel(20));
        assert(!book.contains(20));
        assert(!book.cancel(20));
        assert(!book.best_bid().has_value());
    }

    // Invalid orders are rejected before they can affect the book.
    {
        OrderBook book;
        bool rejected = false;
        // An ID of zero is intentionally invalid.
        try { book.submit({0, Side::Buy, 10'000, 1}); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
}
