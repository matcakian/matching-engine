#include "matching_engine/order_book.hpp"

#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace matching_engine {

void OrderBook::validate(const Order& order) const {
    if (order.id == 0) {
        throw std::invalid_argument("order id must be non-zero");
    }
    if (order.price <= 0) {
        throw std::invalid_argument("price must be positive");
    }
    if (order.quantity <= 0) {
        throw std::invalid_argument("quantity must be positive");
    }
    if (contains(order.id)) {
        throw std::invalid_argument("duplicate order id");
    }
}

void OrderBook::rest(Order order) {
    const auto side = order.side;
    RestingOrder resting{std::move(order), next_sequence_++};
    if (side == Side::Buy) {
        bids_[resting.order.price].push_back(std::move(resting));
    } else {
        asks_[resting.order.price].push_back(std::move(resting));
    }
    order_sides_[resting.order.id] = side;
}

SubmitResult OrderBook::submit(Order order) {
    validate(order);
    SubmitResult result{{}, order.quantity, false};

    if (order.side == Side::Buy) {
        while (result.remaining_quantity > 0 && !asks_.empty() &&
               asks_.begin()->first <= order.price) {
            auto level = asks_.begin();
            auto& queue = level->second;
            while (result.remaining_quantity > 0 && !queue.empty()) {
                auto& resting = queue.front();
                const Quantity fill = std::min(result.remaining_quantity, resting.order.quantity);
                result.trades.push_back({resting.order.id, order.id, resting.order.price, fill});
                result.remaining_quantity -= fill;
                resting.order.quantity -= fill;
                if (resting.order.quantity == 0) {
                    order_sides_.erase(resting.order.id);
                    queue.erase(queue.begin());
                }
            }
            if (queue.empty()) {
                asks_.erase(level);
            }
        }
    } else {
        while (result.remaining_quantity > 0 && !bids_.empty() &&
               bids_.begin()->first >= order.price) {
            auto level = bids_.begin();
            auto& queue = level->second;
            while (result.remaining_quantity > 0 && !queue.empty()) {
                auto& resting = queue.front();
                const Quantity fill = std::min(result.remaining_quantity, resting.order.quantity);
                result.trades.push_back({resting.order.id, order.id, resting.order.price, fill});
                result.remaining_quantity -= fill;
                resting.order.quantity -= fill;
                if (resting.order.quantity == 0) {
                    order_sides_.erase(resting.order.id);
                    queue.erase(queue.begin());
                }
            }
            if (queue.empty()) {
                bids_.erase(level);
            }
        }
    }

    if (result.remaining_quantity > 0) {
        order.quantity = result.remaining_quantity;
        rest(std::move(order));
        result.resting = true;
    }
    return result;
}

bool OrderBook::cancel(OrderId id) {
    const auto side_it = order_sides_.find(id);
    if (side_it == order_sides_.end()) {
        return false;
    }

    const auto erase_from = [id](auto& levels) {
        for (auto level = levels.begin(); level != levels.end(); ++level) {
            auto& queue = level->second;
            const auto order = std::find_if(queue.begin(), queue.end(), [id](const RestingOrder& entry) {
                return entry.order.id == id;
            });
            if (order != queue.end()) {
                queue.erase(order);
                if (queue.empty()) {
                    levels.erase(level);
                }
                return true;
            }
        }
        return false;
    };

    const bool cancelled = side_it->second == Side::Buy ? erase_from(bids_) : erase_from(asks_);
    if (cancelled) {
        order_sides_.erase(side_it);
    }
    return cancelled;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::vector<BookLevel> OrderBook::bids() const {
    std::vector<BookLevel> result;
    for (const auto& [price, queue] : bids_) {
        result.push_back({price, std::accumulate(queue.begin(), queue.end(), Quantity{0},
            [](Quantity total, const RestingOrder& order) { return total + order.order.quantity; })});
    }
    return result;
}

std::vector<BookLevel> OrderBook::asks() const {
    std::vector<BookLevel> result;
    for (const auto& [price, queue] : asks_) {
        result.push_back({price, std::accumulate(queue.begin(), queue.end(), Quantity{0},
            [](Quantity total, const RestingOrder& order) { return total + order.order.quantity; })});
    }
    return result;
}

bool OrderBook::contains(OrderId id) const {
    return order_sides_.contains(id);
}

std::string format_price(Price price) {
    std::ostringstream output;
    output << price / 100 << '.' << std::setw(2) << std::setfill('0') << price % 100;
    return output.str();
}

} // namespace matching_engine
