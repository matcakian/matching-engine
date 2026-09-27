# C++ Matching Engine

A small limit-order matching engine that implements:

- Limit buy and sell orders
- Price-time priority
- Partial and full fills
- Resting orders and order cancellation
- Trade reporting

## Build and run

```bash
cmake -S . -B build
cmake --build build
./build/matching_engine_demo
ctest --test-dir build --output-on-failure
```

## Matching rules

An incoming buy matches the lowest-priced resting sells while its limit price is
at least the best ask. An incoming sell matches the highest-priced resting buys
while its limit price is at most the best bid. At each price level, older orders
fill first. Trades execute at the resting order's price.

## Browser UI

The browser interface is served by a lightweight local HTTP bridge that owns an
`OrderBook`. It displays aggregated depth, supports account registration and
sign-in, submits authenticated limit orders, and lets each user view and cancel
only their own resting orders. Users, password hashes, sessions, and orders are
stored in SQLite; the in-memory book is rebuilt from open orders on restart.
The core matching library has no external dependencies; the authenticated server
requires SQLite and uses macOS CommonCrypto for password hashing.

```bash
cmake -S . -B build
cmake --build build
./build/matching_engine_server
```

Open `http://127.0.0.1:8080`. The server binds only to localhost and starts
with a small illustrative book. By default it stores data in
`matching_engine.db`; pass a port, web root, and database path to override it:

```bash
./build/matching_engine_server 8080 web /path/to/matching-engine.db
```

It exposes:

- `GET /api/book` for the current depth and latest trade
- `POST /api/register` and `POST /api/login` with `username` and `password`
- `POST /api/logout` and `GET /api/session` for the secure session cookie
- `GET /api/orders` for the signed-in user's open orders
- `POST /api/orders` with `side`, `price` (integer cents), and `quantity`
- `DELETE /api/orders/{id}`, allowed only for that order's owner

Password hashes use PBKDF2-HMAC-SHA-256 with a unique random salt. Session
tokens are random, HttpOnly cookies; SQLite retains only their SHA-256 digests.
The loopback HTTP server intentionally omits the cookie `Secure` flag so the
local browser can use it. Enable HTTPS and add `Secure` before deploying it.
