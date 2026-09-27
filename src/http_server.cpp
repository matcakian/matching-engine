#include "matching_engine/order_book.hpp"
#include "matching_engine/user_store.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {

using matching_engine::BookLevel;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::OrderId;
using matching_engine::Price;
using matching_engine::Quantity;
using matching_engine::Side;
using matching_engine::Trade;
using matching_engine::UserId;
using matching_engine::UserStore;

struct Response {
    int status;
    std::string type;
    std::string body;
    std::string headers;

    Response(int response_status, std::string response_type, std::string response_body, std::string response_headers = {})
        : status(response_status), type(std::move(response_type)), body(std::move(response_body)), headers(std::move(response_headers)) {}
};

// Escape the few characters that would otherwise make a JSON string invalid.
std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += character;
        }
    }
    return escaped;
}

// Return the value of a simple cookie name from a request Cookie header. Session
// tokens are hex encoded, so they cannot contain cookie separators.
std::optional<std::string> cookie_value(std::string_view cookies, std::string_view name) {
    std::size_t start = 0;
    while (start < cookies.size()) {
        const std::size_t end = cookies.find(';', start);
        const auto pair = cookies.substr(start, end == std::string_view::npos ? cookies.size() - start : end - start);
        const std::size_t equals = pair.find('=');
        if (equals != std::string_view::npos) {
            const auto key = pair.substr(0, equals);
            const auto value = pair.substr(equals + 1);
            const std::size_t first = key.find_first_not_of(" \t");
            if (first != std::string_view::npos && key.substr(first) == name) return std::string(value);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

std::string session_cookie(std::string_view token) {
    // Secure is deliberately omitted for the loopback-only HTTP development
    // server. A production HTTPS deployment must add the Secure attribute.
    return "Set-Cookie: session=" + std::string(token) + "; Path=/; HttpOnly; SameSite=Lax; Max-Age=86400\r\n";
}

constexpr std::string_view expired_session_cookie =
    "Set-Cookie: session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0\r\n";

// Serialize an ordered collection of aggregated book levels for an API response.
std::string levels_json(const std::vector<BookLevel>& levels) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < levels.size(); ++index) {
        if (index != 0) output << ',';
        output << R"({"price":)" << levels[index].price
               << R"(,"quantity":)" << levels[index].quantity << '}';
    }
    return output << ']', output.str();
}

// Serialize the fills produced by a submitted order for the client activity feed.
std::string trades_json(const std::vector<Trade>& trades) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < trades.size(); ++index) {
        if (index != 0) output << ',';
        const auto& trade = trades[index];
        output << R"({"restingOrderId":)" << trade.resting_order_id
               << R"(,"incomingOrderId":)" << trade.incoming_order_id
               << R"(,"price":)" << trade.price
               << R"(,"quantity":)" << trade.quantity << '}';
    }
    return output << ']', output.str();
}

void skip_json_whitespace(std::string_view input, std::size_t& position) {
    while (position < input.size() && (input[position] == ' ' || input[position] == '\t' ||
                                      input[position] == '\r' || input[position] == '\n')) {
        ++position;
    }
}

bool parse_json_value(std::string_view input, std::size_t& position);

bool parse_json_string(std::string_view input, std::size_t& position) {
    if (position == input.size() || input[position++] != '"') return false;
    while (position < input.size()) {
        const unsigned char character = input[position++];
        if (character == '"') return true;
        if (character < 0x20) return false;
        if (character != '\\') continue;
        if (position == input.size()) return false;
        const char escape = input[position++];
        if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' || escape == 'f' ||
            escape == 'n' || escape == 'r' || escape == 't') continue;
        if (escape != 'u' || position + 4 > input.size()) return false;
        for (int index = 0; index < 4; ++index) {
            const char hex = input[position++];
            if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') || (hex >= 'A' && hex <= 'F'))) return false;
        }
    }
    return false;
}

bool parse_json_number(std::string_view input, std::size_t& position) {
    const std::size_t start = position;
    if (position < input.size() && input[position] == '-') ++position;
    if (position == input.size()) return false;
    if (input[position] == '0') {
        ++position;
    } else if (input[position] >= '1' && input[position] <= '9') {
        do { ++position; } while (position < input.size() && input[position] >= '0' && input[position] <= '9');
    } else {
        return false;
    }
    if (position < input.size() && input[position] == '.') {
        ++position;
        const std::size_t fractional_start = position;
        while (position < input.size() && input[position] >= '0' && input[position] <= '9') ++position;
        if (position == fractional_start) return false;
    }
    if (position < input.size() && (input[position] == 'e' || input[position] == 'E')) {
        ++position;
        if (position < input.size() && (input[position] == '+' || input[position] == '-')) ++position;
        const std::size_t exponent_start = position;
        while (position < input.size() && input[position] >= '0' && input[position] <= '9') ++position;
        if (position == exponent_start) return false;
    }
    return position > start;
}

bool parse_json_object(std::string_view input, std::size_t& position) {
    if (position == input.size() || input[position++] != '{') return false;
    skip_json_whitespace(input, position);
    if (position < input.size() && input[position] == '}') {
        ++position;
        return true;
    }
    while (true) {
        if (!parse_json_string(input, position)) return false;
        skip_json_whitespace(input, position);
        if (position == input.size() || input[position++] != ':') return false;
        skip_json_whitespace(input, position);
        if (!parse_json_value(input, position)) return false;
        skip_json_whitespace(input, position);
        if (position == input.size()) return false;
        if (input[position] == '}') {
            ++position;
            return true;
        }
        if (input[position++] != ',') return false;
        skip_json_whitespace(input, position);
    }
}

bool parse_json_array(std::string_view input, std::size_t& position) {
    if (position == input.size() || input[position++] != '[') return false;
    skip_json_whitespace(input, position);
    if (position < input.size() && input[position] == ']') {
        ++position;
        return true;
    }
    while (true) {
        if (!parse_json_value(input, position)) return false;
        skip_json_whitespace(input, position);
        if (position == input.size()) return false;
        if (input[position] == ']') {
            ++position;
            return true;
        }
        if (input[position++] != ',') return false;
        skip_json_whitespace(input, position);
    }
}

bool parse_json_value(std::string_view input, std::size_t& position) {
    skip_json_whitespace(input, position);
    if (position == input.size()) return false;
    if (input[position] == '"') return parse_json_string(input, position);
    if (input[position] == '{') return parse_json_object(input, position);
    if (input[position] == '[') return parse_json_array(input, position);
    if (input.substr(position, 4) == "true") { position += 4; return true; }
    if (input.substr(position, 5) == "false") { position += 5; return true; }
    if (input.substr(position, 4) == "null") { position += 4; return true; }
    return parse_json_number(input, position);
}

// Validate the full body before extracting fields. Individual field readers are
// intentionally small, so they should never accept a valid prefix of malformed
// JSON (for example a missing closing brace or comma).
bool is_json_object(std::string_view input) {
    std::size_t position = 0;
    skip_json_whitespace(input, position);
    if (position == input.size() || input[position] != '{' || !parse_json_object(input, position)) return false;
    skip_json_whitespace(input, position);
    return position == input.size();
}

// Extract a JSON string field from the small, known payloads accepted by this
// server. Handling escapes keeps passwords containing quotes or backslashes
// intact when the browser serializes them with JSON.stringify.
std::optional<std::string> string_field(std::string_view json, std::string_view name) {
    const std::string key = '"' + std::string(name) + '"';
    const std::size_t key_at = json.find(key);
    if (key_at == std::string_view::npos) return std::nullopt;
    const std::size_t colon = json.find(':', key_at + key.size());
    if (colon == std::string_view::npos) return std::nullopt;
    const std::size_t opening_quote = json.find('"', colon + 1);
    if (opening_quote == std::string_view::npos) return std::nullopt;
    std::string value;
    for (std::size_t index = opening_quote + 1; index < json.size(); ++index) {
        const char character = json[index];
        if (character == '"') return value;
        if (character != '\\') {
            value += character;
            continue;
        }
        if (++index == json.size()) return std::nullopt;
        switch (json[index]) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        // Browser-submitted credentials are UTF-8, so JSON.stringify does not
        // need this escape form. Rejecting it is safer than silently changing a
        // password's bytes in this deliberately small parser.
        case 'u': return std::nullopt;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

template <typename Integer>
// Extract an integer field while rejecting empty, fractional, and overflowing values.
std::optional<Integer> integer_field(std::string_view json, std::string_view name) {
    const std::string key = '"' + std::string(name) + '"';
    const std::size_t key_at = json.find(key);
    if (key_at == std::string_view::npos) return std::nullopt;
    const std::size_t colon = json.find(':', key_at + key.size());
    if (colon == std::string_view::npos) return std::nullopt;
    const std::size_t first = json.find_first_not_of(" \t\r\n", colon + 1);
    if (first == std::string_view::npos) return std::nullopt;
    const std::size_t last = json.find_first_not_of("-0123456789", first);
    // A field stored in integer cents/units cannot accept a decimal or
    // exponent suffix by silently parsing only its whole-number prefix.
    if (last != std::string_view::npos && (json[last] == '.' || json[last] == 'e' || json[last] == 'E')) {
        return std::nullopt;
    }
    const auto token = json.substr(first, last - first);
    Integer result{};
    const auto [pointer, error] = std::from_chars(token.data(), token.data() + token.size(), result);
    if (error != std::errc{} || pointer != token.data() + token.size()) return std::nullopt;
    return result;
}

// Choose the response MIME type needed for each static browser asset.
std::string content_type(const std::filesystem::path& path) {
    const auto extension = path.extension();
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".js") return "text/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}

// Read a static asset into memory so it can be written as the HTTP response body.
std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class EngineServer {
public:
    // Rebuild the in-memory book from durable resting orders. The initial
    // illustrative liquidity is inserted only on a brand-new database.
    EngineServer(std::filesystem::path web_root, const std::string& database_path)
        : web_root_(std::move(web_root)), users_(database_path) {
        if (!users_.has_orders()) {
            submit_seed({1, Side::Sell, 10'075, 50});
            submit_seed({2, Side::Sell, 10'100, 120});
            submit_seed({3, Side::Sell, 10'150, 80});
            submit_seed({4, Side::Buy, 10'050, 75});
            submit_seed({5, Side::Buy, 10'000, 140});
            submit_seed({6, Side::Buy, 9'950, 60});
        }
        for (const auto& stored : users_.open_orders()) {
            book_.submit(stored.order);
        }
        next_order_id_ = users_.next_order_id();
    }

    // Dispatch the parsed HTTP request to an API endpoint or a static web asset.
    Response handle(std::string_view method, std::string_view path, std::string_view body, std::string_view cookies) {
        if (method == "POST" && path == "/api/register") return register_user(body);
        if (method == "POST" && path == "/api/login") return login(body);
        if (method == "POST" && path == "/api/logout") return logout(cookies);
        if (method == "GET" && path == "/api/session") return session_response(cookies);
        if (method == "GET" && path == "/api/book") return book_response();

        const bool protected_order_route = (method == "GET" && path == "/api/orders") ||
            (method == "POST" && path == "/api/orders") ||
            (method == "DELETE" && path.starts_with("/api/orders/"));
        if (protected_order_route) {
            const auto user = authenticated_user(cookies);
            if (!user) return {401, "application/json", R"({"error":"sign in required"})"};
            if (method == "GET") return open_orders_response(user->id);
            if (method == "POST") return submit_order(body, user->id);
            return cancel_order(path, user->id);
        }
        if (method == "GET") return static_file(path);
        return {404, "application/json", R"({"error":"not found"})"};
    }

private:
    // Persist initial liquidity; the store assigns the next order ID after all
    // seed rows are present.
    void submit_seed(Order order) {
        users_.add_seed_order(order);
    }

    // Return the current aggregate depth plus the most recently generated fill.
    Response book_response() const {
        std::ostringstream output;
        output << R"({"bids":)" << levels_json(book_.bids())
               << R"(,"asks":)" << levels_json(book_.asks())
               << R"(,"lastTrade":)";
        if (last_trade_) {
            output << R"({"price":)" << last_trade_->price
                   << R"(,"quantity":)" << last_trade_->quantity << '}';
        } else {
            output << "null";
        }
        output << '}';
        return {200, "application/json", output.str()};
    }

    // Validate a browser order payload, attach its authenticated owner, and
    // persist its resulting resting/fill state alongside the in-memory book.
    Response submit_order(std::string_view body, UserId owner_id) {
        if (!is_json_object(body)) {
            return {400, "application/json", R"({"error":"request body must be a JSON object"})"};
        }
        const auto side_text = string_field(body, "side");
        const auto price = integer_field<Price>(body, "price");
        const auto quantity = integer_field<Quantity>(body, "quantity");
        if (!side_text || !price || !quantity || *price <= 0 || *quantity <= 0 ||
            (*side_text != "buy" && *side_text != "sell")) {
            return {400, "application/json", R"({"error":"side, price, and quantity are required"})"};
        }

        // IDs are server-assigned so a client cannot collide with or probe
        // another user's historical orders.
        const OrderId id = next_order_id_++;

        try {
            const Order order{id, *side_text == "buy" ? Side::Buy : Side::Sell, *price, *quantity};
            const auto result = book_.submit(order);
            users_.record_submission(owner_id, order, result);
            if (!result.trades.empty()) last_trade_ = result.trades.back();
            std::ostringstream output;
            output << R"({"id":)" << id
                   << R"(,"trades":)" << trades_json(result.trades)
                   << R"(,"remainingQuantity":)" << result.remaining_quantity
                   << R"(,"resting":)" << (result.resting ? "true" : "false")
                   << '}';
            return {201, "application/json", output.str()};
        } catch (const std::invalid_argument& error) {
            return {400, "application/json", "{\"error\":\"" + json_escape(error.what()) + "\"}"};
        } catch (const std::exception&) {
            return {500, "application/json", R"({"error":"could not persist order"})"};
        }
    }

    // Parse an order ID and cancel it only when the signed-in user owns it.
    Response cancel_order(std::string_view path, UserId owner_id) {
        const auto id_text = path.substr(std::string_view("/api/orders/").size());
        OrderId id{};
        const auto [pointer, error] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
        if (error != std::errc{} || pointer != id_text.data() + id_text.size()) {
            return {400, "application/json", R"({"error":"invalid order id"})"};
        }
        if (!users_.user_owns_open_order(owner_id, id)) {
            return {404, "application/json", R"({"error":"order not found"})"};
        }
        if (!book_.cancel(id)) return {404, "application/json", R"({"error":"order not found"})"};
        users_.mark_cancelled(owner_id, id);
        return {200, "application/json", R"({"cancelled":true})"};
    }

    Response register_user(std::string_view body) {
        if (!is_json_object(body)) {
            return {400, "application/json", R"({"error":"request body must be a JSON object"})"};
        }
        const auto username = string_field(body, "username");
        const auto password = string_field(body, "password");
        if (!username || !password) return {400, "application/json", R"({"error":"username and password are required"})"};
        const auto result = users_.register_user(*username, *password);
        if (!result.session) {
            switch (result.error) {
            case matching_engine::AuthenticationError::InvalidUsername:
                return {400, "application/json", R"({"error":"username must be 3-64 letters, numbers, dots, dashes, or underscores"})"};
            case matching_engine::AuthenticationError::InvalidPassword:
                return {400, "application/json", R"({"error":"password must be 12-256 characters"})"};
            case matching_engine::AuthenticationError::UsernameTaken:
                return {409, "application/json", R"({"error":"username is already taken"})"};
            default:
                return {400, "application/json", R"({"error":"could not create account"})"};
            }
        }
        return authenticated_response(*result.session, 201);
    }

    Response login(std::string_view body) {
        if (!is_json_object(body)) {
            return {400, "application/json", R"({"error":"request body must be a JSON object"})"};
        }
        const auto username = string_field(body, "username");
        const auto password = string_field(body, "password");
        if (!username || !password) return {400, "application/json", R"({"error":"username and password are required"})"};
        const auto result = users_.login(*username, *password);
        if (!result.session) return {401, "application/json", R"({"error":"invalid username or password"})"};
        return authenticated_response(*result.session, 200);
    }

    Response logout(std::string_view cookies) {
        if (const auto token = cookie_value(cookies, "session")) users_.logout(*token);
        return {200, "application/json", R"({"loggedOut":true})", std::string(expired_session_cookie)};
    }

    std::optional<matching_engine::AuthenticatedUser> authenticated_user(std::string_view cookies) const {
        const auto token = cookie_value(cookies, "session");
        return token ? users_.authenticate(*token) : std::nullopt;
    }

    Response session_response(std::string_view cookies) const {
        const auto user = authenticated_user(cookies);
        if (!user) return {401, "application/json", R"({"error":"sign in required"})"};
        return {200, "application/json", "{\"id\":" + std::to_string(user->id) +
            ",\"username\":\"" + json_escape(user->username) + "\"}"};
    }

    Response authenticated_response(const matching_engine::Session& session, int status) const {
        return {status, "application/json", "{\"id\":" + std::to_string(session.user.id) +
            ",\"username\":\"" + json_escape(session.user.username) + "\"}", session_cookie(session.token)};
    }

    Response open_orders_response(UserId owner_id) const {
        const auto orders = users_.open_orders_for_user(owner_id);
        std::ostringstream output;
        output << R"({"orders":[)";
        for (std::size_t index = 0; index < orders.size(); ++index) {
            if (index != 0) output << ',';
            const auto& order = orders[index].order;
            output << R"({"id":)" << order.id
                   << R"(,"side":")" << (order.side == Side::Buy ? "buy" : "sell")
                   << R"(","price":)" << order.price
                   << R"(,"quantity":)" << order.quantity << '}';
        }
        output << "]}";
        return {200, "application/json", output.str()};
    }

    // Safely resolve and serve a browser asset while rejecting traversal attempts.
    Response static_file(std::string_view requested_path) const {
        const std::string file_name = requested_path == "/" ? "index.html" : std::string(requested_path.substr(1));
        if (file_name.empty() || file_name.find("..") != std::string::npos) {
            return {404, "text/plain; charset=utf-8", "Not found"};
        }
        const auto file = web_root_ / file_name;
        if (!std::filesystem::is_regular_file(file)) return {404, "text/plain; charset=utf-8", "Not found"};
        return {200, content_type(file), read_file(file)};
    }

    std::filesystem::path web_root_;
    UserStore users_;
    OrderBook book_;
    OrderId next_order_id_ = 1;
    std::optional<Trade> last_trade_;
};

// Read one HTTP request, waiting until its declared body has arrived when present.
#ifndef MATCHING_ENGINE_HTTP_SERVER_TEST
std::string receive_request(int socket) {
    std::string request;
    char buffer[4096];
    std::size_t expected_size = 0;
    while (true) {
        const auto received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        request.append(buffer, static_cast<std::size_t>(received));
        const auto header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos && expected_size == 0) {
            constexpr std::string_view content_length = "Content-Length:";
            const auto header_at = request.find(content_length);
            std::size_t content_size = 0;
            if (header_at != std::string::npos) {
                const auto value_start = header_at + content_length.size();
                const auto value_end = request.find("\r\n", value_start);
                const auto value = request.substr(value_start, value_end - value_start);
                content_size = static_cast<std::size_t>(std::stoull(value));
            }
            expected_size = header_end + 4 + content_size;
        }
        if (expected_size != 0 && request.size() >= expected_size) break;
    }
    return request;
}
#endif

// Extract one HTTP header after a request has been fully received. The browser
// sends canonical header casing for Cookie; supporting that exact form keeps the
// small local HTTP bridge intentionally narrow.
std::string request_header(std::string_view request, std::string_view name) {
    const std::size_t header_end = request.find("\r\n\r\n");
    const std::size_t first_line_end = request.find("\r\n");
    if (header_end == std::string_view::npos || first_line_end == std::string_view::npos) return {};
    std::size_t line_start = first_line_end + 2;
    while (line_start < header_end) {
        const std::size_t line_end = request.find("\r\n", line_start);
        const auto line = request.substr(line_start, line_end - line_start);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos && line.substr(0, colon) == name) {
            const std::size_t value_start = line.find_first_not_of(" \t", colon + 1);
            return value_start == std::string_view::npos ? "" : std::string(line.substr(value_start));
        }
        if (line_end == std::string_view::npos || line_end >= header_end) break;
        line_start = line_end + 2;
    }
    return {};
}

// Build a minimal HTTP/1.1 response and keep writing until the client has all bytes.
#ifndef MATCHING_ENGINE_HTTP_SERVER_TEST
void send_response(int socket, const Response& response) {
    const std::string status_text = response.status == 200 ? "OK" : response.status == 201 ? "Created" :
                                    response.status == 400 ? "Bad Request" : response.status == 401 ? "Unauthorized" :
                                    response.status == 409 ? "Conflict" : response.status == 500 ? "Internal Server Error" :
                                    "Not Found";
    const std::string header = "HTTP/1.1 " + std::to_string(response.status) + ' ' + status_text + "\r\n"
        "Content-Type: " + response.type + "\r\n"
        + response.headers
        + "Content-Length: " + std::to_string(response.body.size()) + "\r\n"
        "Connection: close\r\n\r\n";
    const std::string payload = header + response.body;
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto result = send(socket, payload.data() + written, payload.size() - written, 0);
        if (result <= 0) break;
        written += static_cast<std::size_t>(result);
    }
}
#endif

} // namespace

// Start a loopback-only server and process one browser request at a time.
#ifndef MATCHING_ENGINE_HTTP_SERVER_TEST
int main(int argc, char* argv[]) {
    const int port = argc > 1 ? std::stoi(argv[1]) : 8080;
    const std::filesystem::path web_root = argc > 2 ? argv[2] : "web";
    const std::string database_path = argc > 3 ? argv[3] : "matching_engine.db";
    if (!std::filesystem::is_directory(web_root)) {
        std::cerr << "Web root does not exist: " << web_root << '\n';
        return 1;
    }

    std::signal(SIGPIPE, SIG_IGN);
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error("could not create server socket");
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(listener, 16) < 0) {
        close(listener);
        throw std::runtime_error("could not listen on localhost:" + std::to_string(port));
    }

    std::cout << "Matching engine UI available at http://127.0.0.1:" << port << '\n';
    EngineServer server(web_root, database_path);
    while (true) {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        try {
            const std::string request = receive_request(client);
            const auto first_line_end = request.find("\r\n");
            const auto header_end = request.find("\r\n\r\n");
            if (first_line_end == std::string::npos || header_end == std::string::npos) {
                send_response(client, {400, "application/json", R"({"error":"malformed request"})"});
            } else {
                std::istringstream line(request.substr(0, first_line_end));
                std::string method, path, version;
                line >> method >> path >> version;
                send_response(client, server.handle(method, path, request.substr(header_end + 4), request_header(request, "Cookie")));
            }
        } catch (const std::exception& error) {
            send_response(client, {400, "application/json", "{\"error\":\"" + json_escape(error.what()) + "\"}"});
        }
        close(client);
    }
    close(listener);
}
#endif
