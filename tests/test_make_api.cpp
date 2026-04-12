#include "catch.hpp"
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/api.hpp>

namespace {

note::Notecard make_nc(note::test::TestJsonBackend& backend,
                       note::CallbackTransport& transport) {
    return note::test::make_test_notecard(backend, transport);
}

} // namespace

// ---------------------------------------------------------------------------
// make_api(nc) — unconstrained
// ---------------------------------------------------------------------------

TEST_CASE("make_api(nc) returns unconstrained Api") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport(
        [&last_req](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
    auto nc = make_nc(backend, transport);
    auto api = note::make_api(nc);

    // Unconstrained: all endpoints accessible regardless of SKU
    api.execute(api.card.version());
    REQUIRE(last_req.find("card.version") != std::string::npos);

    api.execute(api.card.sleep());  // WiFi-only, but unconstrained allows it
    REQUIRE(last_req.find("card.sleep") != std::string::npos);

    api.execute(api.hub.set());
    REQUIRE(last_req.find("hub.set") != std::string::npos);
}

// ---------------------------------------------------------------------------
// C++20-only: constrained targets
// ---------------------------------------------------------------------------

#if __cplusplus >= 202002L

TEST_CASE("make_api with constrained target — supported endpoints work") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport(
        [&last_req](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
    auto nc = make_nc(backend, transport);
    auto api = note::make_api(nc, note::target<note::Hardware::WiFi>());

    // card.sleep is WiFi-only — should work on WiFi target
    api.execute(api.card.sleep());
    REQUIRE(last_req.find("card.sleep") != std::string::npos);

    // card.wifi needs WiFi — available
    api.execute(api.card.wifi());
    REQUIRE(last_req.find("card.wifi") != std::string::npos);

    // Universal endpoints always work
    api.execute(api.card.version());
    REQUIRE(last_req.find("card.version") != std::string::npos);
}

TEST_CASE("make_api with Product::Cell target — universal endpoints work") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport(
        [&last_req](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
    auto nc = make_nc(backend, transport);
    auto api = note::make_api(nc, note::target<note::Hardware::Cell>());

    api.execute(api.hub.set());
    REQUIRE(last_req.find("hub.set") != std::string::npos);

    api.execute(api.card.version());
    REQUIRE(last_req.find("card.version") != std::string::npos);
}

TEST_CASE("Strict mode — supported endpoints work at runtime") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport(
        [&last_req](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
    auto nc = make_nc(backend, transport);
    auto api = note::make_api(nc, note::Target<note::Hardware::WiFi, true>{});

    // card.sleep is WiFi-only — available on WiFi strict target
    api.execute(api.card.sleep());
    REQUIRE(last_req.find("card.sleep") != std::string::npos);

    // Universal endpoints work
    api.execute(api.hub.set());
    REQUIRE(last_req.find("hub.set") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Constructor with target (CTAD)
// ---------------------------------------------------------------------------

TEST_CASE("Api(nc, target) — constrained via constructor") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport(
        [&last_req](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
    auto nc = make_nc(backend, transport);
    note::Api api(nc, note::target<note::Hardware::WiFi>());

    // card.wifi needs WiFi — available
    api.execute(api.card.wifi());
    REQUIRE(last_req.find("card.wifi") != std::string::npos);

    // Universal endpoints always work
    api.execute(api.card.version());
    REQUIRE(last_req.find("card.version") != std::string::npos);
}

TEST_CASE("Api(nc, target) — strict mode via constructor") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport(
        [&last_req](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
    auto nc = make_nc(backend, transport);
    note::Api api(nc, note::Target<note::Hardware::WiFi, true>{});

    api.execute(api.card.sleep());
    REQUIRE(last_req.find("card.sleep") != std::string::npos);

    api.execute(api.hub.set());
    REQUIRE(last_req.find("hub.set") != std::string::npos);
}

// Note: strict mode compile-time rejection of unsupported endpoints
// (e.g. api.cardSleep() on a LoRa strict target) cannot be tested via
// static_assert(requires(...)) due to CWG 2908 — no major compiler
// handles constraint failures inside requires-expressions correctly yet.
// Instead, this is verified as a compile-fail test in ci.sh.

// ---------------------------------------------------------------------------
// Firmware gating
// ---------------------------------------------------------------------------

TEST_CASE("Api with MinFirmware — firmware-gated endpoints work when version is sufficient") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport(
        [&last_req](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
    auto nc = make_nc(backend, transport);
    // card.illumination requires firmware 9.1.1
    note::Api api(nc, note::min_firmware<9, 1, 1>());

    api.execute(api.card.illumination());
    REQUIRE(last_req.find("card.illumination") != std::string::npos);

    // Universal endpoints always work
    api.execute(api.card.version());
    REQUIRE(last_req.find("card.version") != std::string::npos);
}

TEST_CASE("Api with combined Hardware + Firmware target") {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::CallbackTransport transport(
        [&last_req](note::string_view r, uint32_t) -> note::Result<note::string_view> {
            last_req = std::string(r);
            return "{}";
        },
        [&last_req](note::string_view r) -> note::Result<void> {
            last_req = std::string(r);
            return {};
        });
    auto nc = make_nc(backend, transport);
    // WiFi hardware + firmware 9.1.1
    note::Api api(nc, note::target<note::Hardware::WiFi, 9, 1, 1>());

    // card.illumination: requires 9.1.1, universal hardware — should work
    api.execute(api.card.illumination());
    REQUIRE(last_req.find("card.illumination") != std::string::npos);

    // card.wifi: WiFi hardware — should work
    api.execute(api.card.wifi());
    REQUIRE(last_req.find("card.wifi") != std::string::npos);
}

#endif // C++20
