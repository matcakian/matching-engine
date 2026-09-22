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

    // A marketable order sweeps price levels from best to worst, while preserving
    // FIFO order for the two resting orders at the best price.
    {
        OrderBook book;
        book.submit({101, Side::Sell, 10'100, 7});
        book.submit({102, Side::Sell, 10'100, 6});
        book.submit({103, Side::Sell, 10'125, 10});
        book.submit({104, Side::Sell, 10'150, 4});
        const auto result = book.submit({105, Side::Buy, 10'125, 20});

        assert(result.trades.size() == 3);
        assert(result.trades[0].resting_order_id == 101);
        assert(result.trades[0].price == 10'100);
        assert(result.trades[0].quantity == 7);
        assert(result.trades[1].resting_order_id == 102);
        assert(result.trades[1].price == 10'100);
        assert(result.trades[1].quantity == 6);
        assert(result.trades[2].resting_order_id == 103);
        assert(result.trades[2].price == 10'125);
        assert(result.trades[2].quantity == 7);
        assert(result.remaining_quantity == 0);
        assert(!result.resting);
        assert(!book.contains(101));
        assert(!book.contains(102));
        assert(book.contains(103));
        assert(book.asks().size() == 2);
        assert(book.asks()[0].price == 10'125);
        assert(book.asks()[0].quantity == 3);
        assert(book.asks()[1].price == 10'150);
        assert(book.asks()[1].quantity == 4);
        assert(!book.best_bid().has_value());
        assert(book.best_ask() == 10'125);
    }

    // The same price priority applies to incoming sells: they consume bids from
    // highest to lowest, then use arrival order within an equal-price level.
    {
        OrderBook book;
        book.submit({201, Side::Buy, 10'200, 4});
        book.submit({202, Side::Buy, 10'175, 3});
        book.submit({203, Side::Buy, 10'175, 8});
        book.submit({204, Side::Buy, 10'100, 10});
        const auto result = book.submit({205, Side::Sell, 10'175, 12});

        assert(result.trades.size() == 3);
        assert(result.trades[0].resting_order_id == 201);
        assert(result.trades[0].price == 10'200);
        assert(result.trades[0].quantity == 4);
        assert(result.trades[1].resting_order_id == 202);
        assert(result.trades[1].price == 10'175);
        assert(result.trades[1].quantity == 3);
        assert(result.trades[2].resting_order_id == 203);
        assert(result.trades[2].price == 10'175);
        assert(result.trades[2].quantity == 5);
        assert(result.remaining_quantity == 0);
        assert(book.bids().size() == 2);
        assert(book.bids()[0].price == 10'175);
        assert(book.bids()[0].quantity == 3);
        assert(book.bids()[1].price == 10'100);
        assert(book.bids()[1].quantity == 10);
        assert(book.best_bid() == 10'175);
        assert(!book.best_ask().has_value());
    }

    // Removing an order in the middle of a FIFO queue must leave its neighbours
    // in order, so the later order cannot jump ahead after the cancellation.
    {
        OrderBook book;
        book.submit({301, Side::Sell, 10'000, 3});
        book.submit({302, Side::Sell, 10'000, 4});
        book.submit({303, Side::Sell, 10'000, 5});
        assert(book.cancel(302));

        const auto result = book.submit({304, Side::Buy, 10'000, 7});
        assert(result.trades.size() == 2);
        assert(result.trades[0].resting_order_id == 301);
        assert(result.trades[0].quantity == 3);
        assert(result.trades[1].resting_order_id == 303);
        assert(result.trades[1].quantity == 4);
        assert(!book.contains(301));
        assert(!book.contains(302));
        assert(book.contains(303));
        assert(book.asks().size() == 1);
        assert(book.asks()[0].quantity == 1);
    }

    // A partially filled buy can rest, become the best bid, and later be matched
    // by a sell that itself has a remainder at a new ask price.
    {
        OrderBook book;
        book.submit({401, Side::Sell, 10'100, 5});
        book.submit({402, Side::Buy, 9'900, 8});
        const auto buy = book.submit({403, Side::Buy, 10'150, 9});

        assert(buy.trades.size() == 1);
        assert(buy.trades[0].resting_order_id == 401);
        assert(buy.trades[0].price == 10'100);
        assert(buy.trades[0].quantity == 5);
        assert(buy.remaining_quantity == 4);
        assert(buy.resting);
        assert(book.best_bid() == 10'150);
        assert(!book.best_ask().has_value());

        const auto sell = book.submit({404, Side::Sell, 10'000, 7});
        assert(sell.trades.size() == 1);
        assert(sell.trades[0].resting_order_id == 403);
        assert(sell.trades[0].price == 10'150);
        assert(sell.trades[0].quantity == 4);
        assert(sell.remaining_quantity == 3);
        assert(sell.resting);
        assert(!book.contains(403));
        assert(book.contains(402));
        assert(book.contains(404));
        assert(book.best_bid() == 9'900);
        assert(book.best_ask() == 10'000);
        assert(book.bids().size() == 1);
        assert(book.bids()[0].quantity == 8);
        assert(book.asks().size() == 1);
        assert(book.asks()[0].quantity == 3);
    }

    // Every rejected input leaves existing depth and cancellation state unchanged.
    {
        OrderBook book;
        book.submit({501, Side::Buy, 10'000, 9});

        bool duplicate_rejected = false;
        bool price_rejected = false;
        bool quantity_rejected = false;
        try { book.submit({501, Side::Sell, 10'000, 1}); } catch (const std::invalid_argument&) { duplicate_rejected = true; }
        try { book.submit({502, Side::Sell, 0, 1}); } catch (const std::invalid_argument&) { price_rejected = true; }
        try { book.submit({503, Side::Sell, 10'000, -1}); } catch (const std::invalid_argument&) { quantity_rejected = true; }

        assert(duplicate_rejected);
        assert(price_rejected);
        assert(quantity_rejected);
        assert(book.contains(501));
        assert(book.bids().size() == 1);
        assert(book.bids()[0].price == 10'000);
        assert(book.bids()[0].quantity == 9);
        assert(book.asks().empty());
        assert(book.cancel(501));
    }

    // A filled or cancelled ID is no longer in the resting-order index and may be
    // used again; the replacement behaves as a newly arrived order.
    {
        OrderBook book;
        book.submit({601, Side::Sell, 10'000, 2});
        book.submit({602, Side::Buy, 10'000, 2});
        assert(!book.contains(601));
        book.submit({601, Side::Buy, 9'950, 3});
        assert(book.contains(601));
        assert(book.cancel(601));
        book.submit({601, Side::Sell, 10'050, 4});
        assert(book.contains(601));
        assert(book.best_ask() == 10'050);
        assert(book.asks()[0].quantity == 4);
    }

    // Price formatting must preserve zero-padded cents at both small and whole
    // dollar values used by order and trade reporting.
    {
        assert(format_price(1) == "0.01");
        assert(format_price(10'000) == "100.00");
        assert(format_price(10'050) == "100.50");
    }
}
