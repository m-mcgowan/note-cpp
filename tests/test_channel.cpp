// Tests for DirectChannel — execute(), command(), tick(), notecard() accessor,
// and error propagation (transport and protocol errors).

#include "catch.hpp"
#include "test_json_backend.hpp"

#include <note/app/channel.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>

// A JsonReader that always reports an error — for protocol error path tests.
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
// execute() — forwards to Notecard::execute()
// ---------------------------------------------------------------------------

TEST_CASE("DirectChannel::execute() forwards to Notecard::execute()") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            captured = std::string(req);
            return note::string_view("{}");
        });
    note::Notecard nc(backend, transport);

    note::app::DirectChannel ch(nc);
    auto r = ch.execute(note::api::CardVersion{});
    REQUIRE(r);
    REQUIRE(captured.find("card.version") != std::string::npos);
}

// ---------------------------------------------------------------------------
// execute() — transport error propagation
// ---------------------------------------------------------------------------

TEST_CASE("DirectChannel::execute() propagates transport errors") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::make_error(note::Error::ResponseLost, note::Cause::Timeout, "timed out");
        });
    note::Notecard nc(backend, transport);

    note::app::DirectChannel ch(nc);
    auto r = ch.execute(note::api::CardVersion{});
    REQUIRE(!r);
    REQUIRE(r.error().code == note::Error::ResponseLost);
}

// ---------------------------------------------------------------------------
// execute() — protocol error propagation
// ---------------------------------------------------------------------------

TEST_CASE("DirectChannel::execute() propagates protocol errors") {
    ErrorJsonBackend backend("bad response");
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view("{}");
        });
    note::Notecard nc(backend, transport);

    note::app::DirectChannel ch(nc);
    auto r = ch.execute(note::api::CardVersion{});
    REQUIRE(!r);
    REQUIRE(r.error().code == note::Error::Json);
}

// ---------------------------------------------------------------------------
// command() — forwards to Notecard::command_typed()
// ---------------------------------------------------------------------------

TEST_CASE("DirectChannel::command() forwards to Notecard::command_typed()") {
    note::test::TestJsonBackend backend;
    std::string captured;
    note::CallbackTransport transport(
        [&](note::string_view req, uint32_t) -> note::Result<note::string_view> {
            captured = std::string(req);
            return note::string_view("{}");
        });
    note::Notecard nc(backend, transport);

    note::app::DirectChannel ch(nc);
    auto r = ch.command(note::api::HubSet{});
    REQUIRE(r.has_value());
    REQUIRE(captured.find("\"cmd\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// tick() — no-op
// ---------------------------------------------------------------------------

TEST_CASE("DirectChannel::tick() is a no-op") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view("{}");
        });
    note::Notecard nc(backend, transport);

    note::app::DirectChannel ch(nc);
    ch.tick();  // should compile and not crash
}

// ---------------------------------------------------------------------------
// notecard() accessor
// ---------------------------------------------------------------------------

TEST_CASE("DirectChannel::notecard() returns the wrapped Notecard") {
    note::test::TestJsonBackend backend;
    note::CallbackTransport transport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view("{}");
        });
    note::Notecard nc(backend, transport);

    note::app::DirectChannel ch(nc);
    REQUIRE(&ch.notecard() == &nc);
}
