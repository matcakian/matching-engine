#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace matching_engine {

// Named integer types make the roles of otherwise identical numbers explicit.
using OrderId = std::uint64_t;
// Prices are stored as cents (for example, 10'050 represents $100.50), avoiding
// the rounding errors that can come from using floating-point currency values.
using Price = std::int64_t;
using Quantity = std::int64_t;

// The direction of an order: buyers consume asks and sellers consume bids.
enum class Side { Buy, Sell };

struct Order {
    OrderId id;          // Unique identifier used for cancellation and reporting.
    Side side;           // Whether this order buys or sells.
    Price price;         // Limit price: the worst price the trader accepts.
    Quantity quantity;   // Number of units still requested by this order.
};

struct Trade {
    OrderId resting_order_id;   // Existing order that supplied the liquidity.
    OrderId incoming_order_id;  // Newly submitted order that consumed it.
    Price price;                // Resting order's price, which sets the trade price.
    Quantity quantity;          // Number of units filled in this trade.
};

struct SubmitResult {
    std::vector<Trade> trades;       // One entry for every individual fill.
    Quantity remaining_quantity;     // Portion not matched immediately.
    bool resting;                    // True when the remaining portion joined the book.
};

struct BookLevel {
    Price price;         // The price shared by all orders in this level.
    Quantity quantity;   // Sum of the remaining quantities at that price.
};

class OrderBook {
public:
    // Validate and match a new limit order, then store any unfilled portion.
    SubmitResult submit(Order order);
    // Remove a resting order by ID. Returns false when the ID is not in the book.
    bool cancel(OrderId id);

    // Return the best available buy/sell price, or no value for an empty side.
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] std::vector<BookLevel> bids() const;
    [[nodiscard]] std::vector<BookLevel> asks() const;
    [[nodiscard]] bool contains(OrderId id) const;

private:
    // An order receives a sequence number when it rests, preserving time priority
    // among orders that share one price. The vector order provides that priority.
    struct RestingOrder {
        Order order;
        std::uint64_t sequence;
    };

    // A price level is a FIFO queue of orders at one price.
    using PriceLevel = std::vector<RestingOrder>;
    // Descending bids put the highest (best) bid at begin().
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    // Ascending asks put the lowest (best) ask at begin().
    std::map<Price, PriceLevel, std::less<Price>> asks_;
    // Fast ID lookup tells cancel() which side must be searched.
    std::unordered_map<OrderId, Side> order_sides_;
    // Monotonically increasing arrival marker for resting orders.
    std::uint64_t next_sequence_ = 0;

    // Reject malformed orders and IDs that already exist in the book.
    void validate(const Order& order) const;
    // Add an unmatched order to its price-level FIFO queue.
    void rest(Order order);
};

// Convert a price stored in cents into a fixed two-decimal display string.
[[nodiscard]] std::string format_price(Price price);

} // namespace matching_engine
