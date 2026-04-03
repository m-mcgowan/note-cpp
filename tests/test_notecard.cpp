// Tests for Notecard: request(), command(), command_typed(), set_default_timeout(),
// backend(), execute() error paths, and transport send.

#include "catch.hpp"
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/notecard.hpp>
#include <note/safety.hpp>
#include <note/api/card_version.hpp>
#include <note/api/card_restart.hpp>
#include <note/api/hub_set.hpp>

// ---------------------------------------------------------------------------
// A JsonReader that always reports a parse error (has_error() == true).
// ---------------------------------------------------------------------------
namespace {

class ParseErrorJsonReader : public note::JsonReader {
public:
    explicit ParseErrorJsonReader(std::string msg) : msg_(std::move(msg)) {}
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

class ParseErrorJsonBackend : public note::JsonBackend {
public:
    explicit ParseErrorJsonBackend(std::string error_msg) : error_msg_(std::move(error_msg)) {}
    std::unique_ptr<note::JsonBuilder> create_builder() override {
        return std::make_unique<note::test::TestJsonBuilder>();
    }
    std::unique_ptr<note::JsonReader> parse_response(note::string_view) override {
        return std::make_unique<ParseErrorJsonReader>(error_msg_);
    }
private:
    std::string error_msg_;
};

// ---------------------------------------------------------------------------
// A JsonReader that has a Notecard "err" field (has_error()==false but
// get_error() returns the error string).
// ---------------------------------------------------------------------------

class NotecardErrorJsonReader : public note::JsonReader {
public:
    explicit NotecardErrorJsonReader(std::string msg) : msg_(std::move(msg)) {}
    bool has(note::string_view key) const override { return key == "err"; }
    bool get_bool(note::string_view, bool d) const override { return d; }
    int32_t get_int(note::string_view, int32_t d) const override { return d; }
    double get_double(note::string_view, double d) const override { return d; }
    note::string_view get_string(note::string_view, note::string_view d) const override { return d; }
    std::unique_ptr<note::JsonReader> get_object(note::string_view) const override { return nullptr; }
    bool has_error() const override { return false; }
    note::string_view get_error() const override { return note::string_view(msg_); }
private:
    std::string msg_;
};

class NotecardErrorJsonBackend : public note::JsonBackend {
public:
    explicit NotecardErrorJsonBackend(std::string error_msg) : error_msg_(std::move(error_msg)) {}
    std::unique_ptr<note::JsonBuilder> create_builder() override {
        return std::make_unique<note::test::TestJsonBuilder>();
    }
    std::unique_ptr<note::JsonReader> parse_response(note::string_view) override {
        return std::make_unique<NotecardErrorJsonReader>(error_msg_);
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
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });
    auto nc = note::test::make_test_notecard(backend, transport);
    REQUIRE(&nc.backend() == &backend);
}

// ---------------------------------------------------------------------------
// Default timeout and set_default_timeout()
// ---------------------------------------------------------------------------

TEST_CASE("Notecard default timeout is 10000 ms") {
    note::test::TestJsonBackend backend;
    uint32_t captured_timeout = 0;
    note::CallbackTransport transport(
        [&](note::string_view, uint32_t t) -> note::Result<note::string_view> {
            captured_timeout = t;
            return "{}";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::api::CardVersion req;
    nc.execute(req);
    REQUIRE(captured_timeout == 10000);
}

TEST_CASE("Notecard::set_default_timeout() changes timeout passed to transport") {
    note::test::TestJsonBackend backend;
    uint32_t captured_timeout = 0;
    note::CallbackTransport transport(
        [&](note::string_view, uint32_t t) -> note::Result<note::string_view> {
            captured_timeout = t;
            return "{}";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    nc.set_default_timeout(5000);
    note::api::CardVersion req;
    nc.execute(req);
    REQUIRE(captured_timeout == 5000);
}

// ---------------------------------------------------------------------------
// command() uses transport send (fire-and-forget)
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::command() calls transport send") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; },
        [&](note::string_view req) -> note::Result<void> {
            captured = std::string(req);
            return {};
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.command("card.restart");
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"cmd":"card.restart"})");
}

TEST_CASE("Notecard::command() falls back to transact when no send_fn") {
    note::test::TestJsonBackend backend;
    bool transact_called = false;
    note::CallbackTransport transport(
        [&](note::string_view, uint32_t) -> note::Result<note::string_view> {
            transact_called = true;
            return "{}";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.command("card.restart");
    REQUIRE(r.has_value());
    REQUIRE(transact_called);
}

TEST_CASE("Notecard::command() propagates transport send error") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::Unexpected(note::ErrorInfo{note::Error::SendFailed, {}, "wire error"});
        },
        [](note::string_view) -> note::Result<void> {
            return note::Unexpected(note::ErrorInfo{note::Error::SendFailed, {}, "send failed"});
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.command("card.restart");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}

// ---------------------------------------------------------------------------
// request() — ad-hoc requests
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::request() sends req type with no extra fields") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            captured = std::string(req);
            return "{}";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.request("card.version");
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"req":"card.version"})");
}

TEST_CASE("Notecard::request() with build_fn adds fields to the request") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            captured = std::string(req);
            return "{}";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.request("hub.set", [](note::JsonBuilder& b) {
        b.add("mode", note::string_view("periodic"));
    });
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"req":"hub.set","mode":"periodic"})");
}

TEST_CASE("Notecard::request() returns a non-null JsonReader on success") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.request("card.version");
    REQUIRE(r.has_value());
    REQUIRE(r.value() != nullptr);
}

TEST_CASE("Notecard::request() propagates transport error") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::Unexpected(note::ErrorInfo{note::Error::SendFailed, {}, "lost"});
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.request("card.version");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}

TEST_CASE("Notecard::request() returns Json error on parse failure") {
    ParseErrorJsonBackend backend("invalid json");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"(not json)";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.request("card.version");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Json);
}

TEST_CASE("Notecard::request() returns reader even when response has err field") {
    NotecardErrorJsonBackend backend("notecard not ready");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"err":"notecard not ready"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.request("card.version");
    REQUIRE(r.has_value());
    REQUIRE((*r)->get_error() == "notecard not ready");
}

// ---------------------------------------------------------------------------
// execute() error paths
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::execute() propagates transport error") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::Unexpected(note::ErrorInfo{note::Error::SendFailed, {}, "io error"});
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}

TEST_CASE("Notecard::execute() returns Json error on parse failure") {
    ParseErrorJsonBackend backend("bad json");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"(not json)";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Json);
}

TEST_CASE("Notecard::execute() returns Notecard error when response has err field") {
    NotecardErrorJsonBackend backend("bad firmware");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"err":"bad firmware"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Notecard);
    REQUIRE(r.error().message == "bad firmware");
}

TEST_CASE("Default-constructed Notecard returns NotReady error") {
    note::Notecard nc;
    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

// ---------------------------------------------------------------------------
// command() — fire-and-forget
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::command() sends cmd type with no extra fields") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; },
        [&](note::string_view req) -> note::Result<void> {
            captured = std::string(req);
            return {};
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.command("card.restart");
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"cmd":"card.restart"})");
}

TEST_CASE("Notecard::command() with build_fn adds fields") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; },
        [&](note::string_view req) -> note::Result<void> {
            captured = std::string(req);
            return {};
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.command("hub.set", [](note::JsonBuilder& b) {
        b.add("mode", note::string_view("periodic"));
    });
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"cmd":"hub.set","mode":"periodic"})");
}

TEST_CASE("Notecard::command() propagates send error") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; },
        [](note::string_view) -> note::Result<void> {
            return note::Unexpected(note::ErrorInfo{note::Error::SendFailed, {}, "send failed"});
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    auto r = nc.command("card.restart");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}

// ---------------------------------------------------------------------------
// command_typed() — typed fire-and-forget
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::command_typed() sends typed request as cmd") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; },
        [&](note::string_view req) -> note::Result<void> {
            captured = std::string(req);
            return {};
        });
    auto nc = note::test::make_test_notecard(backend, transport);
    note::api::HubSet req;
    req.mode("continuous");
    auto r = nc.command_typed(req);
    REQUIRE(r.has_value());
    REQUIRE(captured.find("\"cmd\":\"hub.set\"") != std::string::npos);
    REQUIRE(captured.find("\"mode\":\"continuous\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// transport() accessor
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::transport() returns the ITransport reference") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });
    auto nc = note::test::make_test_notecard(backend, transport);
    REQUIRE(&nc.transport() == &transport);
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
// Error + Cause enum utilities
// ---------------------------------------------------------------------------

TEST_CASE("Error::to_string() returns correct strings") {
    REQUIRE(note::to_string(note::Error::SendFailed)   == "send_failed");
    REQUIRE(note::to_string(note::Error::ResponseLost) == "response_lost");
    REQUIRE(note::to_string(note::Error::Notecard)     == "notecard");
    REQUIRE(note::to_string(note::Error::Json)         == "json");
    REQUIRE(note::to_string(note::Error::NotReady)     == "not_ready");
    REQUIRE(note::to_string(note::Error::Overflow)     == "overflow");
    REQUIRE(note::to_string(note::Error::InvalidArg)   == "invalid_argument");
}

TEST_CASE("Cause::to_string() returns correct strings") {
    REQUIRE(note::to_string(note::Cause::Unspecified)  == "unspecified");
    REQUIRE(note::to_string(note::Cause::Timeout)      == "timeout");
    REQUIRE(note::to_string(note::Cause::TimeoutIntra) == "timeout_intra");
    REQUIRE(note::to_string(note::Cause::HalError)     == "hal_error");
    REQUIRE(note::to_string(note::Cause::CrcMismatch)  == "crc_mismatch");
}

TEST_CASE("to_string(ErrorInfo) without cause omits brackets") {
    note::ErrorInfo e{note::Error::Notecard, note::Cause::Unspecified, "not ready"};
    REQUIRE(note::to_string(e) == "notecard: not ready");
}

TEST_CASE("to_string(ErrorInfo) with cause includes brackets") {
    note::ErrorInfo e{note::Error::ResponseLost, note::Cause::Timeout, "no response"};
    REQUIRE(note::to_string(e) == "response_lost[timeout]: no response");
}
