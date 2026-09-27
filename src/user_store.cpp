#include "matching_engine/user_store.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <sqlite3.h>

#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace matching_engine {
namespace {

constexpr std::size_t kPasswordSaltBytes = 16;
constexpr std::size_t kPasswordHashBytes = 32;
constexpr std::size_t kSessionTokenBytes = 32;
constexpr unsigned kPasswordIterations = 210'000;
constexpr auto kSessionLifetime = std::chrono::hours(24);

class Statement {
public:
    Statement(sqlite3* database, const char* sql) {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* database, const char* sql) {
    char* error = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
    if (sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error("could not bind SQLite text value");
    }
}

void bind_int64(sqlite3_stmt* statement, int index, std::int64_t value) {
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
        throw std::runtime_error("could not bind SQLite integer value");
    }
}

void require_done(sqlite3* database, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(database));
}

std::string hex_encode(const unsigned char* bytes, std::size_t size) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

std::optional<std::vector<unsigned char>> hex_decode(std::string_view value) {
    if (value.size() % 2 != 0) return std::nullopt;
    std::vector<unsigned char> bytes;
    bytes.reserve(value.size() / 2);
    const auto digit = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < value.size(); index += 2) {
        const int high = digit(value[index]);
        const int low = digit(value[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes.push_back(static_cast<unsigned char>((high << 4) | low));
    }
    return bytes;
}

template <std::size_t Size>
std::array<unsigned char, Size> random_bytes() {
    std::array<unsigned char, Size> bytes{};
    arc4random_buf(bytes.data(), bytes.size());
    return bytes;
}

std::string sha256(std::string_view value) {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(value.data(), static_cast<CC_LONG>(value.size()), digest.data());
    return hex_encode(digest.data(), digest.size());
}

bool constant_time_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<unsigned char>(left[index] ^ right[index]);
    }
    return difference == 0;
}

bool valid_username(std::string_view username) {
    if (username.size() < 3 || username.size() > 64) return false;
    for (const unsigned char character : username) {
        if (!(std::isalnum(character) || character == '_' || character == '-' || character == '.')) return false;
    }
    return true;
}

bool valid_password(std::string_view password) {
    return password.size() >= 12 && password.size() <= 256;
}

std::string hash_password(std::string_view password) {
    const auto salt = random_bytes<kPasswordSaltBytes>();
    std::array<unsigned char, kPasswordHashBytes> hash{};
    const int result = CCKeyDerivationPBKDF(kCCPBKDF2,
        password.data(), static_cast<size_t>(password.size()),
        salt.data(), salt.size(), kCCPRFHmacAlgSHA256, kPasswordIterations,
        hash.data(), hash.size());
    if (result != 0) throw std::runtime_error("could not derive password hash");
    return "pbkdf2-sha256$" + std::to_string(kPasswordIterations) + '$' +
        hex_encode(salt.data(), salt.size()) + '$' + hex_encode(hash.data(), hash.size());
}

bool verifies_password(std::string_view password, std::string_view encoded) {
    const std::size_t first = encoded.find('$');
    const std::size_t second = encoded.find('$', first + 1);
    const std::size_t third = encoded.find('$', second + 1);
    if (first == std::string_view::npos || second == std::string_view::npos || third == std::string_view::npos ||
        encoded.substr(0, first) != "pbkdf2-sha256") return false;

    unsigned long iterations = 0;
    try {
        const std::string iteration_text(encoded.substr(first + 1, second - first - 1));
        std::size_t parsed = 0;
        iterations = std::stoul(iteration_text, &parsed);
        if (parsed != iteration_text.size() || iterations == 0 || iterations > std::numeric_limits<unsigned>::max()) return false;
    } catch (const std::exception&) {
        return false;
    }

    const auto salt = hex_decode(encoded.substr(second + 1, third - second - 1));
    const auto expected = hex_decode(encoded.substr(third + 1));
    if (!salt || !expected || expected->size() != kPasswordHashBytes) return false;

    std::array<unsigned char, kPasswordHashBytes> actual{};
    const int result = CCKeyDerivationPBKDF(kCCPBKDF2,
        password.data(), static_cast<size_t>(password.size()),
        salt->data(), salt->size(), kCCPRFHmacAlgSHA256, static_cast<unsigned>(iterations),
        actual.data(), actual.size());
    if (result != 0) return false;
    return constant_time_equal(hex_encode(actual.data(), actual.size()), encoded.substr(third + 1));
}

std::int64_t now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

OrderStatus order_status_from_text(std::string_view value) {
    if (value == "open") return OrderStatus::Open;
    if (value == "filled") return OrderStatus::Filled;
    return OrderStatus::Cancelled;
}

std::string_view order_status_text(OrderStatus status) {
    switch (status) {
    case OrderStatus::Open: return "open";
    case OrderStatus::Filled: return "filled";
    case OrderStatus::Cancelled: return "cancelled";
    }
    throw std::logic_error("unknown order status");
}

std::string_view side_text(Side side) {
    return side == Side::Buy ? "buy" : "sell";
}

Side side_from_text(std::string_view value) {
    return value == "buy" ? Side::Buy : Side::Sell;
}

Session create_session(sqlite3* database, AuthenticatedUser user) {
    const auto token_bytes = random_bytes<kSessionTokenBytes>();
    const std::string token = hex_encode(token_bytes.data(), token_bytes.size());
    const auto expires_at = std::chrono::system_clock::now() + kSessionLifetime;
    const auto expires_unix = std::chrono::duration_cast<std::chrono::seconds>(expires_at.time_since_epoch()).count();

    Statement statement(database, "INSERT INTO sessions(token_digest, user_id, expires_at) VALUES (?, ?, ?)");
    bind_text(statement.get(), 1, sha256(token));
    bind_int64(statement.get(), 2, static_cast<std::int64_t>(user.id));
    bind_int64(statement.get(), 3, expires_unix);
    require_done(database, statement.get());
    return {token, std::move(user), expires_at};
}

} // namespace

UserStore::UserStore(const std::string& database_path) {
    if (sqlite3_open_v2(database_path.c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = database_ ? sqlite3_errmsg(database_) : "could not open database";
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error(message);
    }

    execute(database_, "PRAGMA foreign_keys = ON");
    execute(database_, "PRAGMA journal_mode = WAL");
    execute(database_, R"sql(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS sessions (
            token_digest TEXT PRIMARY KEY,
            user_id INTEGER NOT NULL REFERENCES users(id),
            expires_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS orders (
            id INTEGER PRIMARY KEY,
            user_id INTEGER REFERENCES users(id),
            side TEXT NOT NULL CHECK(side IN ('buy', 'sell')),
            price INTEGER NOT NULL CHECK(price > 0),
            original_quantity INTEGER NOT NULL CHECK(original_quantity > 0),
            remaining_quantity INTEGER NOT NULL CHECK(remaining_quantity >= 0),
            status TEXT NOT NULL CHECK(status IN ('open', 'filled', 'cancelled')),
            created_at INTEGER NOT NULL
        );
        CREATE INDEX IF NOT EXISTS orders_open_by_user ON orders(user_id, status, created_at DESC);
    )sql");
}

UserStore::~UserStore() {
    if (database_) sqlite3_close(database_);
}

AuthenticationResult UserStore::register_user(std::string_view username, std::string_view password) {
    if (!valid_username(username)) return {AuthenticationError::InvalidUsername, std::nullopt};
    if (!valid_password(password)) return {AuthenticationError::InvalidPassword, std::nullopt};

    const std::string password_hash = hash_password(password);
    Statement statement(database_, "INSERT INTO users(username, password_hash, created_at) VALUES (?, ?, ?)");
    bind_text(statement.get(), 1, username);
    bind_text(statement.get(), 2, password_hash);
    bind_int64(statement.get(), 3, now_unix_seconds());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        if (sqlite3_extended_errcode(database_) == SQLITE_CONSTRAINT_UNIQUE) {
            return {AuthenticationError::UsernameTaken, std::nullopt};
        }
        throw std::runtime_error(sqlite3_errmsg(database_));
    }

    const auto id = static_cast<UserId>(sqlite3_last_insert_rowid(database_));
    return {AuthenticationError::None, create_session(database_, {id, std::string(username)})};
}

AuthenticationResult UserStore::login(std::string_view username, std::string_view password) {
    Statement statement(database_, "SELECT id, password_hash FROM users WHERE username = ?");
    bind_text(statement.get(), 1, username);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return {AuthenticationError::InvalidCredentials, std::nullopt};

    const auto id = static_cast<UserId>(sqlite3_column_int64(statement.get(), 0));
    const auto* hash_text = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    const std::string password_hash = hash_text ? hash_text : "";
    if (!verifies_password(password, password_hash)) return {AuthenticationError::InvalidCredentials, std::nullopt};

    return {AuthenticationError::None, create_session(database_, {id, std::string(username)})};
}

std::optional<AuthenticatedUser> UserStore::authenticate(std::string_view token) const {
    if (token.empty()) return std::nullopt;
    Statement statement(database_, R"sql(
        SELECT users.id, users.username
        FROM sessions JOIN users ON users.id = sessions.user_id
        WHERE sessions.token_digest = ? AND sessions.expires_at > ?
    )sql");
    bind_text(statement.get(), 1, sha256(token));
    bind_int64(statement.get(), 2, now_unix_seconds());
    if (sqlite3_step(statement.get()) != SQLITE_ROW) return std::nullopt;
    const auto* username = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    return AuthenticatedUser{static_cast<UserId>(sqlite3_column_int64(statement.get(), 0)), username ? username : ""};
}

void UserStore::logout(std::string_view token) {
    if (token.empty()) return;
    Statement statement(database_, "DELETE FROM sessions WHERE token_digest = ?");
    bind_text(statement.get(), 1, sha256(token));
    require_done(database_, statement.get());
}

OrderId UserStore::next_order_id() const {
    Statement statement(database_, "SELECT COALESCE(MAX(id), 0) + 1 FROM orders");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
    return static_cast<OrderId>(sqlite3_column_int64(statement.get(), 0));
}

bool UserStore::has_orders() const {
    Statement statement(database_, "SELECT EXISTS(SELECT 1 FROM orders)");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
    return sqlite3_column_int(statement.get(), 0) != 0;
}

void UserStore::add_seed_order(const Order& order) {
    Statement statement(database_, R"sql(
        INSERT INTO orders(id, user_id, side, price, original_quantity, remaining_quantity, status, created_at)
        VALUES (?, NULL, ?, ?, ?, ?, 'open', ?)
    )sql");
    bind_int64(statement.get(), 1, static_cast<std::int64_t>(order.id));
    bind_text(statement.get(), 2, side_text(order.side));
    bind_int64(statement.get(), 3, order.price);
    bind_int64(statement.get(), 4, order.quantity);
    bind_int64(statement.get(), 5, order.quantity);
    bind_int64(statement.get(), 6, now_unix_seconds());
    require_done(database_, statement.get());
}

std::vector<StoredOrder> UserStore::open_orders() const {
    Statement statement(database_, R"sql(
        SELECT id, user_id, side, price, original_quantity, remaining_quantity, status
        FROM orders WHERE status = 'open' ORDER BY created_at, id
    )sql");
    std::vector<StoredOrder> orders;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto* side = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 2));
        const auto* status = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 6));
        std::optional<UserId> owner;
        if (sqlite3_column_type(statement.get(), 1) != SQLITE_NULL) {
            owner = static_cast<UserId>(sqlite3_column_int64(statement.get(), 1));
        }
        orders.push_back({
            {static_cast<OrderId>(sqlite3_column_int64(statement.get(), 0)), side_from_text(side ? side : "sell"),
             sqlite3_column_int64(statement.get(), 3), sqlite3_column_int64(statement.get(), 5)},
            owner, sqlite3_column_int64(statement.get(), 4), order_status_from_text(status ? status : "cancelled"),
        });
    }
    if (step != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }
    return orders;
}

std::vector<StoredOrder> UserStore::open_orders_for_user(UserId owner_id) const {
    Statement statement(database_, R"sql(
        SELECT id, side, price, original_quantity, remaining_quantity, status
        FROM orders WHERE user_id = ? AND status = 'open' ORDER BY created_at DESC, id DESC
    )sql");
    bind_int64(statement.get(), 1, static_cast<std::int64_t>(owner_id));
    std::vector<StoredOrder> orders;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const auto* side = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
        const auto* status = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 5));
        orders.push_back({
            {static_cast<OrderId>(sqlite3_column_int64(statement.get(), 0)), side_from_text(side ? side : "sell"),
             sqlite3_column_int64(statement.get(), 2), sqlite3_column_int64(statement.get(), 4)},
            owner_id, sqlite3_column_int64(statement.get(), 3), order_status_from_text(status ? status : "cancelled"),
        });
    }
    if (step != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database_));
    }
    return orders;
}

bool UserStore::user_owns_open_order(UserId owner_id, OrderId order_id) const {
    Statement statement(database_, "SELECT EXISTS(SELECT 1 FROM orders WHERE id = ? AND user_id = ? AND status = 'open')");
    bind_int64(statement.get(), 1, static_cast<std::int64_t>(order_id));
    bind_int64(statement.get(), 2, static_cast<std::int64_t>(owner_id));
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
    return sqlite3_column_int(statement.get(), 0) != 0;
}

void UserStore::record_submission(UserId owner_id, const Order& submitted, const SubmitResult& result) {
    execute(database_, "BEGIN IMMEDIATE");
    try {
        const auto status = result.resting ? OrderStatus::Open : OrderStatus::Filled;
        Statement incoming(database_, R"sql(
            INSERT INTO orders(id, user_id, side, price, original_quantity, remaining_quantity, status, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        )sql");
        bind_int64(incoming.get(), 1, static_cast<std::int64_t>(submitted.id));
        bind_int64(incoming.get(), 2, static_cast<std::int64_t>(owner_id));
        bind_text(incoming.get(), 3, side_text(submitted.side));
        bind_int64(incoming.get(), 4, submitted.price);
        bind_int64(incoming.get(), 5, submitted.quantity);
        bind_int64(incoming.get(), 6, result.remaining_quantity);
        bind_text(incoming.get(), 7, order_status_text(status));
        bind_int64(incoming.get(), 8, now_unix_seconds());
        require_done(database_, incoming.get());

        for (const Trade& trade : result.trades) {
            Statement fill(database_, R"sql(
                UPDATE orders
                SET remaining_quantity = remaining_quantity - ?,
                    status = CASE WHEN remaining_quantity - ? = 0 THEN 'filled' ELSE 'open' END
                WHERE id = ? AND status = 'open' AND remaining_quantity >= ?
            )sql");
            bind_int64(fill.get(), 1, trade.quantity);
            bind_int64(fill.get(), 2, trade.quantity);
            bind_int64(fill.get(), 3, static_cast<std::int64_t>(trade.resting_order_id));
            bind_int64(fill.get(), 4, trade.quantity);
            require_done(database_, fill.get());
            if (sqlite3_changes(database_) != 1) throw std::runtime_error("resting order state is inconsistent");
        }
        execute(database_, "COMMIT");
    } catch (...) {
        try { execute(database_, "ROLLBACK"); } catch (...) { }
        throw;
    }
}

void UserStore::mark_cancelled(UserId owner_id, OrderId order_id) {
    Statement statement(database_, R"sql(
        UPDATE orders SET status = 'cancelled'
        WHERE id = ? AND user_id = ? AND status = 'open'
    )sql");
    bind_int64(statement.get(), 1, static_cast<std::int64_t>(order_id));
    bind_int64(statement.get(), 2, static_cast<std::int64_t>(owner_id));
    require_done(database_, statement.get());
    if (sqlite3_changes(database_) != 1) throw std::runtime_error("order is no longer open");
}

} // namespace matching_engine
