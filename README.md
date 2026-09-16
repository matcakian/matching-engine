# C++ Matching Engine

A small, dependency-free limit-order matching engine that implements:

- Limit buy and sell orders
- Price-time priority
- Partial and full fills
- Resting orders and order cancellation
- Trade reporting

## Build and run

```bash
make
make run
make test
```

## Matching rules

An incoming buy matches the lowest-priced resting sells while its limit price is
at least the best ask. An incoming sell matches the highest-priced resting buys
while its limit price is at most the best bid. At each price level, older orders
fill first. Trades execute at the resting order's price.
