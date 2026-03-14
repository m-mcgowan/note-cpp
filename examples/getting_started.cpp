// Getting started with note-cpp
//
// This walks through the library from simplest to most advanced:
//   1. Ad-hoc requests — JSON strings, no types needed
//   2. Compile-time JSON — zero-allocation constexpr buffers
//   3. Typed requests and responses — IDE autocomplete, compile-time checks
//   4. Body schemas — one struct for send, receive, and template registration
//
// Build & run:
//   c++ -std=c++2b -I include examples/getting_started.cpp && ./a.out

#include <note/notecard.hpp>
#include <note/json_buf.hpp>
#include <note/api_context.hpp>
#include <note/body.hpp>

#include <cstdio>
#include <memory>
#include <string>

// ═══════════════════════════════════════════════════════════════════════
// Mock backend — replace with a real JSON library on your platform.
// A production backend wraps cJSON, nlohmann-json, RapidJSON, etc.
// ═══════════════════════════════════════════════════════════════════════

struct MockBuilder : note::JsonBuilder {
    std::string buf_ = "{";
    bool first_ = true;
    void sep() { if (!first_) buf_ += ','; first_ = false; }

    MockBuilder& add(note::string_view k, bool v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += v ? "true" : "false"; return *this;
    }
    MockBuilder& add(note::string_view k, int32_t v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += std::to_string(v); return *this;
    }
    MockBuilder& add(note::string_view k, double v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += std::to_string(v); return *this;
    }
    MockBuilder& add(note::string_view k, note::string_view v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":\""; buf_ += v; buf_ += '"'; return *this;
    }
    MockBuilder& begin_object(note::string_view k) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":{"; first_ = true; return *this;
    }
    MockBuilder& end_object() override { buf_ += '}'; first_ = false; return *this; }
    MockBuilder& begin_array(note::string_view k) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":["; first_ = true; return *this;
    }
    MockBuilder& end_array() override { buf_ += ']'; first_ = false; return *this; }
    std::string to_string() override { buf_ += '}'; return std::move(buf_); }
};

struct MockReader : note::JsonReader {
    bool has(note::string_view) const override { return false; }
    bool get_bool(note::string_view, bool d) const override { return d; }
    int32_t get_int(note::string_view, int32_t d) const override { return d; }
    double get_double(note::string_view, double d) const override { return d; }
    note::string_view get_string(note::string_view, note::string_view d) const override { return d; }
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

// Body struct — define once, use to send, receive, and register templates.
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

int main() {
    // Create a backend and Notecard instance.
    // The transport lambda sends JSON over serial/I2C and returns the response.
    MockBackend backend;
    note::Notecard nc(backend,
        [](note::string_view request, uint32_t) -> note::Result<note::string_view> {
            std::printf("  >> %.*s\n", (int)request.size(), request.data());
            return note::string_view("{}");
        });


    // ═══════════════════════════════════════════════════════════════════
    // 1. AD-HOC REQUESTS
    //    The simplest way to talk to a Notecard. Build JSON fields with
    //    a lambda, read responses with string keys. No generated types
    //    needed — just notecard.hpp.
    // ═══════════════════════════════════════════════════════════════════

    std::puts("\n=== 1. Ad-hoc requests ===\n");

    // Configure the product
    std::puts("--- hub.set ---");
    nc.request("hub.set", [](note::JsonBuilder& b) {
        b.add("product", "com.example.app");
        b.add("mode", "periodic");
        b.add("outbound", 60);
    });

    // Read device info
    std::puts("--- card.version ---");
    {
        auto result = nc.request("card.version");
        if (result) {
            auto version = (*result)->get_string("version");
            auto device  = (*result)->get_string("device");
            (void)version; (void)device;
            std::puts("  (version and device read from response)");
        }
    }

    // Fire-and-forget command (no response expected)
    std::puts("--- hub.sync (command) ---");
    nc.command("hub.sync");

    // Error handling — all operations return a result
    std::puts("--- error handling ---");
    {
        auto r = nc.request("card.version");
        if (!r) {
            std::printf("  error: %.*s\n",
                (int)r.error().message.size(), r.error().message.data());
        } else {
            std::puts("  ok");
        }
    }


    // ═══════════════════════════════════════════════════════════════════
    // 2. COMPILE-TIME JSON
    //    For static requests that never change, JsonBuf builds the JSON
    //    at compile time. Zero allocation, zero runtime cost. The
    //    compiler verifies the JSON is well-formed.
    // ═══════════════════════════════════════════════════════════════════

    std::puts("\n=== 2. Compile-time JSON ===\n");

    // Auto-sized: compiler measures the output and picks the buffer size.
    constexpr auto hub_set_json = note::json<[](auto& b) {
        b.add("req", "hub.set");
        b.add("product", "com.example.app");
        b.add("mode", "periodic");
        b.add("outbound", 60);
        b.close();
    }>();

    // The string is computed entirely at compile time:
    static_assert(hub_set_json.view() ==
        R"({"req":"hub.set","product":"com.example.app","mode":"periodic","outbound":60})");

    std::printf("--- constexpr hub.set ---\n  >> %.*s\n",
        (int)hub_set_json.size(), hub_set_json.data());

    // Fixed-size buffer for explicit control:
    constexpr note::JsonBuf<128> note_add_json = [] {
        note::JsonBuf<128> b;
        b.add("req", "note.add");
        b.add("file", "sensors.qo");
        b.begin_object("body");
            b.add("temp", 22.5);
        b.end_object();
        b.close();
        return b;
    }();

    static_assert(note_add_json);  // overflow check
    std::printf("--- constexpr note.add ---\n  >> %.*s\n",
        (int)note_add_json.size(), note_add_json.data());


    // ═══════════════════════════════════════════════════════════════════
    // 3. TYPED API — REQUESTS AND RESPONSES
    //    Generated types give you named fields, IDE autocomplete, and
    //    compile-time checking. Misspell a field → compile error.
    // ═══════════════════════════════════════════════════════════════════

    std::puts("\n=== 3. Typed API ===\n");

    // Api is the entry point for all typed requests.
    note::Api api(nc);

    // Fluent chain — IDE auto-completes every setter
    std::puts("--- hub.set (fluent) ---");
    api.hub.set()
       .product("com.example.app")
       .mode("periodic")
       .outbound(60)
       .execute();

    // Direct field assignment
    std::puts("--- hub.set (direct) ---");
    {
        auto req = api.hub.set();
        req.product = "com.example.app";
        req.mode = "periodic";
        req.outbound = 60;
        req.execute();
    }

    // Typed response — fields are named members, not strings
    std::puts("--- card.version (typed response) ---");
    {
        auto result = api.card.version().execute();
        if (result) {
            auto version = result.version;
            auto device  = result.device;
            (void)version; (void)device;
        } else {
            printf("error: %s\n", to_string(result.error()).c_str());
        }
    }

    // Polymorphic endpoints — note.get has two variants:
    //   .get()     — read-only
    //   .delete_() — pop from queue
    std::puts("--- note.get ---");
    api.note.get().get().file("data.qi").execute();

    // note.get delete (pop)
    std::puts("--- note.get delete (pop) ---");
    api.note.get().delete_().file("requests.qi").execute();

    // Fire-and-forget command — sends "cmd" instead of "req"
    std::puts("--- hub.set (command) ---");
    api.hub.set().product("com.example.app").command();


    // ═══════════════════════════════════════════════════════════════════
    // 4. BODY SCHEMAS
    //    Define a struct once. Use it to send data, receive data, and
    //    register Notecard templates. The same type does all three.
    // ═══════════════════════════════════════════════════════════════════

    std::puts("\n=== 4. Body schemas ===\n");

    // Send a note with a typed body
    std::puts("--- note.add (typed body) ---");
    {
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.note.add().file("sensors.qo").body(r).execute();
    }

    // Inline initialization
    std::puts("--- note.add (inline body) ---");
    api.note.add()
       .file("sensors.qo")
       .body(Readings{.temperature = 22.5f, .humidity = 60})
       .execute();

    // Direct assignment — request fields and body
    std::puts("--- note.add (assigned body) ---");
    {
        Readings r;
        r.temperature = 22.5f;
        r.humidity = 60;
        auto req = api.note.add();
        req.file = "sensors.qo";
        req.body(r);
        req.execute();
    }

    // Register a Notecard template — auto-generates type hints
    // (14.1 = TFLOAT32, 11 = TINT16)
    std::puts("--- note.template ---");
    api.note.template_().set("sensors.qo")
        .body(note::template_of<Readings>())
        .execute();

    // Parse a response body back into the struct
    std::puts("--- note.get (parse body) ---");
    {
        auto r = api.note.get().get().file("data.qi").execute();
        if (r) {
            Readings data = r.bodyAs<Readings>();
            (void)data.temperature;
            (void)data.humidity;
            std::puts("  (body parsed into Readings struct)");
        }
    }

    // Bodies also work without a struct — three tiers:
    //   Tier 1: Raw JSON string
    std::puts("--- note.add (raw JSON body) ---");
    api.note.add()
        .file("sensors.qo")
        .body(R"({"temp":22.5})")
        .execute();

    //   Tier 2: Builder lambda
    std::puts("--- note.add (builder body) ---");
    api.note.add()
        .file("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);
            b.add("humidity", int32_t{60});
        }))
        .execute();

    //   Tier 3: Schema struct (shown above with Readings)


    std::puts("\nAll examples completed.");
    return 0;
}
