#include "matching_engine/order_book.hpp"

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

namespace {

using matching_engine::BookLevel;
using matching_engine::Order;
using matching_engine::OrderBook;
using matching_engine::OrderId;
using matching_engine::Price;
using matching_engine::Quantity;
using matching_engine::Side;
using matching_engine::Trade;

struct Response {
    int status;
    std::string type;
    std::string body;
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

// Extract a quoted field from the small, known JSON payloads accepted by this server.
std::optional<std::string> string_field(std::string_view json, std::string_view name) {
    const std::string key = '"' + std::string(name) + '"';
    const std::size_t key_at = json.find(key);
    if (key_at == std::string_view::npos) return std::nullopt;
    const std::size_t colon = json.find(':', key_at + key.size());
    if (colon == std::string_view::npos) return std::nullopt;
    const std::size_t opening_quote = json.find('"', colon + 1);
    if (opening_quote == std::string_view::npos) return std::nullopt;
    const std::size_t closing_quote = json.find('"', opening_quote + 1);
    if (closing_quote == std::string_view::npos) return std::nullopt;
    return std::string(json.substr(opening_quote + 1, closing_quote - opening_quote - 1));
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
    // Create the book service and seed it with enough depth for an immediately useful UI.
    explicit EngineServer(std::filesystem::path web_root) : web_root_(std::move(web_root)) {
        // A compact initial market makes the page useful as soon as it opens.
        submit_seed({1, Side::Sell, 10'075, 50});
        submit_seed({2, Side::Sell, 10'100, 120});
        submit_seed({3, Side::Sell, 10'150, 80});
        submit_seed({4, Side::Buy, 10'050, 75});
        submit_seed({5, Side::Buy, 10'000, 140});
        submit_seed({6, Side::Buy, 9'950, 60});
    }

    // Dispatch the parsed HTTP request to an API endpoint or a static web asset.
    Response handle(std::string_view method, std::string_view path, std::string_view body) {
        if (method == "GET" && path == "/api/book") return book_response();
        if (method == "POST" && path == "/api/orders") return submit_order(body);
        if (method == "DELETE" && path.starts_with("/api/orders/")) return cancel_order(path);
        if (method == "GET") return static_file(path);
        return {404, "application/json", R"({"error":"not found"})"};
    }

private:
    // Submit initial liquidity and advance generated IDs beyond the seeded order.
    void submit_seed(Order order) {
        book_.submit(order);
        next_order_id_ = std::max(next_order_id_, order.id + 1);
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

    // Validate a browser order payload, submit it to OrderBook, and report its outcome.
    Response submit_order(std::string_view body) {
        const auto side_text = string_field(body, "side");
        const auto price = integer_field<Price>(body, "price");
        const auto quantity = integer_field<Quantity>(body, "quantity");
        if (!side_text || !price || !quantity || (*side_text != "buy" && *side_text != "sell")) {
            return {400, "application/json", R"({"error":"side, price, and quantity are required"})"};
        }

        const auto requested_id = integer_field<OrderId>(body, "id");
        const OrderId id = requested_id.value_or(next_order_id_++);
        if (requested_id && id >= next_order_id_) next_order_id_ = id + 1;

        try {
            const auto result = book_.submit({id, *side_text == "buy" ? Side::Buy : Side::Sell, *price, *quantity});
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
        }
    }

    // Parse an order ID from the REST path and remove that resting order if present.
    Response cancel_order(std::string_view path) {
        const auto id_text = path.substr(std::string_view("/api/orders/").size());
        OrderId id{};
        const auto [pointer, error] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
        if (error != std::errc{} || pointer != id_text.data() + id_text.size()) {
            return {400, "application/json", R"({"error":"invalid order id"})"};
        }
        if (!book_.cancel(id)) return {404, "application/json", R"({"error":"order not found"})"};
        return {200, "application/json", R"({"cancelled":true})"};
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
    OrderBook book_;
    OrderId next_order_id_ = 1;
    std::optional<Trade> last_trade_;
};

// Read one HTTP request, waiting until its declared body has arrived when present.
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

// Build a minimal HTTP/1.1 response and keep writing until the client has all bytes.
void send_response(int socket, const Response& response) {
    const std::string status_text = response.status == 200 ? "OK" : response.status == 201 ? "Created" :
                                    response.status == 400 ? "Bad Request" : "Not Found";
    const std::string header = "HTTP/1.1 " + std::to_string(response.status) + ' ' + status_text + "\r\n"
        "Content-Type: " + response.type + "\r\n"
        "Content-Length: " + std::to_string(response.body.size()) + "\r\n"
        "Connection: close\r\n\r\n";
    const std::string payload = header + response.body;
    std::size_t written = 0;
    while (written < payload.size()) {
        const auto result = send(socket, payload.data() + written, payload.size() - written, 0);
        if (result <= 0) break;
        written += static_cast<std::size_t>(result);
    }
}

} // namespace

// Start a loopback-only server and process one browser request at a time.
int main(int argc, char* argv[]) {
    const int port = argc > 1 ? std::stoi(argv[1]) : 8080;
    const std::filesystem::path web_root = argc > 2 ? argv[2] : "web";
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
    EngineServer server(web_root);
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
                send_response(client, server.handle(method, path, request.substr(header_end + 4)));
            }
        } catch (const std::exception& error) {
            send_response(client, {400, "application/json", "{\"error\":\"" + json_escape(error.what()) + "\"}"});
        }
        close(client);
    }
    close(listener);
}
