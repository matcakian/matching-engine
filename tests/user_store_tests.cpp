#include "matching_engine/user_store.hpp"

#include <cassert>
#include <filesystem>
#include <string>

using namespace matching_engine;

int main() {
    const auto database = std::filesystem::temp_directory_path() / "matching-engine-user-store-tests.db";
    std::filesystem::remove(database);
    std::filesystem::remove(database.string() + "-shm");
    std::filesystem::remove(database.string() + "-wal");

    {
        UserStore store(database.string());

        // Registration validates credentials, returns an opaque session, and
        // persists the user so a later login can authenticate it.
        const auto alice_registration = store.register_user("alice", "AlicePassword!2026");
        assert(alice_registration.session);
        const auto alice = alice_registration.session->user;
        assert(store.authenticate(alice_registration.session->token)->id == alice.id);
        assert(!store.register_user("alice", "AnotherPassword!2026").session);
        assert(!store.register_user("no spaces", "AnotherPassword!2026").session);
        assert(!store.register_user("bob", "too-short").session);
        assert(!store.login("alice", "wrong-password").session);

        const auto bob_login = store.register_user("bob", "BobPassword!2026");
        assert(bob_login.session);
        const auto bob = bob_login.session->user;

        // A user's API query can retrieve only that user's open orders.
        const Order alice_order{1, Side::Buy, 10'000, 5};
        store.record_submission(alice.id, alice_order, {{}, 5, true});
        assert(store.open_orders_for_user(alice.id).size() == 1);
        assert(store.open_orders_for_user(bob.id).empty());
        assert(store.user_owns_open_order(alice.id, alice_order.id));
        assert(!store.user_owns_open_order(bob.id, alice_order.id));

        // Cancelling checks the owner and removes the order from the open list.
        store.mark_cancelled(alice.id, alice_order.id);
        assert(store.open_orders_for_user(alice.id).empty());

        // A fill updates the resting order's durable quantity/status and saves
        // the unfilled portion of the authenticated incoming order.
        store.add_seed_order({2, Side::Sell, 10'050, 7});
        const Order incoming{3, Side::Buy, 10'050, 10};
        const SubmitResult result{{{2, 3, 10'050, 7}}, 3, true};
        store.record_submission(bob.id, incoming, result);
        const auto bob_orders = store.open_orders_for_user(bob.id);
        assert(bob_orders.size() == 1);
        assert(bob_orders[0].order.id == incoming.id);
        assert(bob_orders[0].order.quantity == 3);
        assert(store.next_order_id() == 4);

        store.logout(alice_registration.session->token);
        assert(!store.authenticate(alice_registration.session->token));
    }

    // A new store instance sees the same durable open order after restart.
    {
        UserStore reloaded(database.string());
        const auto orders = reloaded.open_orders();
        assert(orders.size() == 1);
        assert(orders[0].order.id == 3);
        assert(orders[0].order.quantity == 3);
    }

    std::filesystem::remove(database);
    std::filesystem::remove(database.string() + "-shm");
    std::filesystem::remove(database.string() + "-wal");
}
