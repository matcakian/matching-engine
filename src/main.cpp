#include "matching_engine/order_book.hpp"

#include <iostream>

using namespace matching_engine;

int main() {
    OrderBook book;
    book.submit({1, Side::Sell, 10'050, 100});
    book.submit({2, Side::Sell, 10'075, 50});

    const auto result = book.submit({3, Side::Buy, 10'075, 120});
    for (const auto& trade : result.trades) {
        std::cout << "TRADE resting=" << trade.resting_order_id
                  << " incoming=" << trade.incoming_order_id
                  << " quantity=" << trade.quantity
                  << " price=$" << format_price(trade.price) << '\n';
    }

    std::cout << "\nOrder book\nASKS\n";
    for (const auto& level : book.asks()) {
        std::cout << level.quantity << " @ $" << format_price(level.price) << '\n';
    }
    std::cout << "BIDS\n";
    for (const auto& level : book.bids()) {
        std::cout << level.quantity << " @ $" << format_price(level.price) << '\n';
    }
}
