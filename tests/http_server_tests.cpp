// Compile the exact server request handler into this test process. This avoids
// the sandbox's loopback-bind restriction while exercising the production
// endpoint dispatch, JSON parsing, persistence, and restart behavior.
#define MATCHING_ENGINE_HTTP_SERVER_TEST
#include "../src/http_server.cpp"

#include <cassert>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(std::string(message));
}

bool contains(std::string_view value, std::string_view expected) {
    return value.find(expected) != std::string_view::npos;
}

void require_status(const Response& response, int expected, std::string_view context) {
    if (response.status != expected) {
        fail(std::string(context) + ": expected HTTP " + std::to_string(expected) + ", received " +
             std::to_string(response.status) + " with body " + response.body);
    }
}

std::optional<std::string> response_header(const Response& response, std::string_view name) {
    const std::string key = std::string(name) + ':';
    const auto at = response.headers.find(key);
    if (at == std::string::npos) return std::nullopt;
    const auto value_start = response.headers.find_first_not_of(" \t", at + key.size());
    const auto value_end = response.headers.find("\r\n", value_start);
    return response.headers.substr(value_start, value_end - value_start);
}

std::string session_token(const Response& response) {
    const auto cookie = response_header(response, "Set-Cookie");
    require(cookie.has_value(), "authenticated response did not set a cookie");
    constexpr std::string_view prefix = "session=";
    require(cookie->starts_with(prefix), "authenticated response set an unexpected cookie");
    const auto end = cookie->find(';');
    return cookie->substr(prefix.size(), end - prefix.size());
}

class ServerFixture {
public:
    explicit ServerFixture(const std::filesystem::path& database)
        : database_(database) {
        start();
    }

    void restart() {
        server_.reset();
        start();
    }

    [[nodiscard]] Response request(std::string_view method, std::string_view path,
                                   std::string_view body = {}, std::string_view token = {}) const {
        return server_->handle(method, path, body, token.empty() ? "" : "session=" + std::string(token));
    }

private:
    void start() {
        server_ = std::make_unique<EngineServer>("web", database_.string());
    }

    std::filesystem::path database_;
    std::unique_ptr<EngineServer> server_;
};

void assert_book_works(const ServerFixture& server) {
    const auto response = server.request("GET", "/api/book");
    require_status(response, 200, "book health check");
    require(contains(response.body, "\"bids\":"), "book health check returned no bids");
    require(contains(response.body, "\"asks\":"), "book health check returned no asks");
}

void assert_failure_then_book_works(const ServerFixture& server, const Response& response,
                                    int expected_status, std::string_view context) {
    require_status(response, expected_status, context);
    assert_book_works(server);
}

void static_and_routing_test(const ServerFixture& server) {
    // Static assets and the aggregate book are public.
    const auto page = server.request("GET", "/");
    require_status(page, 200, "GET /");
    require(contains(page.body, "Matching Engine"), "index page was not served");
    require_status(server.request("GET", "/app.js"), 200, "GET /app.js");
    require_status(server.request("GET", "/styles.css"), 200, "GET /styles.css");
    assert_book_works(server);

    // Unknown routes, traversal attempts, and unsupported methods must leave
    // the public book readable.
    assert_failure_then_book_works(server, server.request("GET", "/missing"), 404, "unknown URL");
    assert_failure_then_book_works(server, server.request("GET", "/../CMakeLists.txt"), 404, "path traversal");
    assert_failure_then_book_works(server, server.request("PUT", "/api/book"), 404, "wrong book method");
    assert_failure_then_book_works(server, server.request("GET", "/api/register"), 404, "wrong registration method");
    assert_failure_then_book_works(server, server.request("PUT", "/api/login"), 404, "wrong login method");
    assert_failure_then_book_works(server, server.request("GET", "/api/logout"), 404, "wrong logout method");
    assert_failure_then_book_works(server, server.request("POST", "/api/session", "{}"), 404, "wrong session method");
    assert_failure_then_book_works(server, server.request("PATCH", "/api/orders"), 404, "unsupported order method");
    assert_failure_then_book_works(server, server.request("DELETE", "/api/orders"), 404, "wrong collection delete method");

    // Protected routes are safe before login, including requests made in an
    // unexpected order and requests with malformed bodies.
    assert_failure_then_book_works(server, server.request("GET", "/api/session"), 401, "session before login");
    assert_failure_then_book_works(server, server.request("GET", "/api/orders"), 401, "orders before login");
    assert_failure_then_book_works(server, server.request("POST", "/api/orders", "not json"), 401, "order before login");
    assert_failure_then_book_works(server, server.request("DELETE", "/api/orders/7"), 401, "cancel before login");
    require_status(server.request("POST", "/api/logout"), 200, "logout before login");
    require_status(server.request("POST", "/api/logout"), 200, "repeated logout before login");
}

void registration_and_session_test(const ServerFixture& server, std::string& alice_token) {
    // Registration rejects absent, empty, malformed, non-string, form-shaped,
    // and oversized credentials. Every failure is followed by a normal request.
    assert_failure_then_book_works(server, server.request("POST", "/api/register", "{}"), 400, "registration without fields");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", R"({"username":"alice"})"), 400, "registration without password");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", R"({"password":"AlicePassword!2026"})"), 400, "registration without username");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", R"({"username":"","password":"AlicePassword!2026"})"), 400, "registration with empty username");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", R"({"username":"alice","password":""})"), 400, "registration with empty password");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", R"({"username":42,"password":false})"), 400, "registration with non-string fields");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", R"({"username":"alice","password":"broken})"), 400, "malformed registration JSON");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", "username=alice&password=AlicePassword!2026"), 400, "form registration data");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", "{\"username\":\"" + std::string(5'000, 'a') + "\",\"password\":\"AlicePassword!2026\"}"), 400, "long username");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", "{\"username\":\"alice\",\"password\":\"" + std::string(5'000, 'p') + "\"}"), 400, "long password");

    const auto registered = server.request("POST", "/api/register", R"({"username":"alice","password":"AlicePassword!2026"})");
    require_status(registered, 201, "valid registration");
    require(contains(registered.body, R"("username":"alice")"), "registration did not return the user");
    alice_token = session_token(registered);
    require_status(server.request("GET", "/api/session", {}, alice_token), 200, "session after registration");
    assert_failure_then_book_works(server, server.request("POST", "/api/register", R"({"username":"alice","password":"AlicePassword!2026"})"), 409, "duplicate registration");

    // Login accepts only valid credentials and creates an independently usable
    // session. A random token never authenticates a request.
    assert_failure_then_book_works(server, server.request("POST", "/api/login", "{}"), 400, "login without fields");
    assert_failure_then_book_works(server, server.request("POST", "/api/login", R"({"username":"alice","password":"wrong-password"})"), 401, "wrong password");
    assert_failure_then_book_works(server, server.request("POST", "/api/login", R"({"username":"missing","password":"AlicePassword!2026"})"), 401, "unknown user");
    assert_failure_then_book_works(server, server.request("POST", "/api/login", "[]"), 400, "malformed login data");
    assert_failure_then_book_works(server, server.request("GET", "/api/session", {}, "not-a-real-token"), 401, "invalid session token");
    require_status(server.request("GET", "/api/session", {}, alice_token), 200, "existing session after failed logins");

    const auto login = server.request("POST", "/api/login", R"({"username":"alice","password":"AlicePassword!2026"})");
    require_status(login, 200, "valid login");
    const auto second_token = session_token(login);
    require(second_token != alice_token, "login reused the registration token");
    require_status(server.request("GET", "/api/session", {}, second_token), 200, "second active session");
}

void order_endpoint_test(const ServerFixture& server, const std::string& alice_token, std::string& bob_token) {
    // Alice places a non-marketable order and can read it back from her own
    // server-owned order list.
    const auto placed = server.request("POST", "/api/orders", R"({"side":"buy","price":9000,"quantity":10})", alice_token);
    require_status(placed, 201, "valid resting order");
    require(contains(placed.body, R"("resting":true)"), "resting order was not reported as resting");
    require(contains(server.request("GET", "/api/orders", {}, alice_token).body, R"("quantity":10)"), "Alice order was not persisted");

    // Invalid order payloads must not alter Alice's existing order. Note that
    // form data is intentionally invalid because this API accepts JSON only.
    const auto assert_order_failure = [&](std::string_view body, std::string_view description) {
        assert_failure_then_book_works(server, server.request("POST", "/api/orders", body, alice_token), 400, description);
        const auto unchanged = server.request("GET", "/api/orders", {}, alice_token);
        require_status(unchanged, 200, "list after failed order");
        require(contains(unchanged.body, R"("quantity":10)"), "failed order changed Alice state");
    };
    assert_order_failure("{}", "order without fields");
    assert_order_failure(R"({"side":"buy","price":9000})", "order without quantity");
    assert_order_failure(R"({"side":"buy","quantity":1})", "order without price");
    assert_order_failure(R"({"price":9000,"quantity":1})", "order without side");
    assert_order_failure(R"({"side":"","price":9000,"quantity":1})", "order with empty side");
    assert_order_failure(R"({"side":"hold","price":9000,"quantity":1})", "order with invalid side");
    assert_order_failure(R"({"side":"buy","price":0,"quantity":1})", "order with zero price");
    assert_order_failure(R"({"side":"buy","price":-1,"quantity":1})", "order with negative price");
    assert_order_failure(R"({"side":"buy","price":9000,"quantity":0})", "order with zero quantity");
    assert_order_failure(R"({"side":"buy","price":9000,"quantity":-1})", "order with negative quantity");
    assert_order_failure(R"({"side":"buy","price":9000.5,"quantity":1})", "order with fractional price");
    assert_order_failure(R"({"side":"buy","price":9000,"quantity":1.5})", "order with fractional quantity");
    assert_order_failure(R"({"side":"buy","price":999999999999999999999999,"quantity":1})", "order with overflow price");
    assert_order_failure(R"({"side":"buy","price":9000,"quantity":1)" , "malformed order JSON");
    assert_order_failure("side=buy&price=9000&quantity=1", "form order data");
    assert_order_failure("{\"side\":\"hold\",\"price\":9000,\"quantity\":1,\"padding\":\"" + std::string(20'000, 'x') + "\"}", "long invalid order");

    // A sell sweeps eligible seed bids, but skips Alice's own resting buy.
    // The self-trade-prevention remainder rests rather than filling her order.
    const auto matched = server.request("POST", "/api/orders", R"({"side":"sell","price":9000,"quantity":278})", alice_token);
    require_status(matched, 201, "matching sell order");
    require(contains(matched.body, R"("resting":true)"), "self-trade remainder did not rest");
    const auto alice_after_self_trade = server.request("GET", "/api/orders", {}, alice_token);
    require(contains(alice_after_self_trade.body, R"("quantity":10)"), "self-trade reduced Alice's buy order");
    require(contains(alice_after_self_trade.body, R"("quantity":3)"), "self-trade remainder was not persisted");

    // Create Bob, prove he cannot see or cancel Alice's order, then exercise a
    // repeated cancellation of Bob's own order.
    const auto bob = server.request("POST", "/api/register", R"({"username":"bob","password":"BobPassword!2026"})");
    require_status(bob, 201, "register Bob");
    bob_token = session_token(bob);
    const auto bob_list = server.request("GET", "/api/orders", {}, bob_token);
    require_status(bob_list, 200, "list Bob orders");
    require(contains(bob_list.body, R"("orders":[])"), "Bob saw Alice's orders");

    assert_failure_then_book_works(server, server.request("DELETE", "/api/orders/7", {}, bob_token), 404, "Bob cancelling Alice order");
    require(contains(server.request("GET", "/api/orders", {}, alice_token).body, R"("quantity":10)"), "Bob cancellation changed Alice order");
    assert_failure_then_book_works(server, server.request("DELETE", "/api/orders/not-a-number", {}, bob_token), 400, "invalid cancellation ID");
    assert_failure_then_book_works(server, server.request("POST", "/api/orders/7", "{}", bob_token), 404, "wrong order-detail method");

    // A different user can still consume Alice's self-trade-prevention
    // remainder, proving the order remains eligible for other participants.
    require_status(server.request("POST", "/api/orders", R"({"side":"buy","price":9000,"quantity":3})", bob_token), 201, "Bob matching Alice sell");
    require(contains(server.request("GET", "/api/orders", {}, alice_token).body, R"("quantity":10)"), "Bob match changed Alice's buy order");

    const auto bob_order = server.request("POST", "/api/orders", R"({"side":"sell","price":11000,"quantity":4})", bob_token);
    require_status(bob_order, 201, "Bob resting order");
    require(contains(server.request("GET", "/api/orders", {}, bob_token).body, R"("quantity":4)"), "Bob order was not visible to Bob");
    require_status(server.request("DELETE", "/api/orders/10", {}, bob_token), 200, "cancel Bob order");
    assert_failure_then_book_works(server, server.request("DELETE", "/api/orders/10", {}, bob_token), 404, "repeated cancellation");
}

void restart_and_transport_helper_test(ServerFixture& server, const std::string& alice_token, const std::string& bob_token) {
    // The small transport helper ignores absent headers and extracts a normal
    // cookie header. Raw-socket malformed framing remains covered separately
    // when tests run in an environment that permits loopback binding.
    require(request_header("GET / HTTP/1.1\r\nCookie: session=abc\r\n\r\n", "Cookie") == "session=abc", "cookie header parsing");
    require(request_header("malformed", "Cookie").empty(), "malformed request header parsing");

    // Both open orders and sessions are persistent. Last trade is deliberately
    // process-local, so it resets while the order book is rebuilt from SQLite.
    require_status(server.request("POST", "/api/orders", R"({"side":"sell","price":11000,"quantity":4})", bob_token), 201, "Bob persistent order");
    require(contains(server.request("GET", "/api/book").body, "\"lastTrade\":{\"price\":9000"), "expected last trade before restart");
    server.restart();
    assert_book_works(server);
    require_status(server.request("GET", "/api/session", {}, alice_token), 200, "Alice session after restart");
    require_status(server.request("GET", "/api/session", {}, bob_token), 200, "Bob session after restart");
    require(contains(server.request("GET", "/api/orders", {}, alice_token).body, R"("quantity":10)"), "Alice open order missing after restart");
    require(contains(server.request("GET", "/api/orders", {}, bob_token).body, R"("quantity":4)"), "Bob open order missing after restart");
    require(contains(server.request("GET", "/api/book").body, "\"lastTrade\":null"), "temporary last trade survived restart");

    // Logout invalidates the durable session; repeating it remains safe.
    require_status(server.request("POST", "/api/logout", {}, alice_token), 200, "logout after restart");
    assert_failure_then_book_works(server, server.request("GET", "/api/orders", {}, alice_token), 401, "orders after logout");
    require_status(server.request("POST", "/api/logout", {}, alice_token), 200, "repeated logout after restart");
}

} // namespace

int main() {
    const auto database = std::filesystem::temp_directory_path() /
        ("matching-engine-http-tests-" + std::to_string(getpid()) + ".db");
    std::filesystem::remove(database);
    std::filesystem::remove(database.string() + "-shm");
    std::filesystem::remove(database.string() + "-wal");

    try {
        ServerFixture server(database);
        std::string alice_token;
        std::string bob_token;
        static_and_routing_test(server);
        registration_and_session_test(server, alice_token);
        order_endpoint_test(server, alice_token, bob_token);
        restart_and_transport_helper_test(server, alice_token, bob_token);
    } catch (const std::exception& error) {
        std::cerr << "HTTP server test failure: " << error.what() << '\n';
        return 1;
    }

    std::filesystem::remove(database);
    std::filesystem::remove(database.string() + "-shm");
    std::filesystem::remove(database.string() + "-wal");
}
