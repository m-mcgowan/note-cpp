// Smoke test: verifies the note-cpp API types compile and compose correctly.
// This won't link (no backend implementation), but proves the abstractions work.
//
// Build: clang++ -std=c++20 -fsyntax-only -I include examples/smoke.cpp

// Development utility — not a pedagogical example. See getting_started.cpp for learning.

#include <note/notecard.hpp>
#include <memory>

// Mock backend for compilation testing
struct MockBuilder : note::JsonBuilder {
    using JsonBuilder::add;
    using JsonBuilder::add_element;
    MockBuilder& add(note::string_view, bool) override { return *this; }
    MockBuilder& add(note::string_view, note::json_int_t) override { return *this; }
    MockBuilder& add(note::string_view, double) override { return *this; }
    MockBuilder& add(note::string_view, note::string_view) override { return *this; }
    MockBuilder& add_raw(note::string_view, note::string_view) override { return *this; }
    MockBuilder& begin_object(note::string_view) override { return *this; }
    MockBuilder& end_object() override { return *this; }
    MockBuilder& begin_array(note::string_view) override { return *this; }
    MockBuilder& end_array() override { return *this; }
    note::string_view to_view() override { return "{}"; }
};

struct MockReader : note::JsonReader {
    bool has(note::string_view) const override { return false; }
    bool get_bool(note::string_view, bool def) const override { return def; }
    note::json_int_t get_int(note::string_view, note::json_int_t def) const override { return def; }
    double get_double(note::string_view, double def) const override { return def; }
    note::string_view get_string(note::string_view, note::string_view def) const override { return def; }
    std::unique_ptr<note::JsonReader> get_object(note::string_view) const override { return nullptr; }
    bool has_error() const override { return false; }
    note::string_view get_error() const override { return {}; }
};

struct MockBackend : note::JsonBackend {
    std::unique_ptr<note::JsonBuilder> create_builder() override {
        return std::make_unique<MockBuilder>();
    }
    std::unique_ptr<note::JsonReader> parse_response(note::string_view) override {
        return std::make_unique<MockReader>();
    }
};

struct MockTransport : note::ITransport {
    using note::ITransport::transact;
    using note::ITransport::send;
    note::Result<note::string_view> transact(note::string_view, note::span<char> buf, uint32_t) override {
        constexpr note::string_view rsp = "{}";
        if (rsp.size() >= buf.size())
            return note::make_error(note::Error::Overflow, NOTE_ERR("response exceeds buffer"));
        std::memcpy(buf.data(), rsp.data(), rsp.size());
        return note::string_view(buf.data(), rsp.size());
    }
    note::Result<void> send(note::string_view) override { return {}; }
    void reset() override {}
    void abort() override {}

    struct NoopHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return note::Result<size_t>{size_t{0}}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return 0; }
        void delay(uint32_t) override {}
    } hal_;
    note::Hal& hal() override { return hal_; }
};

// Example generated request type (what the code generator would produce)
struct HubSetRequest {
    static constexpr note::string_view notecard_request = "hub.set";
    static constexpr bool supports_cmd = true;
    static constexpr note::Safety safety = note::Safety::Idempotent;

    struct Response {
        static Response parse(std::unique_ptr<note::JsonReader>) { return {}; }
        static Response parse(const note::JsonReader&) { return {}; }
    };

    void build(note::JsonBuilder& b) const {
        b.add("product", "com.example.test");
        b.add("mode", "periodic");
        b.add("outbound", int32_t{60});
    }
};

int main() {
    MockBackend backend;
    MockTransport transport;
    note::Notecard nc(backend, transport);

    // Type-safe generated request
    HubSetRequest req;
    auto result = nc.execute(req);
    if (result) {
        // success
    }

    // Ad-hoc request
    auto rsp = nc.request("card.version");
    if (rsp) {
        auto version = (*rsp)->get_string("version");
        (void)version;
    }

    // Fire-and-forget command
    auto cmd_result = nc.command("hub.set", [](note::JsonBuilder& b) {
        b.add("product", "com.example.test");
    });
    (void)cmd_result;

    // Error handling
    auto err = note::make_error(note::Error::ResponseLost, note::Cause::Timeout, "request timed out");
    (void)err;

    // Safety introspection
    static_assert(note::is_safe_to_retry(note::Safety::ReadOnly));
    static_assert(note::is_safe_to_retry(note::Safety::Idempotent));
    static_assert(!note::is_safe_to_retry(note::Safety::NonIdempotent));
    static_assert(!note::is_safe_to_retry(note::Safety::Destructive));
}
