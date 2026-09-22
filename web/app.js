// This temporary snapshot lets the interface render before a server exposes the
// C++ OrderBook. Prices use the engine's integer-cents representation.
const bookSnapshot = {
  asks: [{ price: 10_075, quantity: 50 }, { price: 10_100, quantity: 120 }, { price: 10_150, quantity: 80 }],
  bids: [{ price: 10_050, quantity: 75 }, { price: 10_000, quantity: 140 }, { price: 9_950, quantity: 60 }],
  lastTrade: { price: 10_050, quantity: 20 },
};

const currency = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD", minimumFractionDigits: 2 });
const formatPrice = (cents) => currency.format(cents / 100);

function renderLevels(containerId, levels) {
  const container = document.querySelector(containerId);
  const template = document.querySelector("#level-template");
  const largestQuantity = Math.max(...levels.map(({ quantity }) => quantity));
  for (const level of levels) {
    const row = template.content.cloneNode(true);
    row.querySelector(".book-level").style.setProperty("--depth", `${(level.quantity / largestQuantity) * 100}%`);
    row.querySelector(".level-price").textContent = formatPrice(level.price);
    row.querySelector(".level-quantity").textContent = level.quantity.toLocaleString();
    container.append(row);
  }
}

function renderSnapshot(snapshot) {
  const bestAsk = snapshot.asks[0]?.price;
  const bestBid = snapshot.bids[0]?.price;
  const spread = bestAsk !== undefined && bestBid !== undefined ? bestAsk - bestBid : undefined;
  document.querySelector("#best-bid").textContent = bestBid === undefined ? "—" : formatPrice(bestBid);
  document.querySelector("#best-ask").textContent = bestAsk === undefined ? "—" : formatPrice(bestAsk);
  document.querySelector("#spread").textContent = spread === undefined ? "—" : formatPrice(spread);
  document.querySelector("#spread-row").textContent = spread === undefined ? "—" : formatPrice(spread);
  document.querySelector("#last-trade").textContent = `${formatPrice(snapshot.lastTrade.price)} × ${snapshot.lastTrade.quantity}`;
  renderLevels("#asks", [...snapshot.asks].reverse());
  renderLevels("#bids", snapshot.bids);
}

function parseCents(priceText) {
  const match = /^(\d+)(?:\.(\d{1,2}))?$/.exec(priceText.trim());
  if (!match) return undefined;
  return Number(match[1]) * 100 + Number((match[2] ?? "").padEnd(2, "0"));
}

function addActivity(message) {
  const entry = document.createElement("li");
  const text = document.createElement("span");
  const time = document.createElement("time");
  text.textContent = message;
  time.textContent = new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  entry.append(text, time);
  document.querySelector("#activity").prepend(entry);
}

async function submitOrder(order) {
  // Replace this mock result with a POST to an API backed by OrderBook::submit.
  // For example: return fetch("/api/orders", { method: "POST", ... });
  return { accepted: true, mode: "mock", order };
}

document.querySelector("#order-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = new FormData(event.currentTarget);
  const price = parseCents(form.get("price"));
  const quantity = Number(form.get("quantity"));
  const message = document.querySelector("#order-message");
  if (!Number.isSafeInteger(price) || price <= 0 || !Number.isSafeInteger(quantity) || quantity <= 0) {
    message.textContent = "Enter a positive price with up to two decimals and a whole-unit quantity.";
    return;
  }
  const order = { side: form.get("side"), price, quantity };
  const result = await submitOrder(order);
  message.textContent = `Mock request created: ${result.order.side.toUpperCase()} ${quantity} at ${formatPrice(price)}. No order was sent.`;
  addActivity(`Mock ${result.order.side} request: ${quantity} at ${formatPrice(price)}`);
});

renderSnapshot(bookSnapshot);
addActivity("Loaded example order-book snapshot");
