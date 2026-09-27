#include "matching_engine/user_store.hpp"

#include <cassert>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace matching_engine;

namespace {

std::filesystem::path database_path(std::string_view test_name) {
    return std::filesystem::temp_directory_path() / ("matching-engine-user-store-" + std::string(test_name) + ".db");
}

void remove_database(const std::filesystem::path& database) {
    std::filesystem::remove(database);
    std::filesystem::remove(database.string() + "-shm");
    std::filesystem::remove(database.string() + "-wal");
}

template <typename Action>
void assert_throws(Action action) {
    bool thrown = false;
    try {
        action();
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    assert(thrown);
}

const StoredOrder* find_order(const std::vector<StoredOrder>& orders, OrderId id) {
    for (const auto& order : orders) {
        if (order.order.id == id) return &order;
    }
    return nullptr;
}

void credential_validation_test() {
    const auto database = database_path("credentials");
    remove_database(database);
    {
        UserStore store(database.string());
        const std::string minimum_password = "123456789012";
        const std::string maximum_password(256, 'p');

        // Usernames must be non-empty identifiers that the UI and API can use
        // safely, and passwords have an explicit minimum/maximum length.
        assert(!store.register_user("ab", minimum_password).session);
        assert(!store.register_user("name with space", minimum_password).session);
        assert(!store.register_user("name/with/slash", minimum_password).session);
        assert(!store.register_user(std::string(65, 'a'), minimum_password).session);
        assert(!store.register_user("valid_name", "too-short").session);
        assert(!store.register_user("valid_name", std::string(257, 'p')).session);

        const auto alice = store.register_user("alice-1.2", minimum_password);
        assert(alice.session);
        assert(alice.error == AuthenticationError::None);
        assert(store.authenticate(alice.session->token)->username == "alice-1.2");
        assert(store.register_user("bob_2", maximum_password).session);

        // Registration cannot overwrite an account, and login does not reveal
        // whether a username or password was the invalid credential.
        const auto duplicate = store.register_user("alice-1.2", "AnotherPassword!2026");
        assert(!duplicate.session);
        assert(duplicate.error == AuthenticationError::UsernameTaken);
        assert(!store.login("alice-1.2", "wrong-password").session);
        assert(!store.login("missing-user", minimum_password).session);
    }
    remove_database(database);
}

void session_lifecycle_test() {
    const auto database = database_path("sessions");
    remove_database(database);
    std::string first_token;
    std::string second_token;
    {
        UserStore store(database.string());
        const auto registration = store.register_user("alice", "AlicePassword!2026");
        assert(registration.session);
        first_token = registration.session->token;

        const auto login = store.login("alice", "AlicePassword!2026");
        assert(login.session);
        second_token = login.session->token;
        assert(first_token != second_token);
        assert(store.authenticate(first_token));
        assert(store.authenticate(second_token));

        // Signing out one device/session must not log out another active session.
        store.logout(first_token);
        assert(!store.authenticate(first_token));
        assert(store.authenticate(second_token));
    }
    {
        // Session digests are durable, so a server restart preserves valid
        // sessions until logout or expiry.
        UserStore reloaded(database.string());
        assert(!reloaded.authenticate(first_token));
        assert(reloaded.authenticate(second_token));
        reloaded.logout(second_token);
        assert(!reloaded.authenticate(second_token));
    }
    remove_database(database);
}

void ownership_and_listing_test() {
    const auto database = database_path("ownership");
    remove_database(database);
    {
        UserStore store(database.string());
        assert(!store.has_orders());
        assert(store.next_order_id() == 1);

        // Seed liquidity has no user owner and is visible to the matching book,
        // but never appears in a signed-in user's order list.
        store.add_seed_order({1, Side::Sell, 10'050, 20});
        assert(store.has_orders());
        assert(store.next_order_id() == 2);

        const auto alice = store.register_user("alice", "AlicePassword!2026").session->user;
        const auto bob = store.register_user("bob", "BobPassword!2026").session->user;
        store.record_submission(alice.id, {2, Side::Buy, 10'000, 5}, {{}, 5, true});
        store.record_submission(alice.id, {3, Side::Buy, 9'950, 7}, {{}, 7, true});
        store.record_submission(bob.id, {4, Side::Sell, 10'100, 3}, {{}, 3, true});

        const auto alice_orders = store.open_orders_for_user(alice.id);
        assert(alice_orders.size() == 2);
        assert(alice_orders[0].order.id == 3); // Newest first for the dashboard.
        assert(alice_orders[1].order.id == 2);
        assert(alice_orders[0].owner_id == alice.id);
        assert(store.open_orders_for_user(bob.id).size() == 1);
        assert(store.user_owns_open_order(alice.id, 2));
        assert(!store.user_owns_open_order(bob.id, 2));

        const auto all_open = store.open_orders();
        assert(all_open.size() == 4);
        const auto* seed = find_order(all_open, 1);
        assert(seed && !seed->owner_id);
        assert(store.next_order_id() == 5);
    }
    remove_database(database);
}

void fill_state_test() {
    const auto database = database_path("fills");
    remove_database(database);
    {
        UserStore store(database.string());
        const auto alice = store.register_user("alice", "AlicePassword!2026").session->user;
        const auto bob = store.register_user("bob", "BobPassword!2026").session->user;

        store.add_seed_order({1, Side::Sell, 10'050, 7});
        store.record_submission(alice.id, {2, Side::Sell, 10'050, 5}, {{}, 5, true});

        // Bob sweeps both resting orders. Fully filled orders leave every
        // user's open list, while the incoming filled order remains historical.
        const SubmitResult sweep{{{1, 3, 10'050, 7}, {2, 3, 10'050, 5}}, 0, false};
        store.record_submission(bob.id, {3, Side::Buy, 10'050, 12}, sweep);
        assert(store.open_orders().empty());
        assert(store.open_orders_for_user(alice.id).empty());
        assert(store.open_orders_for_user(bob.id).empty());

        store.add_seed_order({4, Side::Sell, 10'075, 10});
        const SubmitResult partial{{{4, 5, 10'075, 10}}, 2, true};
        store.record_submission(bob.id, {5, Side::Buy, 10'075, 12}, partial);
        const auto bob_orders = store.open_orders_for_user(bob.id);
        assert(bob_orders.size() == 1);
        assert(bob_orders[0].order.id == 5);
        assert(bob_orders[0].order.quantity == 2);
        assert(bob_orders[0].original_quantity == 12);
        assert(store.next_order_id() == 6);
    }
    remove_database(database);
}

void cancellation_and_transaction_test() {
    const auto database = database_path("transactions");
    remove_database(database);
    {
        UserStore store(database.string());
        const auto alice = store.register_user("alice", "AlicePassword!2026").session->user;
        const auto bob = store.register_user("bob", "BobPassword!2026").session->user;
        store.record_submission(alice.id, {1, Side::Buy, 10'000, 5}, {{}, 5, true});

        // The repository independently protects cancellation in case a caller
        // misses the HTTP layer's owner check.
        assert_throws([&] { store.mark_cancelled(bob.id, 1); });
        assert(store.user_owns_open_order(alice.id, 1));
        store.mark_cancelled(alice.id, 1);
        assert(!store.user_owns_open_order(alice.id, 1));
        assert_throws([&] { store.mark_cancelled(alice.id, 1); });

        store.add_seed_order({2, Side::Sell, 10'050, 10});
        // The first fill is valid but the second references an absent order.
        // The whole submission must roll back instead of retaining a partial
        // database mutation that disagrees with the matching book.
        const SubmitResult invalid_fill{{{2, 3, 10'050, 3}, {999, 3, 10'050, 1}}, 1, true};
        assert_throws([&] { store.record_submission(bob.id, {3, Side::Buy, 10'050, 5}, invalid_fill); });
        const auto open = store.open_orders();
        assert(open.size() == 1);
        const auto* seed = find_order(open, 2);
        assert(seed);
        assert(seed->order.quantity == 10); // The valid first fill rolled back.
        assert(store.open_orders_for_user(bob.id).empty());
        assert(store.next_order_id() == 3);
    }
    remove_database(database);
}

void restart_order_persistence_test() {
    const auto database = database_path("restart");
    remove_database(database);
    std::string token;
    UserId alice_id = 0;
    {
        UserStore store(database.string());
        const auto registration = store.register_user("alice", "AlicePassword!2026");
        assert(registration.session);
        token = registration.session->token;
        alice_id = registration.session->user.id;
        store.add_seed_order({1, Side::Sell, 10'100, 8});
        store.record_submission(alice_id, {2, Side::Buy, 10'000, 4}, {{}, 4, true});
    }
    {
        UserStore reloaded(database.string());
        const auto user = reloaded.authenticate(token);
        assert(user && user->id == alice_id);
        const auto alice_orders = reloaded.open_orders_for_user(alice_id);
        assert(alice_orders.size() == 1);
        assert(alice_orders[0].order.id == 2);
        assert(alice_orders[0].order.quantity == 4);
        assert(reloaded.open_orders().size() == 2);
        assert(reloaded.next_order_id() == 3);
    }
    remove_database(database);
}

} // namespace

int main() {
    credential_validation_test();
    session_lifecycle_test();
    ownership_and_listing_test();
    fill_state_test();
    cancellation_and_transaction_test();
    restart_order_persistence_test();
}
