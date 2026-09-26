const currency = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD", minimumFractionDigits: 2 });
const openOrders = new Map();
let pollingTimer;
const formatPrice = (cents) => currency.format(cents / 100);

function renderLevels(containerId, levels) {
  const container = document.querySelector(containerId);
  const template = document.querySelector("#level-template");
  container.replaceChildren();
  if (levels.length === 0) {
    const empty = document.createElement("p");
    empty.className = "empty-levels";
    empty.textContent = "No resting orders";
    container.append(empty);
    return;
  }
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
  document.querySelector("#last-trade").textContent = snapshot.lastTrade
    ? `${formatPrice(snapshot.lastTrade.price)} × ${snapshot.lastTrade.quantity}`
    : "—";
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
  const activity = document.querySelector("#activity");
  activity.prepend(entry);
  while (activity.children.length > 6) activity.lastElementChild.remove();
}

function setConnection(connected) {
  document.querySelector(".connection-status").classList.toggle("is-connected", connected);
  document.querySelector("#connection-label").textContent = connected ? "Engine online" : "Engine unavailable";
}

async function request(path, options) {
  const response = await fetch(path, options);
  const payload = await response.json();
  if (!response.ok) throw new Error(payload.error || "The engine rejected the request.");
  return payload;
}

async function loadBook({ announce = false } = {}) {
  try {
    const snapshot = await request("/api/book");
    renderSnapshot(snapshot);
    setConnection(true);
    document.querySelector("#book-updated").textContent = `Updated ${new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" })}`;
    const message = document.querySelector("#order-message");
    if (message.textContent.startsWith("Connecting")) message.textContent = "Ready to send orders to the local matching engine.";
    if (announce) addActivity("Order book refreshed");
  } catch (error) {
    setConnection(false);
    document.querySelector("#book-updated").textContent = "Waiting for local engine";
  }
}

function renderOpenOrders() {
  const root = document.querySelector("#open-orders");
  const entries = [...openOrders.values()];
  document.querySelector("#open-order-count").textContent = `${entries.length} open`;
  root.replaceChildren();
  if (entries.length === 0) {
    const empty = document.createElement("p");
    empty.className = "empty-state";
    empty.textContent = "Resting orders placed here appear here.";
    root.append(empty);
    return;
  }
  for (const order of entries) {
    const row = document.createElement("div");
    row.className = "open-order";
    const details = document.createElement("div");
    const side = document.createElement("span");
    side.className = `order-side ${order.side}`;
    side.textContent = order.side;
    const description = document.createElement("p");
    description.textContent = `${order.quantity.toLocaleString()} at ${formatPrice(order.price)}`;
    details.append(side, description);
    const cancel = document.createElement("button");
    cancel.type = "button";
    cancel.className = "cancel-order";
    cancel.textContent = "Cancel";
    cancel.addEventListener("click", () => cancelOrder(order.id));
    row.append(details, cancel);
    root.append(row);
  }
}

async function cancelOrder(id) {
  try {
    await request(`/api/orders/${id}`, { method: "DELETE" });
    const order = openOrders.get(id);
    openOrders.delete(id);
    renderOpenOrders();
    addActivity(`Cancelled ${order.side} order #${id}`);
    await loadBook();
  } catch (error) {
    addActivity(`Could not cancel order #${id}: ${error.message}`);
  }
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
  const button = document.querySelector("#submit-order");
  button.disabled = true;
  button.textContent = "Sending order…";
  try {
    const result = await request("/api/orders", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(order),
    });
    if (result.resting) {
      openOrders.set(result.id, { id: result.id, ...order, quantity: result.remainingQuantity });
      renderOpenOrders();
    }
    const fills = result.trades.length;
    message.textContent = fills
      ? `Order #${result.id} matched ${result.trades.reduce((total, trade) => total + trade.quantity, 0)} unit${result.remainingQuantity === 1 ? "" : "s"} across ${fills} fill${fills === 1 ? "" : "s"}.${result.resting ? " The remainder is resting." : ""}`
      : `Order #${result.id} is resting on the book.`;
    addActivity(fills ? `Order #${result.id} created ${fills} fill${fills === 1 ? "" : "s"}` : `Order #${result.id} is now resting`);
    await loadBook();
  } catch (error) {
    message.textContent = error.message;
    setConnection(false);
  } finally {
    button.disabled = false;
    button.textContent = `Place ${form.get("side")} order`;
  }
});

document.querySelector("#side").addEventListener("change", (event) => {
  document.querySelector("#submit-order").textContent = `Place ${event.target.value} order`;
});

loadBook();
pollingTimer = window.setInterval(loadBook, 2_500);
window.addEventListener("beforeunload", () => window.clearInterval(pollingTimer));
