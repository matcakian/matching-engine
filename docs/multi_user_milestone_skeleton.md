# Multi-user first-milestone skeleton

This is a deliberately non-runnable skeleton for the first multi-user milestone.
It documents the seams to add without changing the current matching rules:

- users can register and log in;
- the server derives an order's owner from the authenticated session;
- a signed-in user can retrieve only their currently open orders.

The browser must **not** send a `userId` when placing an order. The server gets
the identity from the session and supplies the owner itself.

## 1. Persistent records

Use a relational database in the real implementation. These are the minimum
records; migrations should create equivalent tables and indexes.

```sql
-- User IDs are internal and never chosen by a browser.
CREATE TABLE users (
  id            BIGINT PRIMARY KEY,
  username      TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Store only a hash/digest of a random session token. A leaked database must
-- not be sufficient to impersonate a signed-in user.
CREATE TABLE sessions (
  token_digest TEXT PRIMARY KEY,
  user_id      BIGINT NOT NULL REFERENCES users(id),
  expires_at   TIMESTAMPTZ NOT NULL
);

-- `remaining_quantity > 0` represents a resting order. Keep the owner even
-- after an order fills or is cancelled so later trade/audit work is possible.
CREATE TABLE orders (
  id                 BIGINT PRIMARY KEY,
  user_id            BIGINT NOT NULL REFERENCES users(id),
  side               TEXT NOT NULL CHECK (side IN ('buy', 'sell')),
  price              BIGINT NOT NULL CHECK (price > 0),
  original_quantity  BIGINT NOT NULL CHECK (original_quantity > 0),
  remaining_quantity BIGINT NOT NULL CHECK (remaining_quantity >= 0),
  status             TEXT NOT NULL CHECK (status IN ('open', 'filled', 'cancelled')),
  created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Makes GET /api/orders?status=open efficient and naturally user-scoped.
CREATE INDEX orders_open_by_user ON orders (user_id, status, created_at DESC);
```

## 2. Domain and persistence interfaces

The eventual production versions belong in headers and `.cpp` files. Keeping
them behind interfaces prevents the HTTP server from knowing database details.

```cpp
// include/matching_engine/multi_user.hpp
// Skeleton only: do not add this to CMake until its implementations exist.

#pragma once

#include "matching_engine/order_book.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace matching_engine {

using UserId = std::uint64_t;

struct User {
    UserId id;
    std::string username;
    // Never return password_hash to a browser or place it in logs.
    std::string password_hash;
};

struct Session {
    // This is an opaque, cryptographically-random value sent only in a secure,
    // HttpOnly cookie. The database stores a digest, not this raw token.
    std::string token;
    UserId user_id;
    std::chrono::system_clock::time_point expires_at;
};

struct OwnedOrder {
    // Reuse the existing matching order fields. `owner_id` is attached by the
    // server after authentication, rather than accepted from JSON input.
    Order order;
    UserId owner_id;
    Quantity original_quantity;
    enum class Status { Open, Filled, Cancelled } status;
};

class UserRepository {
public:
    virtual ~UserRepository() = default;

    // Enforce username uniqueness at both the database and application layers.
    virtual std::optional<User> find_by_username(std::string_view username) const = 0;
    virtual User create(std::string_view username, std::string_view password_hash) = 0;
};

class SessionRepository {
public:
    virtual ~SessionRepository() = default;

    // `raw_token` is digested before persistence. Expired sessions must not be
    // returned by find_valid.
    virtual Session create(UserId user_id, std::chrono::system_clock::time_point expires_at) = 0;
    virtual std::optional<Session> find_valid(std::string_view raw_token) const = 0;
    virtual void revoke(std::string_view raw_token) = 0;
};

class OrderRepository {
public:
    virtual ~OrderRepository() = default;

    // The caller passes the owner from the session; the repository must persist
    // it with the resting order in the same transaction as book state changes.
    virtual void save_open(const OwnedOrder& order) = 0;

    // This query always includes `owner_id`; it must never be implemented as an
    // unfiltered "all open orders" query for an authenticated browser endpoint.
    virtual std::vector<OwnedOrder> find_open_for_user(UserId owner_id) const = 0;

    // Include the owner in single-order lookups and updates as well. This is
    // what lets cancellation safely enforce ownership.
    virtual std::optional<OwnedOrder> find_open_for_user_and_id(UserId owner_id,
                                                                  OrderId order_id) const = 0;
    virtual void mark_cancelled(OrderId order_id, UserId owner_id) = 0;
};

class PasswordService {
public:
    // Use a vetted password-hashing implementation (for example Argon2id or
    // bcrypt), including a unique salt. Do not implement password hashing here.
    virtual std::string hash(std::string_view plain_text_password) const = 0;
    virtual bool verifies(std::string_view plain_text_password,
                          std::string_view password_hash) const = 0;
};

class AuthenticationService {
public:
    // Registration validates input, hashes the password, creates a user, then
    // creates a session. Login verifies the existing hash before creating one.
    virtual Session register_user(std::string_view username,
                                  std::string_view plain_text_password) = 0;
    virtual std::optional<Session> login(std::string_view username,
                                         std::string_view plain_text_password) = 0;

    // HTTP handlers call this once per protected request after extracting the
    // session cookie. A missing/expired token becomes HTTP 401.
    virtual std::optional<UserId> authenticate(std::string_view raw_token) const = 0;
};

} // namespace matching_engine
```

## 3. HTTP route skeleton

The existing `EngineServer::handle` method is the integration point. A production
version needs real JSON parsing and a secure cookie parser; the snippets show
responsibilities rather than complete request parsing.

```cpp
// src/http_server.cpp -- conceptual additions

Response EngineServer::handle(std::string_view method,
                              std::string_view path,
                              std::string_view body,
                              std::string_view cookie_header) {
    if (method == "POST" && path == "/api/register") return register_user(body);
    if (method == "POST" && path == "/api/login")    return login(body);
    if (method == "POST" && path == "/api/logout")   return logout(cookie_header);

    // Authenticate before any route that exposes or changes a user's orders.
    const auto user_id = authenticated_user(cookie_header);
    if (is_protected_order_route(method, path) && !user_id) {
        return {401, "application/json", R"({"error":"sign in required"})"};
    }

    if (method == "GET" && path == "/api/orders") {
        return open_orders_response(*user_id); // SELECT ... WHERE user_id = ?
    }
    if (method == "POST" && path == "/api/orders") {
        return submit_order(body, *user_id);   // Owner comes from session.
    }
    if (method == "DELETE" && path.starts_with("/api/orders/")) {
        return cancel_order(path, *user_id);   // Reject another user's order.
    }

    // Aggregate book depth can remain public if that is the product decision.
    if (method == "GET" && path == "/api/book") return book_response();
    return static_or_not_found(method, path);
}

Response EngineServer::submit_order(std::string_view body, UserId owner_id) {
    const auto incoming = parse_and_validate_order(body);
    if (!incoming) return bad_request("side, price, and quantity are required");

    // Do not accept an owner/userId field in `body`.
    const OrderId id = allocate_order_id();
    const auto result = book_.submit({id, incoming->side, incoming->price, incoming->quantity});

    // If there is a remainder, persist it with owner_id atomically. Persist
    // execution/cancellation changes too once the first milestone grows into
    // durable matching and trade history.
    if (result.resting) {
        orders_.save_open({{id, incoming->side, incoming->price,
                            result.remaining_quantity}, owner_id,
                           incoming->quantity, OwnedOrder::Status::Open});
    }
    return order_submission_response(id, result);
}

Response EngineServer::open_orders_response(UserId owner_id) const {
    const auto orders = orders_.find_open_for_user(owner_id);
    // Serialize only the caller's own id, side, price, and remaining quantity.
    return {200, "application/json", open_orders_json(orders)};
}

Response EngineServer::cancel_order(std::string_view path, UserId owner_id) {
    const OrderId id = parse_order_id(path);
    // The repository lookup must require both id and owner_id. A mismatch is
    // normally reported as 404 to avoid confirming another user's order exists.
    const auto order = orders_.find_open_for_user_and_id(owner_id, id);
    if (!order) return {404, "application/json", R"({"error":"order not found"})"};

    book_.cancel(id);
    orders_.mark_cancelled(id, owner_id);
    return {200, "application/json", R"({"cancelled":true})"};
}
```

## 4. Browser changes after the server routes exist

```js
// On page load, server state replaces the local-only openOrders map.
const response = await fetch("/api/orders", { credentials: "same-origin" });
if (response.status === 401) showLogin();
else renderOpenOrders(await response.json());

// The browser sends the order economics only. It never supplies userId.
await fetch("/api/orders", {
  method: "POST",
  credentials: "same-origin",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ side: "buy", price: 10050, quantity: 10 }),
});
```

The present `localStorage` cache can stay as a temporary display cache, but
`GET /api/orders` must overwrite it after successful authentication. That makes
the server authoritative across refreshes, devices, and users.

## 5. Implementation order

1. Add migrations and database-backed repositories.
2. Implement password hashing, session creation, secure cookie issuance, and
   registration/login/logout handlers.
3. Add `user_id` to persisted orders and have authenticated `POST /api/orders`
   use it.
4. Implement the user-scoped `GET /api/orders` and ownership-checked delete.
5. Update the browser to use the authenticated endpoints.
6. Add integration tests: unauthenticated access returns 401; Alice cannot list
   or cancel Bob's order; each user sees only their own resting orders.
