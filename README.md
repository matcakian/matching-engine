# C++ Matching Engine

A small, dependency-free limit-order matching engine that implements:

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

The dependency-free browser interface is served by a lightweight local HTTP
bridge that owns an `OrderBook`. It displays aggregated depth, submits limit
orders, reports fills, and lets you cancel resting orders created in the UI.

```bash
cmake -S . -B build
cmake --build build
./build/matching_engine_server
```

Open `http://127.0.0.1:8080`. The server binds only to localhost and starts
with a small illustrative book. It exposes:

- `GET /api/book` for the current depth and latest trade
- `POST /api/orders` with `side`, `price` (integer cents), and `quantity`
- `DELETE /api/orders/{id}` for cancellation
