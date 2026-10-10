#include "matching_engine/order_book.hpp"

#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace matching_engine {

void OrderBook::validate(const Order& order) const {
    // An ID of zero is reserved as invalid so every stored order has a real key.
    if (order.id == 0) {
        throw std::invalid_argument("order id must be non-zero");
    }
    // A limit order must quote a positive price and request a positive amount.
    if (order.price <= 0) {
        throw std::invalid_argument("price must be positive");
    }
    if (order.quantity <= 0) {
        throw std::invalid_argument("quantity must be positive");
    }
    // IDs remain unique while the corresponding order is resting in the book.
    if (contains(order.id)) {
        throw std::invalid_argument("duplicate order id");
    }
}

void OrderBook::rest(Order order, std::optional<OwnerId> owner_id) {
    // Save these fields before `order` is moved into the price-level queue below.
    const auto side = order.side;
    const auto id = order.id;
    // Record an arrival sequence, then transfer ownership of the order to the
    // selected price-level queue. The queue's insertion order enforces FIFO.
    RestingOrder resting{std::move(order), owner_id, next_sequence_++};
    if (side == Side::Buy) {
        // operator[] creates the price level when this is its first bid.
        bids_[resting.order.price].push_back(std::move(resting));
    } else {
        // Equivalent behavior for the sell side of the book.
        asks_[resting.order.price].push_back(std::move(resting));
    }
    // Keep the side index in sync so an order can later be cancelled by ID.
    order_sides_[id] = side;
}

SubmitResult OrderBook::submit(Order order, std::optional<OwnerId> owner_id) {
    // Reject invalid input before changing any book state.
    validate(order);
    // Start with the whole incoming order unfilled and no resting remainder.
    SubmitResult result{{}, order.quantity, false};

    if (order.side == Side::Buy) {
        // A buyer may match only asks no more expensive than its limit. Since
        // asks are ascending, begin() is always the next best eligible level.
        auto level = asks_.begin();
        while (result.remaining_quantity > 0 && level != asks_.end() &&
               level->first <= order.price) {
            auto& queue = level->second;
            // Consume eligible orders in arrival order. A user's own resting
            // order remains available to other participants but cannot fill
            // this incoming order.
            for (auto resting = queue.begin();
                 result.remaining_quantity > 0 && resting != queue.end();) {
                if (owner_id && resting->owner_id && *owner_id == *resting->owner_id) {
                    ++resting;
                    continue;
                }
                // A fill cannot exceed either order's remaining quantity.
                const Quantity fill = std::min(result.remaining_quantity, resting->order.quantity);
                // Trades execute at the resting order's price under price-time priority.
                result.trades.push_back({resting->order.id, order.id, resting->order.price, fill});
                // Decrease both sides by the quantity just traded.
                result.remaining_quantity -= fill;
                resting->order.quantity -= fill;
                if (resting->order.quantity == 0) {
                    // A fully filled resting order is no longer cancellable.
                    order_sides_.erase(resting->order.id);
                    resting = queue.erase(resting);
                } else {
                    ++resting;
                }
            }
            if (queue.empty()) {
                // Do not retain an empty price level after its final order fills.
                level = asks_.erase(level);
            } else {
                ++level;
            }
        }
    } else {
        // A seller may match only bids at or above its limit. Bids are descending,
        // so begin() is the highest-priced eligible level.
        auto level = bids_.begin();
        while (result.remaining_quantity > 0 && level != bids_.end() &&
               level->first >= order.price) {
            auto& queue = level->second;
            // Preserve FIFO priority between every eligible bid at this price.
            for (auto resting = queue.begin();
                 result.remaining_quantity > 0 && resting != queue.end();) {
                if (owner_id && resting->owner_id && *owner_id == *resting->owner_id) {
                    ++resting;
                    continue;
                }
                const Quantity fill = std::min(result.remaining_quantity, resting->order.quantity);
                result.trades.push_back({resting->order.id, order.id, resting->order.price, fill});
                result.remaining_quantity -= fill;
                resting->order.quantity -= fill;
                if (resting->order.quantity == 0) {
                    order_sides_.erase(resting->order.id);
                    resting = queue.erase(resting);
                } else {
                    ++resting;
                }
            }
            if (queue.empty()) {
                // Remove an exhausted bid level before considering the next one.
                level = bids_.erase(level);
            } else {
                ++level;
            }
        }
    }

    if (result.remaining_quantity > 0) {
        // Only the unfilled part becomes a new resting order at the original limit.
        order.quantity = result.remaining_quantity;
        rest(std::move(order), owner_id);
        result.resting = true;
    }
    // Return every fill plus the portion (if any) that remains in the book.
    return result;
}

bool OrderBook::cancel(OrderId id) {
    // The side index both rejects unknown IDs quickly and selects the correct map.
    const auto side_it = order_sides_.find(id);
    if (side_it == order_sides_.end()) {
        return false;
    }

    // Search the indicated side's price levels for the requested order. A generic
    // lambda lets the same removal logic operate on bids and asks.
    const auto erase_from = [id](auto& levels) {
        for (auto level = levels.begin(); level != levels.end(); ++level) {
            auto& queue = level->second;
            // find_if locates the order without changing the FIFO order of others.
            const auto order = std::find_if(queue.begin(), queue.end(), [id](const RestingOrder& entry) {
                return entry.order.id == id;
            });
            if (order != queue.end()) {
                // Remove only the requested order from its price-level queue.
                queue.erase(order);
                if (queue.empty()) {
                    // Remove the map entry when this cancellation empties its level.
                    levels.erase(level);
                }
                return true;
            }
        }
        return false;
    };

    // Search only the side recorded when this order first rested.
    const bool cancelled = side_it->second == Side::Buy ? erase_from(bids_) : erase_from(asks_);
    if (cancelled) {
        // An erased order must no longer be reported as present or cancellable.
        order_sides_.erase(side_it);
    }
    return cancelled;
}

std::optional<Price> OrderBook::best_bid() const {
    // The descending bid map keeps the best price at its first entry.
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    // The ascending ask map likewise keeps the best ask at its first entry.
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::vector<BookLevel> OrderBook::bids() const {
    std::vector<BookLevel> result;
    // Report one aggregate quantity per price, in best-to-worst bid order.
    for (const auto& [price, queue] : bids_) {
        // Sum the quantities of all FIFO entries in this price level.
        result.push_back({price, std::accumulate(queue.begin(), queue.end(), Quantity{0},
            [](Quantity total, const RestingOrder& order) { return total + order.order.quantity; })});
    }
    return result;
}

std::vector<BookLevel> OrderBook::asks() const {
    std::vector<BookLevel> result;
    // Ask levels are already ordered lowest to highest by their map comparator.
    for (const auto& [price, queue] : asks_) {
        result.push_back({price, std::accumulate(queue.begin(), queue.end(), Quantity{0},
            [](Quantity total, const RestingOrder& order) { return total + order.order.quantity; })});
    }
    return result;
}

bool OrderBook::contains(OrderId id) const {
    // The index is the source of truth for whether an order is still resting.
    return order_sides_.contains(id);
}

std::string format_price(Price price) {
    std::ostringstream output;
    // Split cents into whole dollars and a zero-padded two-digit cents component.
    output << price / 100 << '.' << std::setw(2) << std::setfill('0') << price % 100;
    return output.str();
}

} // namespace matching_engine
