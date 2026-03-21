// Getting started with note-cpp
//
// The Notecard speaks JSON over serial or I2C. You can build requests at
// different levels of abstraction depending on what you need — from raw
// JSON strings (simplest, no type safety) to fully typed request builders
// (IDE autocomplete, compile-time error checking).
//
// This example walks through each level so you can see what fits your
// situation:
//
//   1. Ad-hoc requests   — raw JSON, no types needed, quickest path
//   2. Compile-time JSON — same raw approach, but the compiler builds the
//                          JSON at compile time — zero runtime cost
//   3. Typed API         — named fields with autocomplete; misspellings
//                          are compile errors, not runtime surprises
//   4. Body schemas      — define your data struct once, use it to send,
//                          receive, and register Notecard templates
//
// Build & run:
//   c++ -std=c++20 -I include examples/getting_started.cpp && ./a.out

#include <note/notecard.hpp>
#include <note/json_buf.hpp>
#include <note/api.hpp>
#include <note/body.hpp>

#include "mock_backend.hpp"
#include <cstdio>


// ── Your application's sensor data ──────────────────────────────────────
// Define once — use to send, receive, and register Notecard templates.
// On C++20, the NOTE_FIELDS macro is optional (aggregate reflection works
// automatically). On C++17, it tells the library your field names.

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};


int main() {
    // Create a backend and a Notecard instance.
    //
    // On real hardware, you'd use a JSON backend (cJSON, nlohmann, etc.)
    // and a transport (serial or I2C). For example, with Arduino:
    //
    //   #include <note/backends/cjson.hpp>
    //   #include <note/transport/serial.hpp>
    //   note::backends::CjsonBackend backend;
    //   auto transport = note::NotecardSerial(note::arduino::serial_hal(Serial1));
    //   note::Notecard nc(backend, transport);
    //
    // For this example, we use a mock that prints each request to stdout
    // so you can run it on any machine without Notecard hardware.
    MockBackend backend;
    note::Notecard nc(backend,
        [](note::string_view request, uint32_t) -> note::Result<note::string_view> {
            std::printf("  >> %.*s\n", (int)request.size(), request.data());
            return note::string_view("{}");
        });


    // ═════════════════════════════════════════════════════════════════════
    // 1. AD-HOC REQUESTS
    //
    // The quickest way to talk to a Notecard. You provide the request
    // name as a string and build fields with a lambda. Field names are
    // unchecked strings — convenient for prototyping, but typos compile
    // fine and fail silently at runtime.
    // ═════════════════════════════════════════════════════════════════════

    std::puts("\n=== 1. Ad-hoc requests ===\n");

    // Configure the Notecard's connection to Notehub.
    std::puts("--- hub.set ---");
    nc.request("hub.set", [](note::JsonBuilder& b) {
        b.add("product", "com.example.app");
        b.add("mode", "periodic");
        b.add("outbound", 60);
    });

    // Read device info. The response is a JsonReader with string-keyed
    // accessors — you need to know the field names from the docs.
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

    // Fire-and-forget command — sends "cmd" instead of "req", so the
    // Notecard doesn't send a response. Useful for triggers like sync.
    std::puts("--- hub.sync (command) ---");
    nc.command("hub.sync");

    // Every operation returns a result. Check it to handle errors.
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


    // ═════════════════════════════════════════════════════════════════════
    // 2. COMPILE-TIME JSON
    //
    // If a request never changes (same fields, same values), the compiler
    // can build the entire JSON string at compile time. The result is a
    // fixed-size buffer with zero runtime allocation and zero runtime
    // cost — the JSON is baked into your binary.
    //
    // This uses a C++20 feature (constexpr lambda in template argument).
    // ═════════════════════════════════════════════════════════════════════

    std::puts("\n=== 2. Compile-time JSON ===\n");

    // Auto-sized: the compiler measures the JSON output and picks the
    // smallest buffer that fits.
    constexpr auto hub_set_json = note::json<[](auto& b) {
        b.add("req", "hub.set");
        b.add("product", "com.example.app");
        b.add("mode", "periodic");
        b.add("outbound", 60);
        b.close();
    }>();

    // Proof that this happened at compile time — static_assert runs
    // during compilation, not at runtime.
    static_assert(hub_set_json.view() ==
        R"({"req":"hub.set","product":"com.example.app","mode":"periodic","outbound":60})");

    std::printf("--- constexpr hub.set ---\n  >> %.*s\n",
        (int)hub_set_json.size(), hub_set_json.data());

    // You can also specify a fixed buffer size for explicit control:
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

    static_assert(note_add_json);  // verifies the buffer didn't overflow
    std::printf("--- constexpr note.add ---\n  >> %.*s\n",
        (int)note_add_json.size(), note_add_json.data());


    // ═════════════════════════════════════════════════════════════════════
    // 3. TYPED API — REQUESTS AND RESPONSES
    //
    // This is the recommended approach for most code. Request types are
    // generated from the Notecard API spec — every field is a named
    // member with the correct type. Your IDE auto-completes after the
    // dot, and misspelled field names are compile errors.
    //
    // Responses are also typed: result.version is a string_view, not a
    // string you look up by key.
    // ═════════════════════════════════════════════════════════════════════

    std::puts("\n=== 3. Typed API ===\n");

    // Api wraps a Notecard and provides typed accessors for every endpoint.
    note::Api api(nc);

    // Fluent chain — build and execute in one expression.
    std::puts("--- hub.set (fluent) ---");
    api.hub.set()
       .product("com.example.app")
       .mode("periodic")
       .outbound(60)
       .execute();

    // Direct field assignment — useful when fields come from different
    // sources or are set conditionally.
    std::puts("--- hub.set (direct) ---");
    {
        auto req = api.hub.set();
        req.product = "com.example.app";
        req.mode = "periodic";
        req.outbound = 60;
        req.execute();
    }

    // Typed response — fields are named members, not string lookups.
    std::puts("--- card.version (typed response) ---");
    {
        auto result = api.card.version().execute();
        if (result) {
            auto version = result.version;   // string_view, not a map lookup
            auto device  = result.device;
            (void)version; (void)device;
        } else {
            // Structured error with code, cause, and human-readable message.
            printf("error: %s\n", to_string(result.error()).c_str());
        }
    }

    // Intent-based aliases for polymorphic endpoints. The Notecard's
    // note.get can either read a note by ID or pop one from a queue
    // depending on which fields you send. In note-cpp, these are
    // separate methods with clear names:
    std::puts("--- note.read (read by ID) ---");
    api.note.read("data.db").noteId("my-note").execute();

    std::puts("--- note.pop (pop from queue) ---");
    api.note.pop("requests.qi").execute();

    // Fire-and-forget command — sends "cmd" instead of "req".
    std::puts("--- hub.set (command) ---");
    api.hub.set().product("com.example.app").command();


    // ═════════════════════════════════════════════════════════════════════
    // 4. BODY SCHEMAS
    //
    // Notes carry a JSON body with your application's data. You can send
    // bodies as raw JSON strings, builder lambdas, or typed structs.
    //
    // The struct approach is the most powerful: define your data shape
    // once (see Readings at the top of this file), then use the same
    // type to send data, receive data, and register Notecard templates
    // (which enable compact on-device storage).
    // ═════════════════════════════════════════════════════════════════════

    std::puts("\n=== 4. Body schemas ===\n");

    // Send a note with your struct as the body.
    std::puts("--- note.add (typed body) ---");
    {
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.note.add().file("sensors.qo").body(r).execute();
    }

    // Inline works too — no need to name a variable.
    std::puts("--- note.add (inline body) ---");
    api.note.add()
       .file("sensors.qo")
       .body(Readings{.temperature = 22.5f, .humidity = 60})
       .execute();

    // Direct assignment — useful when building the body incrementally.
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

    // Register a Notecard template. This tells the Notecard the shape of
    // your data so it can store notes compactly (bit-packed binary instead
    // of JSON text). The type hints are generated automatically from your
    // struct's field types (e.g. float → 14.1, int16_t → 11).
    std::puts("--- note.template ---");
    api.note.templates().define("sensors.qo")
        .body(note::template_of<Readings>())
        .execute();

    // Parse a response body back into your struct.
    std::puts("--- note.read (parse body) ---");
    {
        auto r = api.note.read("data.qi").execute();
        if (r) {
            Readings data = r.bodyAs<Readings>();
            (void)data.temperature;
            (void)data.humidity;
            std::puts("  (body parsed into Readings struct)");
        }
    }

    // You don't have to use structs. Bodies also accept raw JSON strings
    // or builder lambdas — useful when the shape varies at runtime.
    std::puts("--- note.add (raw JSON body) ---");
    api.note.add()
        .file("sensors.qo")
        .body(R"({"temp":22.5})")
        .execute();

    std::puts("--- note.add (builder body) ---");
    api.note.add()
        .file("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);
            b.add("humidity", int32_t{60});
        }))
        .execute();


    std::puts("\nAll examples completed.");
    return 0;
}
