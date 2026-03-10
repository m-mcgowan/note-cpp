// Tests for StaticStateStore and NullStateStore — get/set/invalidate/on_change,
// type isolation, observer behaviour.

#include "catch.hpp"

#include <note/app/state_store.hpp>

// Test state types
namespace {

struct Temperature { float celsius; };
struct Humidity { float percent; };
struct Config { int interval; };

} // namespace

// ---------------------------------------------------------------------------
// StaticStateStore — basic get/set
// ---------------------------------------------------------------------------

TEST_CASE("StaticStateStore: get() returns nullopt before set()") {
    note::app::StaticStateStore<Temperature, Humidity> store;
    REQUIRE(!store.get<Temperature>().has_value());
    REQUIRE(!store.get<Humidity>().has_value());
}

TEST_CASE("StaticStateStore: set() then get() returns the value") {
    note::app::StaticStateStore<Temperature, Humidity> store;
    store.set(Temperature{23.5f});
    auto t = store.get<Temperature>();
    REQUIRE(t.has_value());
    REQUIRE(t->celsius == 23.5f);
}

TEST_CASE("StaticStateStore: set() overwrites previous value") {
    note::app::StaticStateStore<Temperature> store;
    store.set(Temperature{20.0f});
    store.set(Temperature{25.0f});
    REQUIRE(store.get<Temperature>()->celsius == 25.0f);
}

// ---------------------------------------------------------------------------
// Type isolation
// ---------------------------------------------------------------------------

TEST_CASE("StaticStateStore: set() does not affect other types") {
    note::app::StaticStateStore<Temperature, Humidity> store;
    store.set(Temperature{23.5f});
    REQUIRE(store.get<Temperature>().has_value());
    REQUIRE(!store.get<Humidity>().has_value());
}

// ---------------------------------------------------------------------------
// invalidate()
// ---------------------------------------------------------------------------

TEST_CASE("StaticStateStore: invalidate() clears the value") {
    note::app::StaticStateStore<Temperature> store;
    store.set(Temperature{23.5f});
    REQUIRE(store.get<Temperature>().has_value());
    store.invalidate<Temperature>();
    REQUIRE(!store.get<Temperature>().has_value());
}

TEST_CASE("StaticStateStore: invalidate() on empty slot is a no-op") {
    note::app::StaticStateStore<Temperature> store;
    store.invalidate<Temperature>();  // should not crash
    REQUIRE(!store.get<Temperature>().has_value());
}

// ---------------------------------------------------------------------------
// on_change() — observer behaviour
// ---------------------------------------------------------------------------

TEST_CASE("StaticStateStore: on_change() fires on set()") {
    note::app::StaticStateStore<Temperature> store;
    Temperature observed{};
    store.on_change<Temperature>([&](const Temperature& t) { observed = t; });
    store.set(Temperature{42.0f});
    REQUIRE(observed.celsius == 42.0f);
}

TEST_CASE("StaticStateStore: on_change() fires on every set()") {
    note::app::StaticStateStore<Temperature> store;
    int count = 0;
    store.on_change<Temperature>([&](const Temperature&) { count++; });
    store.set(Temperature{1.0f});
    store.set(Temperature{2.0f});
    store.set(Temperature{3.0f});
    REQUIRE(count == 3);
}

TEST_CASE("StaticStateStore: on_change() does not fire for other types") {
    note::app::StaticStateStore<Temperature, Humidity> store;
    bool fired = false;
    store.on_change<Temperature>([&](const Temperature&) { fired = true; });
    store.set(Humidity{50.0f});
    REQUIRE(!fired);
}

TEST_CASE("StaticStateStore: on_change() does not fire on invalidate()") {
    note::app::StaticStateStore<Temperature> store;
    store.set(Temperature{23.5f});
    bool fired = false;
    store.on_change<Temperature>([&](const Temperature&) { fired = true; });
    store.invalidate<Temperature>();
    REQUIRE(!fired);
}

TEST_CASE("StaticStateStore: replacing observer stops old, starts new") {
    note::app::StaticStateStore<Temperature> store;
    int count1 = 0, count2 = 0;
    store.on_change<Temperature>([&](const Temperature&) { count1++; });
    store.set(Temperature{1.0f});
    store.on_change<Temperature>([&](const Temperature&) { count2++; });
    store.set(Temperature{2.0f});
    REQUIRE(count1 == 1);
    REQUIRE(count2 == 1);
}

TEST_CASE("StaticStateStore: no observer registered is fine") {
    note::app::StaticStateStore<Temperature> store;
    store.set(Temperature{23.5f});  // should not crash
    REQUIRE(store.get<Temperature>()->celsius == 23.5f);
}

// ---------------------------------------------------------------------------
// NullStateStore
// ---------------------------------------------------------------------------

TEST_CASE("NullStateStore: get() always returns nullopt") {
    note::app::NullStateStore store;
    store.set(Temperature{23.5f});
    REQUIRE(!store.get<Temperature>().has_value());
}

TEST_CASE("NullStateStore: all methods compile and are no-ops") {
    note::app::NullStateStore store;
    store.set(Temperature{1.0f});
    store.set(Humidity{50.0f});
    store.invalidate<Temperature>();
    store.on_change<Temperature>([](const Temperature&) {});
    store.on_change<Humidity>([](const Humidity&) {});
    // Verifying compilation and no crash
    REQUIRE(!store.get<Temperature>().has_value());
    REQUIRE(!store.get<Humidity>().has_value());
}
