#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace matching_engine {

using OrderId = std::uint64_t;
using Price = std::int64_t;     // Integer ticks, never floating point.
using Quantity = std::int64_t;

enum class Side { Buy, Sell };

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
};

struct Trade {
    OrderId resting_order_id;
    OrderId incoming_order_id;
    Price price;
    Quantity quantity;
};

struct SubmitResult {
    std::vector<Trade> trades;
    Quantity remaining_quantity;
    bool resting;
};

struct BookLevel {
    Price price;
    Quantity quantity;
};

class OrderBook {
public:
    SubmitResult submit(Order order);
    bool cancel(OrderId id);

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] std::vector<BookLevel> bids() const;
    [[nodiscard]] std::vector<BookLevel> asks() const;
    [[nodiscard]] bool contains(OrderId id) const;

private:
    struct RestingOrder {
        Order order;
        std::uint64_t sequence;
    };

    using PriceLevel = std::vector<RestingOrder>;
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>> asks_;
    std::unordered_map<OrderId, Side> order_sides_;
    std::uint64_t next_sequence_ = 0;

    void validate(const Order& order) const;
    void rest(Order order);
};

[[nodiscard]] std::string format_price(Price price);

} // namespace matching_engine
