#pragma once

#include "matching_engine/order_book.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace matching_engine {

using UserId = OwnerId;

struct AuthenticatedUser {
    UserId id;
    std::string username;
};

struct Session {
    // This raw value is returned once so the HTTP layer can put it in an
    // HttpOnly cookie. Only its SHA-256 digest is stored in SQLite.
    std::string token;
    AuthenticatedUser user;
    std::chrono::system_clock::time_point expires_at;
};

enum class AuthenticationError {
    None,
    InvalidUsername,
    InvalidPassword,
    UsernameTaken,
    InvalidCredentials,
};

struct AuthenticationResult {
    AuthenticationError error = AuthenticationError::None;
    std::optional<Session> session;
};

enum class OrderStatus { Open, Filled, Cancelled };

struct StoredOrder {
    Order order;
    // System-provided seed liquidity intentionally has no user owner.
    std::optional<UserId> owner_id;
    Quantity original_quantity;
    OrderStatus status;
};

// SQLite-backed persistence for users, sessions, and order ownership. The
// matching engine remains responsible for price-time matching; this store is
// the durable source of account and per-user order data.
class UserStore {
public:
    explicit UserStore(const std::string& database_path);
    ~UserStore();

    UserStore(const UserStore&) = delete;
    UserStore& operator=(const UserStore&) = delete;

    [[nodiscard]] AuthenticationResult register_user(std::string_view username,
                                                      std::string_view password);
    [[nodiscard]] AuthenticationResult login(std::string_view username,
                                             std::string_view password);
    [[nodiscard]] std::optional<AuthenticatedUser> authenticate(std::string_view token) const;
    void logout(std::string_view token);

    // Order IDs are persisted, so an engine restart cannot reuse a historical
    // ID. Open orders can then be loaded into a new in-memory OrderBook.
    [[nodiscard]] OrderId next_order_id() const;
    [[nodiscard]] bool has_orders() const;
    void add_seed_order(const Order& order);
    [[nodiscard]] std::vector<StoredOrder> open_orders() const;
    [[nodiscard]] std::vector<StoredOrder> open_orders_for_user(UserId owner_id) const;
    [[nodiscard]] bool user_owns_open_order(UserId owner_id, OrderId order_id) const;

    // Call after a successful OrderBook::submit. This records the incoming
    // order and applies every fill to the affected resting orders together.
    void record_submission(UserId owner_id, const Order& submitted, const SubmitResult& result);
    void mark_cancelled(UserId owner_id, OrderId order_id);

private:
    sqlite3* database_ = nullptr;
};

} // namespace matching_engine
