#include "matching_engine/order_book.hpp"

#include <iostream>

using namespace matching_engine;

int main() {
    // Create an empty book, then seed it with two sell orders (asks).
    OrderBook book;
    // 10'050 is stored in cents and represents $100.50.
    book.submit({1, Side::Sell, 10'050, 100});
    book.submit({2, Side::Sell, 10'075, 50});

    // This buy order crosses both asks: it fills 100 at $100.50 and 20 at $100.75.
    const auto result = book.submit({3, Side::Buy, 10'075, 120});
    // Display each individual match created while submitting the incoming order.
    for (const auto& trade : result.trades) {
        std::cout << "TRADE resting=" << trade.resting_order_id
                  << " incoming=" << trade.incoming_order_id
                  << " quantity=" << trade.quantity
                  << " price=$" << format_price(trade.price) << '\n';
    }

    // Print the remaining sell side, followed by the remaining buy side.
    std::cout << "\nOrder book\nASKS\n";
    for (const auto& level : book.asks()) {
        std::cout << level.quantity << " @ $" << format_price(level.price) << '\n';
    }
    std::cout << "BIDS\n";
    for (const auto& level : book.bids()) {
        std::cout << level.quantity << " @ $" << format_price(level.price) << '\n';
    }
}
