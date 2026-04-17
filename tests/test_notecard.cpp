// Tests for Notecard: request(), command(), command_typed(), set_default_timeout(),
// backend(), execute() error paths, and transport send.

#include "catch.hpp"
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"

#include <note/notecard.hpp>
#include <note/safety.hpp>
#include <note/allocator.hpp>
#include <note/backends/buffer.hpp>
#include <note/api/card_version.hpp>
#include <note/api/card_restart.hpp>
#include <note/api/card_binary_put.hpp>
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
    note::json_int_t get_int(note::string_view, note::json_int_t d) const override { return d; }
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
    note::json_int_t get_int(note::string_view, note::json_int_t d) const override { return d; }
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

// ---------------------------------------------------------------------------
// Request IDs
// ---------------------------------------------------------------------------

TEST_CASE("Request IDs appear in wire format when enabled") {
    note::test::TestJsonBackend backend;
    std::vector<std::string> captured;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            captured.emplace_back(req);
            return "{}";
        });

    // Build a Notecard with request IDs enabled (don't use make_test_notecard
    // which disables them).
    note::Notecard nc(backend, transport);
    nc.set_retry_policy({.max_retries = 0});

    note::api::CardVersion req;
    nc.execute(req);
    nc.execute(req);

    REQUIRE(captured.size() == 2);
    // First request should have "id":1
    REQUIRE(captured[0].find("\"id\":1") != std::string::npos);
    // Second request should have "id":2 (incremented)
    REQUIRE(captured[1].find("\"id\":2") != std::string::npos);
}

TEST_CASE("Request IDs increment across different request types") {
    note::test::TestJsonBackend backend;
    std::vector<std::string> captured;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            captured.emplace_back(req);
            return "{}";
        });

    note::Notecard nc(backend, transport);
    nc.set_retry_policy({.max_retries = 0});

    note::api::CardVersion ver;
    note::api::CardRestart restart;
    nc.execute(ver);
    nc.execute(restart);
    nc.execute(ver);

    REQUIRE(captured.size() == 3);
    REQUIRE(captured[0].find("\"id\":1") != std::string::npos);
    REQUIRE(captured[1].find("\"id\":2") != std::string::npos);
    REQUIRE(captured[2].find("\"id\":3") != std::string::npos);
}

TEST_CASE("Request IDs do not appear when disabled") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            captured = std::string(req);
            return "{}";
        });

    auto nc = note::test::make_test_notecard(backend, transport);

    note::api::CardVersion req;
    nc.execute(req);

    REQUIRE(captured.find("\"id\":") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Notecard error with allocator (string interning via pool)
// ---------------------------------------------------------------------------

TEST_CASE("Notecard error with allocator interns the error message via pool") {
    // Use a backend whose reader reports a Notecard error (has_error=false,
    // get_error="firmware error"). This exercises the alloc_.has_value()
    // branch in execute_buffered.
    NotecardErrorJsonBackend backend("firmware error");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"err":"firmware error"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    // Set an allocator so the error message is interned into the pool
    // rather than kept alive by the owned JsonReader.
    char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    nc.set_allocator(note::arena_allocator(arena));

    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Notecard);
    REQUIRE(r.error().message == "firmware error");

    // The message should be interned into the arena, not pointing into the
    // transport buffer. Verify by checking the pointer is inside the arena.
    auto msg_ptr = r.error().message.data();
    auto arena_start = reinterpret_cast<const char*>(arena_buf);
    auto arena_end = arena_start + sizeof(arena_buf);
    REQUIRE(msg_ptr >= arena_start);
    REQUIRE(msg_ptr < arena_end);
}

TEST_CASE("Notecard error without allocator keeps reader alive for message") {
    // This is the existing behavior: without allocator, the owned reader is
    // stored in the ApiResult to keep the string_view alive.
    NotecardErrorJsonBackend backend("notecard not ready");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"err":"notecard not ready"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Notecard);
    REQUIRE(r.error().message == "notecard not ready");
}

// ---------------------------------------------------------------------------
// Execute with temporary allocator overload
// ---------------------------------------------------------------------------

TEST_CASE("execute(req, Allocator) uses the temporary allocator then restores") {
    // Use a real JSON backend so we can exercise the full buffered path
    // including string interning.
    note::backends::BufferJsonBackend<512, 32> backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"version":"1.2.3","board":"notecard"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    // No allocator set on the Notecard initially.
    char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    note::Allocator temp_alloc = note::arena_allocator(arena);

    note::api::CardVersion req;
    auto r = nc.execute(req, temp_alloc);
    REQUIRE(r.has_value());
    // The version string should be interned into the temp arena.
    auto ver_ptr = r.version.value().data();
    auto arena_start = reinterpret_cast<const char*>(arena_buf);
    auto arena_end = arena_start + sizeof(arena_buf);
    CHECK(ver_ptr >= arena_start);
    CHECK(ver_ptr < arena_end);

    // After the call, the Notecard should not retain the temporary allocator.
    // Execute again without allocator — strings point into transport buffer.
    auto r2 = nc.execute(req);
    REQUIRE(r2.has_value());
    auto ver_ptr2 = r2.version.value().data();
    // Should NOT be in the arena (the temp allocator was restored).
    bool in_arena = (ver_ptr2 >= arena_start && ver_ptr2 < arena_end);
    CHECK_FALSE(in_arena);
}

TEST_CASE("execute(req, Allocator) with error interns via temporary allocator") {
    NotecardErrorJsonBackend backend("temp alloc error");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"err":"temp alloc error"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    note::Allocator temp_alloc = note::arena_allocator(arena);

    note::api::CardVersion req;
    auto r = nc.execute(req, temp_alloc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Notecard);
    REQUIRE(r.error().message == "temp alloc error");

    // Error message interned into the temporary arena.
    auto msg_ptr = r.error().message.data();
    auto arena_start = reinterpret_cast<const char*>(arena_buf);
    auto arena_end = arena_start + sizeof(arena_buf);
    CHECK(msg_ptr >= arena_start);
    CHECK(msg_ptr < arena_end);
}

// ---------------------------------------------------------------------------
// Binary pre-flight failures
// ---------------------------------------------------------------------------

namespace {

// Harness for binary pre-flight tests: queues transact responses.
struct PreflightHarness {
    note::backends::BufferJsonBackend<512, 32> backend;
    std::vector<uint8_t> written_bytes;
    int transact_count = 0;
    std::vector<std::string> responses;

    note::CallbackTransport transport;
    note::Notecard nc;

    PreflightHarness(std::initializer_list<std::string> resps)
        : responses(resps)
        , transport(
            [this](note::string_view, uint32_t) -> note::Result<note::string_view> {
                size_t idx = static_cast<size_t>(transact_count++);
                if (idx < responses.size())
                    return note::string_view(responses[idx]);
                return note::string_view("{}");
            })
        , nc(note::test::make_test_notecard(backend, transport))
    {
        transport.set_write([this](const uint8_t* d, size_t n) -> note::Result<void> {
            written_bytes.insert(written_bytes.end(), d, d + n);
            return {};
        });
    }
};

// Harness where the transact call itself returns a transport error.
struct PreflightErrorHarness {
    note::backends::BufferJsonBackend<512, 32> backend;
    int transact_count = 0;
    int fail_at;  // which transact call (0-based) should return error

    note::CallbackTransport transport;
    note::Notecard nc;

    PreflightErrorHarness(int fail_at_index)
        : fail_at(fail_at_index)
        , transport(
            [this](note::string_view, uint32_t) -> note::Result<note::string_view> {
                int idx = transact_count++;
                if (idx == fail_at)
                    return note::Unexpected(note::ErrorInfo{
                        note::Error::SendFailed, {}, "transport error"});
                return note::string_view("{}");
            })
        , nc(note::test::make_test_notecard(backend, transport))
    {
        transport.set_write([](const uint8_t*, size_t) -> note::Result<void> {
            return {};
        });
    }
};

} // namespace

TEST_CASE("Binary PUT: pre-flight binary reset failure returns SendFailed") {
    // verify=true triggers: reset (card.binary delete) → fail
    // The first transact (reset) returns an error.
    PreflightErrorHarness h(0);  // fail at first transact

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));  // verify=true by default

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::SendFailed);
}

TEST_CASE("Binary PUT: pre-flight status query failure returns SendFailed") {
    // verify=true triggers: reset OK → status query fails
    // Transact 0 (reset) succeeds, transact 1 (status query) fails.
    PreflightErrorHarness h(1);  // fail at second transact

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::SendFailed);
}

TEST_CASE("Binary PUT: max_bytes=0 returns Overflow") {
    // reset OK, status returns max:0 → binary store not available
    PreflightHarness h({
        "{}",            // reset (card.binary delete)
        "{\"max\":0}",   // status: max=0
    });

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::Overflow);
}

TEST_CASE("Binary PUT: max_bytes negative returns Overflow") {
    // reset OK, status has no "max" field → binary_response_int returns 0
    PreflightHarness h({
        "{}",     // reset
        "{}",     // status with no "max" field → 0
    });

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::Overflow);
}

TEST_CASE("Binary PUT: source exceeds max_bytes returns Overflow") {
    // reset OK, status returns max:2 but data is 3 bytes → overflow
    PreflightHarness h({
        "{}",             // reset
        "{\"max\":2}",    // max=2, but data is 3 bytes
    });

    uint8_t data[] = {1, 2, 3};
    note::api::CardBinaryPut req;
    req.data(data, sizeof(data));

    auto rsp = h.nc.execute(req);
    REQUIRE_FALSE(rsp.has_value());
    CHECK(rsp.error().code == note::Error::Overflow);
}

// ---------------------------------------------------------------------------
// transact() — raw JSON passthrough
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::transact() rejects malformed JSON") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });
    auto nc = note::test::make_test_notecard(backend, transport);

    SECTION("not JSON at all") {
        auto r = nc.transact("not json");
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == note::Error::Json);
    }

    SECTION("empty string") {
        auto r = nc.transact("");
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == note::Error::Json);
    }
}

TEST_CASE("Notecard::transact() with OwnedBuffer returns response copy") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"ok":true})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    auto r = nc.transact(R"({"req":"card.version"})");
    REQUIRE(r.has_value());
    auto sv = r->view();
    REQUIRE(sv == R"({"ok":true})");
}

TEST_CASE("Notecard::transact() propagates transport error") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::Unexpected(note::ErrorInfo{note::Error::SendFailed, {}, "wire error"});
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    auto r = nc.transact(R"({"req":"card.version"})");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::SendFailed);
}

TEST_CASE("Notecard::transact() with caller buffer returns string_view into buffer") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"ok":true})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    char buf[128];
    auto r = nc.transact(R"({"req":"card.version"})", note::span<char>(buf, sizeof(buf)));
    REQUIRE(r.has_value());
    REQUIRE(*r == R"({"ok":true})");
    // The result should point into our buffer
    REQUIRE(r->data() >= buf);
    REQUIRE(r->data() < buf + sizeof(buf));
}

TEST_CASE("Notecard::transact() with caller buffer rejects malformed JSON") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });
    auto nc = note::test::make_test_notecard(backend, transport);

    char buf[128];
    auto r = nc.transact("not json", note::span<char>(buf, sizeof(buf)));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Json);
}

TEST_CASE("Notecard::transact() with caller buffer: overflow returns error") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"result":"this is a long response"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    // Buffer too small for the response.
    char buf[4];
    auto r = nc.transact(R"({"req":"card.version"})", note::span<char>(buf, sizeof(buf)));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Overflow);
}

// ---------------------------------------------------------------------------
// send() — raw JSON fire-and-forget
// ---------------------------------------------------------------------------

TEST_CASE("Notecard::send() validates JSON") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });
    auto nc = note::test::make_test_notecard(backend, transport);

    auto r = nc.send("not json");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::Json);
}

TEST_CASE("Notecard::send() sends valid JSON via buffered transport") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; },
        [&](note::string_view req) -> note::Result<void> {
            captured = std::string(req);
            return {};
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    auto r = nc.send(R"({"cmd":"card.restart"})");
    REQUIRE(r.has_value());
    REQUIRE(captured == R"({"cmd":"card.restart"})");
}

// ---------------------------------------------------------------------------
// Default-constructed Notecard: transact/send/command_typed return NotReady
// ---------------------------------------------------------------------------

TEST_CASE("Default-constructed Notecard: transact() returns NotReady") {
    note::Notecard nc;

    auto r = nc.transact(R"({"req":"card.version"})");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

TEST_CASE("Default-constructed Notecard: transact() with buffer returns NotReady") {
    note::Notecard nc;

    char buf[128];
    auto r = nc.transact(R"({"req":"card.version"})", note::span<char>(buf, sizeof(buf)));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

TEST_CASE("Default-constructed Notecard: send() returns NotReady") {
    note::Notecard nc;

    auto r = nc.send(R"({"cmd":"card.restart"})");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

TEST_CASE("Default-constructed Notecard: command_typed() returns NotReady") {
    note::Notecard nc;
    note::api::CardRestart req;
    auto r = nc.command_typed(req);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

TEST_CASE("Default-constructed Notecard: send_command() returns NotReady") {
    note::Notecard nc;
    auto build = [](note::JsonBuilder& b, void*) { b.add("cmd", note::string_view("test")); };
    auto r = nc.send_command(build, nullptr);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == note::Error::NotReady);
}

// ---------------------------------------------------------------------------
// set_allocator / clear_allocator
// ---------------------------------------------------------------------------

TEST_CASE("clear_allocator removes the allocator for subsequent calls") {
    NotecardErrorJsonBackend backend("alloc error");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"err":"alloc error"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    char arena_buf[256];
    note::MonotonicArena arena(arena_buf);
    nc.set_allocator(note::arena_allocator(arena));

    // First call uses arena allocator.
    note::api::CardVersion req;
    auto r1 = nc.execute(req);
    REQUIRE_FALSE(r1.has_value());
    auto msg1_ptr = r1.error().message.data();
    auto arena_start = reinterpret_cast<const char*>(arena_buf);
    auto arena_end = arena_start + sizeof(arena_buf);
    CHECK(msg1_ptr >= arena_start);
    CHECK(msg1_ptr < arena_end);

    // Clear the allocator, second call should NOT use arena.
    nc.clear_allocator();
    auto r2 = nc.execute(req);
    REQUIRE_FALSE(r2.has_value());
    REQUIRE(r2.error().message == "alloc error");
    auto msg2_ptr = r2.error().message.data();
    bool in_arena = (msg2_ptr >= arena_start && msg2_ptr < arena_end);
    CHECK_FALSE(in_arena);
}

// ---------------------------------------------------------------------------
// Successful execute with allocator interns response strings
// ---------------------------------------------------------------------------

TEST_CASE("Successful execute with allocator interns response strings into pool") {
    note::backends::BufferJsonBackend<512, 32> backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return R"({"version":"4.5.6","board":"notecard:v2"})";
        });
    auto nc = note::test::make_test_notecard(backend, transport);

    char arena_buf[512];
    note::MonotonicArena arena(arena_buf);
    nc.set_allocator(note::arena_allocator(arena));

    note::api::CardVersion req;
    auto r = nc.execute(req);
    REQUIRE(r.has_value());
    REQUIRE(r.version == "4.5.6");
    REQUIRE(r.board == "notecard:v2");

    // String data should be in the arena.
    auto ver_ptr = r.version.value().data();
    auto arena_start = reinterpret_cast<const char*>(arena_buf);
    auto arena_end = arena_start + sizeof(arena_buf);
    CHECK(ver_ptr >= arena_start);
    CHECK(ver_ptr < arena_end);
}

// ---------------------------------------------------------------------------
// Void-response execute with allocator (CardRestart)
// ---------------------------------------------------------------------------

TEST_CASE("Void-response execute succeeds with allocator set") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });
    auto nc = note::test::make_test_notecard(backend, transport);

    char arena_buf[128];
    note::MonotonicArena arena(arena_buf);
    nc.set_allocator(note::arena_allocator(arena));

    note::api::CardRestart req;
    auto r = nc.execute(req);
    REQUIRE(r.has_value());
}
