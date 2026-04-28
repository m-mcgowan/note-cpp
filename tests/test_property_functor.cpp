// Tests for per-field functor (Qt-style property accessors).
// Verifies the container_of offset computation is correct and that all
// three access patterns work: functor call, direct assignment, and
// designated initializers.

#include <doctest.h>
#include <string>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_get.hpp>
#include <note/api/note_add.hpp>
#include <note/api/env_set.hpp>

// ---------------------------------------------------------------------------
// Offset correctness: functor operator() must return the parent struct
// ---------------------------------------------------------------------------

TEST_CASE("functor operator() returns parent reference") {
    note::api::HubSet req;
    SUBCASE("mode") {
        auto& ret = req.mode("periodic");
        REQUIRE(&ret == &req);
        REQUIRE(*req.mode == "periodic");
    }
    SUBCASE("outbound") {
        auto& ret = req.outbound(60);
        REQUIRE(&ret == &req);
        REQUIRE(*req.outbound == 60);
    }
    SUBCASE("product") {
        auto& ret = req.product("com.example");
        REQUIRE(&ret == &req);
        REQUIRE(*req.product == "com.example");
    }
    SUBCASE("align (bool)") {
        auto& ret = req.align(true);
        REQUIRE(&ret == &req);
        REQUIRE(*req.align == true);
    }
    SUBCASE("inbound (int32_t)") {
        auto& ret = req.inbound(30);
        REQUIRE(&ret == &req);
        REQUIRE(*req.inbound == 30);
    }
}

TEST_CASE("functor chaining across multiple properties") {
    note::api::HubSet req;
    req.mode("periodic").product("com.example").outbound(60);
    REQUIRE(*req.mode    == "periodic");
    REQUIRE(*req.product == "com.example");
    REQUIRE(*req.outbound == 60);
}

TEST_CASE("functor chaining terminates with execute() on bound request") {
    struct TestHarness {
        note::test::TestJsonBackend backend;
        std::string last_request;
        note::test::CallbackTransport transport;
        note::Notecard nc;
        TestHarness()
            : transport(
                [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                    last_request = std::string(req);
                    return note::string_view("{}");
                },
                [this](note::string_view req) -> note::Result<void> {
                    last_request = std::string(req);
                    return {};
                })
            , nc(note::test::make_test_notecard(backend, transport)) {}
    } h;

    note::Api api(h.nc);
    api.hub.set()
       .mode("periodic")
       .product("com.example.app")
       .outbound(60)
       .execute();
    REQUIRE(h.last_request ==
        R"({"req":"hub.set","mode":"periodic","outbound":60,"product":"com.example.app"})");
}

// ---------------------------------------------------------------------------
// Direct field assignment (Field<T>::operator=)
// ---------------------------------------------------------------------------

TEST_CASE("direct field assignment works") {
    note::api::HubSet req;
    req.mode = "continuous";
    req.outbound = 30;
    REQUIRE(*req.mode    == "continuous");
    REQUIRE(*req.outbound == 30);
}

TEST_CASE("direct field assignment and functor produce same result") {
    note::api::HubSet a, b;
    a.mode = "periodic";
    b.mode("periodic");
    REQUIRE(*a.mode == *b.mode);
}

// ---------------------------------------------------------------------------
// Designated initializers (aggregate, fields are public)
// ---------------------------------------------------------------------------

TEST_CASE("designated initializers work") {
    note::api::HubSet req{.mode = "minimum", .outbound = 120};
    REQUIRE(*req.mode    == "minimum");
    REQUIRE(*req.outbound == 120);
    REQUIRE_FALSE(req.product.has_value());
}

TEST_CASE("designated initializers for polymorphic sub-type") {
    note::api::NoteGet::Get req{.file = "data.qi"};
    REQUIRE(*req.file == "data.qi");
}

// ---------------------------------------------------------------------------
// optional-style access on field
// ---------------------------------------------------------------------------

TEST_CASE("Field<T> optional semantics still work") {
    note::api::HubSet req;
    REQUIRE_FALSE(req.mode.has_value());
    req.mode("periodic");
    REQUIRE(req.mode.has_value());
    REQUIRE(*req.mode == "periodic");
    req.mode = std::nullopt;
    REQUIRE_FALSE(req.mode.has_value());
}

// ---------------------------------------------------------------------------
// extra() method — undocumented properties
// ---------------------------------------------------------------------------

TEST_CASE("extra() adds undocumented bool property to wire format") {
    struct TestHarness {
        note::test::TestJsonBackend backend;
        std::string last_request;
        note::test::CallbackTransport transport;
        note::Notecard nc;
        TestHarness()
            : transport(
                [this](note::string_view req, uint32_t) -> note::Result<note::string_view> {
                    last_request = std::string(req);
                    return note::string_view("{}");
                },
                [](note::string_view) -> note::Result<void> {
                    return {};
                })
            , nc(note::test::make_test_notecard(backend, transport)) {}
    } h;
    note::api::HubSet req;
    req.mode("periodic").extra("exp_feature", true);
    h.nc.execute(req);
    REQUIRE(h.last_request.find("\"exp_feature\":true") != std::string::npos);
    REQUIRE(h.last_request.find("\"mode\":\"periodic\"") != std::string::npos);
}

TEST_CASE("extra() with const char* key") {
    note::api::HubSet req;
    req.extra("beta", "yes");    // const char* value
    REQUIRE(req.extras_count_ == 1);
}

TEST_CASE("extra() silently ignores overflow beyond NOTE_EXTRAS_MAX") {
    note::api::HubSet req;
    for (int i = 0; i < NOTE_EXTRAS_MAX + 5; ++i)
        req.extra("k", i);
    REQUIRE(req.extras_count_ == NOTE_EXTRAS_MAX);
}

// ---------------------------------------------------------------------------
// operator[] — known key routes to typed field
// ---------------------------------------------------------------------------

TEST_CASE("operator[] for known key sets typed field") {
    note::api::HubSet req;
    req["mode"] = note::string_view("periodic");
    REQUIRE(*req.mode == "periodic");
    REQUIRE(req.extras_count_ == 0);  // not stored as extra
}

// ---------------------------------------------------------------------------
// operator[] — unknown key routes to extras
// ---------------------------------------------------------------------------

TEST_CASE("operator[] for unknown key routes to extras") {
    note::api::HubSet req;
    req["undocumented"] = true;
    REQUIRE(req.extras_count_ == 1);
}

TEST_CASE("operator[] reserved word as string key") {
    note::api::HubSet req;
    req["delete"] = note::string_view("yes");    // 'delete' as string — no conflict
    REQUIRE(req.extras_count_ == 1);
}

// ---------------------------------------------------------------------------
// DynField operator=(double) — stores floating-point value in extras
// ---------------------------------------------------------------------------

TEST_CASE("DynField operator=(double) stores double value") {
    note::api::HubSet req;
    req["threshold"] = 3.14;
    REQUIRE(req.extras_count_ == 1);
}

TEST_CASE("DynField operator=(double) value is preserved") {
    note::api::HubSet req;
    req["ratio"] = 1.5;
    REQUIRE(req.extras_count_ == 1);
    // Verify the stored value is a double (not some other variant alternative)
    REQUIRE(std::holds_alternative<double>(req.extras_[0].value));
    REQUIRE(std::get<double>(req.extras_[0].value) == 1.5);
}

TEST_CASE("DynField operator=(const char*) stores string value") {
    note::api::HubSet req;
    req["label"] = "sensor-a";   // const char* literal → operator=(const char*)
    REQUIRE(req.extras_count_ == 1);
    REQUIRE(std::holds_alternative<note::string_view>(req.extras_[0].value));
}

// ---------------------------------------------------------------------------
// operator[] known key — typed field assignment via DynField
// Covers set_typed_field<bool> and set_typed_field<int32_t> instantiations.
// ---------------------------------------------------------------------------

TEST_CASE("operator[] bool known key routes to typed Field<bool>") {
    note::api::HubSet req;
    req["align"] = true;
    REQUIRE(req.align.has_value());
    REQUIRE(*req.align == true);
    REQUIRE(req.extras_count_ == 0);
}

TEST_CASE("operator[] int32_t known key routes to typed Field<int32_t>") {
    note::api::HubSet req;
    req["outbound"] = int32_t{60};
    REQUIRE(req.outbound.has_value());
    REQUIRE(*req.outbound == 60);
    REQUIRE(req.extras_count_ == 0);
}

// operator[] for required (plain) fields — covers set_plain_field<string_view>
TEST_CASE("operator[] string_view required field routes to plain member") {
    note::api::EnvSet req;
    req["name"] = note::string_view("test-var");
    REQUIRE(req.name == "test-var");
    REQUIRE(req.extras_count_ == 0);
}
