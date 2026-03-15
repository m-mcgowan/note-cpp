// Smoke test: verifies the note-cpp API types compile and compose correctly.
// This won't link (no backend implementation), but proves the abstractions work.
//
// Build: clang++ -std=c++20 -fsyntax-only -I include examples/smoke.cpp

#include <note/notecard.hpp>
#include <memory>

// Mock backend for compilation testing
struct MockBuilder : note::JsonBuilder {
    MockBuilder& add(note::string_view, bool) override { return *this; }
    MockBuilder& add(note::string_view, int32_t) override { return *this; }
    MockBuilder& add(note::string_view, double) override { return *this; }
    MockBuilder& add(note::string_view, note::string_view) override { return *this; }
    MockBuilder& begin_object(note::string_view) override { return *this; }
    MockBuilder& end_object() override { return *this; }
    MockBuilder& begin_array(note::string_view) override { return *this; }
    MockBuilder& end_array() override { return *this; }
    std::string to_string() override { return "{}"; }
};

struct MockReader : note::JsonReader {
    bool has(note::string_view) const override { return false; }
    bool get_bool(note::string_view, bool def) const override { return def; }
    int32_t get_int(note::string_view, int32_t def) const override { return def; }
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
    note::Notecard nc(backend,
        [](note::string_view, uint32_t) -> note::Result<note::string_view> {
            return note::string_view("{}");
        });

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
