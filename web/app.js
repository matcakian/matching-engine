const currency = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD", minimumFractionDigits: 2 });
const openOrders = new Map();
const activities = [];
const OPEN_ORDERS_STORAGE_PREFIX = "matching-engine.open-orders.v2";
const ACTIVITY_STORAGE_PREFIX = "matching-engine.activity.v2";
const MAX_ACTIVITY_ENTRIES = 6;
let pollingTimer;
let currentUser = null;
const formatPrice = (cents) => currency.format(cents / 100);

function readStoredJson(key, fallback) {
  try {
    const stored = window.localStorage.getItem(key);
    return stored === null ? fallback : JSON.parse(stored);
  } catch {
    return fallback;
  }
}

function writeStoredJson(key, value) {
  try {
    window.localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // The dashboard remains usable when browser storage is unavailable.
  }
}

function isOpenOrder(value) {
  return value && Number.isSafeInteger(value.id) && value.id > 0 &&
    (value.side === "buy" || value.side === "sell") &&
    Number.isSafeInteger(value.price) && value.price > 0 &&
    Number.isSafeInteger(value.quantity) && value.quantity > 0;
}

function saveOpenOrders() {
  if (currentUser) writeStoredJson(`${OPEN_ORDERS_STORAGE_PREFIX}.${currentUser.id}`, [...openOrders.values()]);
}

function saveActivities() {
  if (currentUser) writeStoredJson(`${ACTIVITY_STORAGE_PREFIX}.${currentUser.id}`, activities);
}

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

function formatPriceInput(event) {
  const cents = parseCents(event.currentTarget.value);
  if (cents !== undefined && cents > 0) event.currentTarget.value = (cents / 100).toFixed(2);
}

function renderActivities() {
  const activity = document.querySelector("#activity");
  activity.replaceChildren();
  for (const item of activities) {
    const entry = document.createElement("li");
    const content = document.createElement("div");
    const text = document.createElement("strong");
    const time = document.createElement("time");
    text.textContent = item.title;
    time.textContent = new Date(item.createdAt).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
    content.append(text);
    if (item.details) {
      const detail = document.createElement("span");
      detail.className = "activity-detail";
      detail.textContent = item.details;
      content.append(detail);
    }
    entry.append(content, time);
    activity.append(entry);
  }
}

function addActivity(title, details) {
  activities.unshift({ title, details, createdAt: new Date().toISOString() });
  activities.splice(MAX_ACTIVITY_ENTRIES);
  saveActivities();
  renderActivities();
}

function restoreActivities() {
  activities.splice(0);
  if (!currentUser) {
    renderActivities();
    return;
  }
  const storedActivities = readStoredJson(`${ACTIVITY_STORAGE_PREFIX}.${currentUser.id}`, []);
  for (const activity of Array.isArray(storedActivities) ? storedActivities : []) {
    if (activity && typeof activity.title === "string" && typeof activity.details === "string" &&
        Number.isFinite(Date.parse(activity.createdAt))) {
      activities.push(activity);
    }
  }
  activities.splice(MAX_ACTIVITY_ENTRIES);
  renderActivities();
}

function setCurrentUser(user) {
  currentUser = user;
  openOrders.clear();
  document.querySelector("#auth-form").classList.toggle("is-hidden", Boolean(user));
  document.querySelector("#logout").classList.toggle("is-hidden", !user);
  document.querySelector("#account-heading").textContent = user ? `Signed in as ${user.username}` : "Sign in to trade";
  document.querySelector("#account-state").textContent = user ? "Active" : "Guest";
  document.querySelector("#order-message").textContent = user
    ? "Ready to send orders to the matching engine."
    : "Sign in to place orders.";
  document.querySelector("#submit-order").disabled = !user;
  restoreActivities();
  renderOpenOrders();
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
    if (message.textContent.startsWith("Connecting")) {
      message.textContent = currentUser ? "Ready to send orders to the matching engine." : "Sign in to place orders.";
    }
    // Another signed-in user may have filled one of this user's resting orders.
    // Refresh the user-scoped list with the book poll so it stays authoritative.
    if (currentUser) await loadOpenOrders();
    if (announce) addActivity("Order book refreshed", "Current aggregate depth loaded from the engine.");
  } catch (error) {
    setConnection(false);
    document.querySelector("#book-updated").textContent = "Waiting for local engine";
  }
}

async function loadOpenOrders() {
  if (!currentUser) return;
  try {
    const payload = await request("/api/orders");
    openOrders.clear();
    for (const order of payload.orders) {
      if (isOpenOrder(order)) openOrders.set(order.id, order);
    }
    saveOpenOrders();
    renderOpenOrders();
  } catch (error) {
    if (error.message === "sign in required") setCurrentUser(null);
    else addActivity("Could not load open orders", error.message);
  }
}

async function loadSession() {
  try {
    setCurrentUser(await request("/api/session"));
    await loadOpenOrders();
  } catch (error) {
    setCurrentUser(null);
  }
}

function renderOpenOrders() {
  const root = document.querySelector("#open-orders");
  const entries = [...openOrders.values()];
  document.querySelector("#open-order-count").textContent = currentUser ? `${entries.length} open` : "Sign in";
  root.replaceChildren();
  if (!currentUser) {
    const signIn = document.createElement("p");
    signIn.className = "empty-state";
    signIn.textContent = "Sign in to view and manage your open orders.";
    root.append(signIn);
    return;
  }
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

function applyFillsToOpenOrders(trades) {
  for (const trade of trades) {
    const restingOrder = openOrders.get(trade.restingOrderId);
    if (!restingOrder) continue;
    restingOrder.quantity -= trade.quantity;
    if (restingOrder.quantity <= 0) openOrders.delete(trade.restingOrderId);
  }
  saveOpenOrders();
}

async function cancelOrder(id) {
  try {
    await request(`/api/orders/${id}`, { method: "DELETE" });
    const order = openOrders.get(id);
    openOrders.delete(id);
    saveOpenOrders();
    renderOpenOrders();
    addActivity(
      `Cancelled ${order.side.toUpperCase()} order #${id}`,
      `${order.quantity.toLocaleString()} units at ${formatPrice(order.price)} removed from the book.`,
    );
    await loadBook();
  } catch (error) {
    if (error.message === "sign in required") setCurrentUser(null);
    addActivity(`Could not cancel order #${id}`, error.message);
  }
}

document.querySelector("#order-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (!currentUser) {
    document.querySelector("#order-message").textContent = "Sign in to place orders.";
    return;
  }
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
    applyFillsToOpenOrders(result.trades);
    if (result.resting) {
      openOrders.set(result.id, { id: result.id, ...order, quantity: result.remainingQuantity });
    }
    saveOpenOrders();
    renderOpenOrders();
    const fills = result.trades.length;
    const filledQuantity = result.trades.reduce((total, trade) => total + trade.quantity, 0);
    const orderSummary = `${order.side.toUpperCase()} #${result.id} · ${quantity.toLocaleString()} units at ${formatPrice(price)}`;
    message.textContent = fills
      ? `Order #${result.id} matched ${filledQuantity} unit${filledQuantity === 1 ? "" : "s"} across ${fills} fill${fills === 1 ? "" : "s"}.${result.resting ? " The remainder is resting." : ""}`
      : `Order #${result.id} is resting on the book.`;
    if (fills) {
      const restingSide = order.side === "buy" ? "SELL" : "BUY";
      const fillBreakdown = result.trades.map((trade) =>
        `resting ${restingSide} #${trade.restingOrderId}: ${trade.quantity.toLocaleString()} at ${formatPrice(trade.price)}`,
      ).join("; ");
      const remainder = result.resting ? ` · ${result.remainingQuantity.toLocaleString()} units resting` : " · fully filled";
      addActivity(orderSummary, `Filled ${filledQuantity.toLocaleString()} units in ${fills} match${fills === 1 ? "" : "es"}: ${fillBreakdown}${remainder}.`);
    } else {
      addActivity(orderSummary, `${result.remainingQuantity.toLocaleString()} units are resting on the book.`);
    }
    await loadBook();
  } catch (error) {
    message.textContent = error.message;
    if (error.message === "sign in required") setCurrentUser(null);
    setConnection(false);
  } finally {
    button.disabled = !currentUser;
    button.textContent = `Place ${form.get("side")} order`;
  }
});

async function authenticate(path) {
  const form = document.querySelector("#auth-form");
  const credentials = new FormData(form);
  const message = document.querySelector("#auth-message");
  const loginButton = document.querySelector("#login");
  const registerButton = document.querySelector("#register");
  const username = String(credentials.get("username") ?? "").trim();
  const password = String(credentials.get("password") ?? "");
  if (!username || !password) {
    message.textContent = "Enter a username and password.";
    return;
  }
  loginButton.disabled = true;
  registerButton.disabled = true;
  try {
    const user = await request(path, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username, password }),
    });
    form.reset();
    setCurrentUser(user);
    message.textContent = path === "/api/register" ? "Account created. You can now place orders." : "Signed in. You can now place orders.";
    await loadOpenOrders();
  } catch (error) {
    message.textContent = error.message;
  } finally {
    loginButton.disabled = false;
    registerButton.disabled = false;
  }
}

document.querySelector("#auth-form").addEventListener("submit", (event) => {
  event.preventDefault();
  authenticate("/api/login");
});

document.querySelector("#register").addEventListener("click", () => authenticate("/api/register"));

document.querySelector("#logout").addEventListener("click", async () => {
  const message = document.querySelector("#auth-message");
  try {
    await request("/api/logout", { method: "POST" });
    setCurrentUser(null);
    message.textContent = "Signed out.";
  } catch (error) {
    message.textContent = error.message;
  }
});

document.querySelector("#side").addEventListener("change", (event) => {
  document.querySelector("#submit-order").textContent = `Place ${event.target.value} order`;
});
document.querySelector("#price").addEventListener("change", formatPriceInput);

setCurrentUser(null);
loadBook();
loadSession();
pollingTimer = window.setInterval(loadBook, 2_500);
window.addEventListener("beforeunload", () => window.clearInterval(pollingTimer));
