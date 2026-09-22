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

## Browser UI prototype

`web/` contains a dependency-free browser skeleton for the engine: a market
summary, aggregated bid/ask depth, an order-entry form, and an activity feed.
It uses clearly labeled sample data and creates mock requests only; it does not
yet start the C++ engine or submit live orders.

Preview it locally with:

```bash
python3 -m http.server 8080 -d web
```

Then open `http://localhost:8080` in a browser.

To make it live, add a small HTTP or WebSocket server around `OrderBook`:

- `GET /api/book` returns the result of `bids()`, `asks()`, `best_bid()`, and
  `best_ask()`.
- `POST /api/orders` accepts an ID, side, price (in cents), and quantity, calls
  `submit()`, and returns its trades, remaining quantity, and resting state.
- `DELETE /api/orders/{id}` calls `cancel(id)`.
- A WebSocket or server-sent-event stream publishes book and trade updates, so
  the UI can replace the example snapshot with current engine state.
