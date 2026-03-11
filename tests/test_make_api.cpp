#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/api_context.hpp>

namespace {

note::Notecard make_nc(note::test::TestJsonBackend& backend, std::string& last_req) {
    return note::Notecard(backend,
        [&last_req](note::string_view r, uint32_t) -> note::Result<std::string> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
}

} // namespace

// ---------------------------------------------------------------------------
// make_api(nc) — unconstrained
// ---------------------------------------------------------------------------

TEST_CASE("make_api(nc) returns unconstrained Api") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    auto nc = make_nc(backend, last_req);
    auto api = note::make_api(nc);

    // All methods available
    api.execute(api.cardVersion());
    REQUIRE(last_req.find("card.version") != std::string::npos);

    api.execute(api.cardSleep());
    REQUIRE(last_req.find("card.sleep") != std::string::npos);

    api.execute(api.hubSet());
    REQUIRE(last_req.find("hub.set") != std::string::npos);
}

// ---------------------------------------------------------------------------
// C++20-only: constrained targets
// ---------------------------------------------------------------------------

#if __cplusplus >= 202002L

TEST_CASE("make_api with constrained target — supported endpoints work") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    auto nc = make_nc(backend, last_req);
    auto api = note::make_api(nc, note::target<note::Product::WiFi>());

    // card.sleep is WiFi-only — should work on WiFi target
    api.execute(api.cardSleep());
    REQUIRE(last_req.find("card.sleep") != std::string::npos);

    // card.wifi needs WiFi — available
    api.execute(api.cardWifi());
    REQUIRE(last_req.find("card.wifi") != std::string::npos);

    // Universal endpoints always work
    api.execute(api.cardVersion());
    REQUIRE(last_req.find("card.version") != std::string::npos);
}

TEST_CASE("make_api with Product::Cell target — universal endpoints work") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    auto nc = make_nc(backend, last_req);
    auto api = note::make_api(nc, note::target<note::Product::Cell>());

    api.execute(api.hubSet());
    REQUIRE(last_req.find("hub.set") != std::string::npos);

    api.execute(api.cardVersion());
    REQUIRE(last_req.find("card.version") != std::string::npos);
}

TEST_CASE("Strict mode — supported endpoints work at runtime") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    auto nc = make_nc(backend, last_req);
    auto api = note::make_api(nc, note::Target<note::Rat::WiFi, true>{});

    // card.sleep is WiFi-only — available on WiFi strict target
    api.execute(api.cardSleep());
    REQUIRE(last_req.find("card.sleep") != std::string::npos);

    // Universal endpoints work
    api.execute(api.hubSet());
    REQUIRE(last_req.find("hub.set") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Constructor with target (CTAD)
// ---------------------------------------------------------------------------

TEST_CASE("Api(nc, target) — constrained via constructor") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    auto nc = make_nc(backend, last_req);
    note::Api api(nc, note::target<note::Product::WiFi>());

    // card.wifi needs WiFi — available
    api.execute(api.cardWifi());
    REQUIRE(last_req.find("card.wifi") != std::string::npos);

    // Universal endpoints always work
    api.execute(api.cardVersion());
    REQUIRE(last_req.find("card.version") != std::string::npos);
}

TEST_CASE("Api(nc, target) — strict mode via constructor") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    auto nc = make_nc(backend, last_req);
    note::Api api(nc, note::Target<note::Rat::WiFi, true>{});

    api.execute(api.cardSleep());
    REQUIRE(last_req.find("card.sleep") != std::string::npos);

    api.execute(api.hubSet());
    REQUIRE(last_req.find("hub.set") != std::string::npos);
}

// Note: strict mode compile-time rejection of unsupported endpoints
// (e.g. api.cardSleep() on a LoRa strict target) cannot be tested via
// static_assert(requires(...)) due to CWG 2908 — no major compiler
// handles constraint failures inside requires-expressions correctly yet.
// Instead, this is verified as a compile-fail test in ci.sh.

#endif // C++20
