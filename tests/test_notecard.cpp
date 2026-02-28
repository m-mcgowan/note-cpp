// Tests for Notecard: request(), command(), command_typed(), set_default_timeout(),
// backend(), execute() error paths, and send_fn derivation.

#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/notecard.hpp>
#include <note/safety.hpp>
#include <note/api/card_version.hpp>
#include <note/api/card_restart.hpp>
#include <note/api/hub_set.hpp>

// ---------------------------------------------------------------------------
// A JsonReader that always reports an error — for protocol error path tests.
// ---------------------------------------------------------------------------
namespace {

class ErrorJsonReader : public note::JsonReader {
public:
    explicit ErrorJsonReader(std::string msg) : msg_(std::move(msg)) {}
    bool has(note::string_view) const override { return false; }
    bool get_bool(note::string_view, bool d) const override { return d; }
    int32_t get_int(note::string_view, int32_t d) const override { return d; }
    double get_double(note::string_view, double d) const override { return d; }
    note::string_view get_string(note::string_view, note::string_view d) const override { return d; }
    std::unique_ptr<note::JsonReader> get_object(note::string_view) const override { return nullptr; }
    bool has_error() const override { return true; }
    note::string_view get_error() const override { return note::string_view(msg_); }
private:
    std::string msg_;
};

class ErrorJsonBackend : public note::JsonBackend {
public:
    explicit ErrorJsonBackend(std::string error_msg) : error_msg_(std::move(error_msg)) {}
    std::unique_ptr<note::JsonBuilder> create_builder() override {
        return std::make_unique<note::test::TestJsonBuilder>();
    }
    std::unique_ptr<note::JsonReader> parse_response(note::string_view) override {
        return std::make_unique<ErrorJsonReader>(error_msg_);
    }
private:
    std::string error_msg_;
};

} // namespace

// ---------------------------------------------------------------------------
// backend() accessor
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::backend() returns the JsonBackend reference") {
    note::test::TestJsonBackend backend;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> { return "{}"; });
    REQUIRE(&nc.backend() == &backend);
}

// ---------------------------------------------------------------------------
// Default timeout and set_default_timeout()
// ---------------------------------------------------------------------------

TEST_CASE("Notecard default timeout is 10000 ms") {
    note::test::TestJsonBackend backend;
    uint32_t captured_timeout = 0;
    note::Notecard nc(backend,
        [&](note::string_view, uint32_t t) -> note::Result<std::string> {
            captured_timeout = t;
            return "{}";
        });
    note::api::CardVersion req;
    nc.execute(req);
    REQUIRE(captured_timeout == 10000);
}

TEST_CASE("Notecard::set_default_timeout() changes timeout passed to request_fn") {
    note::test::TestJsonBackend backend;
    uint32_t captured_timeout = 0;
    note::Notecard nc(backend,
        [&](note::string_view, uint32_t t) -> note::Result<std::string> {
            captured_timeout = t;
            return "{}";
        });
    nc.set_default_timeout(5000);
    note::api::CardVersion req;
    nc.execute(req);
    REQUIRE(captured_timeout == 5000);
}

// ---------------------------------------------------------------------------
// send_fn derivation from request_fn
// ---------------------------------------------------------------------------

TEST_CASE("Notecard derives send_fn from request_fn when not provided") {
    note::test::TestJsonBackend backend;
    bool request_fn_called = false;
    note::Notecard nc(backend,
        [&](note::string_view, uint32_t) -> note::Result<std::string> {
            request_fn_called = true;
            return "{}";
        });
    // command() uses send_fn; when not provided, it calls request_fn
    auto r = nc.command("card.restart");
    REQUIRE(r.has_value());
    REQUIRE(request_fn_called);
}

TEST_CASE("Notecard derived send_fn propagates transport error from request_fn") {
    note::test::TestJsonBackend backend;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> {
            return note::Unexpected(note::ErrorInfo{note::Error::Transport, "wire error"});
        });
    auto r = nc.command("card.restart");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Transport);
}

// ---------------------------------------------------------------------------
// request() — ad-hoc requests
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::request() sends req type with no extra fields") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::Notecard nc(backend,
        [&](note::string_view req, uint32_t) -> note::Result<std::string> {
            captured = std::string(req);
            return "{}";
        });
    auto r = nc.request("card.version");
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"req":"card.version"})");
}

TEST_CASE("Notecard::request() with build_fn adds fields to the request") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::Notecard nc(backend,
        [&](note::string_view req, uint32_t) -> note::Result<std::string> {
            captured = std::string(req);
            return "{}";
        });
    auto r = nc.request("hub.set", [](note::JsonBuilder& b) {
        b.add("mode", note::string_view("periodic"));
    });
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"req":"hub.set","mode":"periodic"})");
}

TEST_CASE("Notecard::request() returns a non-null JsonReader on success") {
    note::test::TestJsonBackend backend;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> { return "{}"; });
    auto r = nc.request("card.version");
    REQUIRE(r.has_value());
    REQUIRE(r.value() != nullptr);
}

TEST_CASE("Notecard::request() propagates transport error") {
    note::test::TestJsonBackend backend;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> {
            return note::Unexpected(note::ErrorInfo{note::Error::Transport, "lost"});
        });
    auto r = nc.request("card.version");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Transport);
}

TEST_CASE("Notecard::request() returns protocol error when response has err field") {
    ErrorJsonBackend backend("notecard not ready");
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> {
            return R"({"err":"notecard not ready"})";
        });
    auto r = nc.request("card.version");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Protocol);
    // Note: error message is a string_view into the (now-destroyed) reader;
    // checking the code is sufficient for the protocol error path.
}

// ---------------------------------------------------------------------------
// execute() error paths (transport + protocol)
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::execute() propagates transport error") {
    note::test::TestJsonBackend backend;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> {
            return note::Unexpected(note::ErrorInfo{note::Error::Transport, "io error"});
        });
    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Transport);
}

TEST_CASE("Notecard::execute() returns protocol error when response has err field") {
    ErrorJsonBackend backend("bad firmware");
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> {
            return R"({"err":"bad firmware"})";
        });
    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Protocol);
    // Note: error message is a string_view into the (now-destroyed) reader;
    // checking the code is sufficient for the protocol error path.
}

// ---------------------------------------------------------------------------
// command() — fire-and-forget
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::command() sends cmd type with no extra fields") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> { return "{}"; },
        [&](note::string_view req) -> note::Result<void> {
            captured = std::string(req);
            return {};
        });
    auto r = nc.command("card.restart");
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"cmd":"card.restart"})");
}

TEST_CASE("Notecard::command() with build_fn adds fields") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> { return "{}"; },
        [&](note::string_view req) -> note::Result<void> {
            captured = std::string(req);
            return {};
        });
    auto r = nc.command("hub.set", [](note::JsonBuilder& b) {
        b.add("mode", note::string_view("periodic"));
    });
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"cmd":"hub.set","mode":"periodic"})");
}

TEST_CASE("Notecard::command() propagates send error") {
    note::test::TestJsonBackend backend;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> { return "{}"; },
        [](note::string_view) -> note::Result<void> {
            return note::Unexpected(note::ErrorInfo{note::Error::Transport, "send failed"});
        });
    auto r = nc.command("card.restart");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Transport);
}

// ---------------------------------------------------------------------------
// command_typed() — typed fire-and-forget
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::command_typed() sends typed request as cmd") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<std::string> { return "{}"; },
        [&](note::string_view req) -> note::Result<void> {
            captured = std::string(req);
            return {};
        });
    note::api::HubSet req;
    req.mode("continuous");
    auto r = nc.command_typed(req);
    REQUIRE(r.has_value());
    REQUIRE(captured.find("\"cmd\":\"hub.set\"") != std::string::npos);
    REQUIRE(captured.find("\"mode\":\"continuous\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Safety enum utilities
// ---------------------------------------------------------------------------

TEST_CASE("Safety::to_string() returns correct strings") {
    REQUIRE(note::to_string(note::Safety::ReadOnly)      == "readonly");
    REQUIRE(note::to_string(note::Safety::Idempotent)    == "idempotent");
    REQUIRE(note::to_string(note::Safety::NonIdempotent) == "non-idempotent");
    REQUIRE(note::to_string(note::Safety::Destructive)   == "destructive");
}

TEST_CASE("Safety::is_safe_to_retry() is true for ReadOnly and Idempotent") {
    REQUIRE( note::is_safe_to_retry(note::Safety::ReadOnly));
    REQUIRE( note::is_safe_to_retry(note::Safety::Idempotent));
    REQUIRE_FALSE(note::is_safe_to_retry(note::Safety::NonIdempotent));
    REQUIRE_FALSE(note::is_safe_to_retry(note::Safety::Destructive));
}

// ---------------------------------------------------------------------------
// Error enum utilities
// ---------------------------------------------------------------------------

TEST_CASE("Error::to_string() returns correct strings") {
    REQUIRE(note::to_string(note::Error::Timeout)    == "timeout");
    REQUIRE(note::to_string(note::Error::Transport)  == "transport");
    REQUIRE(note::to_string(note::Error::Json)       == "json");
    REQUIRE(note::to_string(note::Error::Protocol)   == "protocol");
    REQUIRE(note::to_string(note::Error::NotReady)   == "not ready");
    REQUIRE(note::to_string(note::Error::Overflow)   == "overflow");
    REQUIRE(note::to_string(note::Error::InvalidArg) == "invalid argument");
}
