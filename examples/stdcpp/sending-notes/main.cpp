// Sending notes — every way to send data to and receive data from the Notecard.
//
// Notes are how your application sends data through the Notecard to Notehub
// (and receives data back). The body of a note is JSON — your sensor readings,
// commands, status, or whatever your application needs to communicate.
//
// You can build note bodies at three levels:
//   - Raw JSON strings (quick prototyping, no type safety)
//   - Builder lambdas (runtime construction, field-by-field)
//   - Typed structs (recommended — one struct for send, receive, and templates)
//
// This example shows all three, plus Notecard templates (which enable compact
// on-device storage — the Notecard stores your data as bit-packed binary
// instead of JSON text, reducing bandwidth and flash usage significantly).
//
// All example code in the accompanying README comes from this file, which is
// compiled as part of CI to verify correctness.
//
// Build & run:
//   c++ -std=c++20 -I ../../include main.cpp && ./a.out

#include <note/notecard.hpp>
#include <note/json_buf.hpp>
#include <note/api.hpp>
#include <note/body.hpp>

#include "../mock_backend.hpp"
#include <cstdio>


// ── Your application's sensor data ──────────────────────────────────────
// Define your data shape as a plain struct. On C++20, aggregate reflection
// discovers the fields automatically. On C++17, NOTE_FIELDS lists them.
//
// This one struct is used to:
//   - Send data (body of note.add)
//   - Receive data (.into(readings) on note.get request)
//   - Register a template (note::template_of(Readings()) generates type hints)

// readme:body-struct
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};
// readme:end


int main() {
    MockBackend backend;
    note::CallbackTransport transport(
        [](note::string_view request, uint32_t) -> note::Result<note::string_view> {
            std::printf("  >> %.*s\n", (int)request.size(), request.data());
            return note::string_view("{}");
        });
    note::Notecard nc(backend, transport);

    note::Api api(nc);


    // ═════════════════════════════════════════════════════════════════════════
    // 1. Ad-hoc note.add — raw JSON string body
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Ad-hoc note.add ---");
    // readme:adhoc
    // nc.request() sends a raw JSON request — you supply the request name
    // and a builder lambda that populates the fields.
    nc.request("note.add", [](note::JsonBuilder& b) {
        b.add("file", "sensors.qo");                     // target Notefile
        b.add("body", R"({"temp":22.5,"humidity":60})");  // body as raw JSON string
    });
    // readme:end


    // ═════════════════════════════════════════════════════════════════════════
    // 2. Builder body — construct body fields with a lambda
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Builder body ---");
    // readme:builder-body
    // note::body() wraps a lambda that builds the body JSON field-by-field.
    // The lambda receives a JsonBuilder — call b.add(key, value) for each field.
    api.note.add()
        .file("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);       // adds "temp":22.5 to the body object
            b.add("humidity", 60);     // adds "humidity":60 to the body object
        }))
        .execute();                          // sends the request to the Notecard
    // readme:end


    // ═════════════════════════════════════════════════════════════════════════
    // 3. Typed body struct — the recommended approach
    //
    // Define your data as a struct (see Readings above) and pass it directly.
    // The library serializes it to JSON automatically. No manual field names,
    // no risk of typos, and the same struct works for send, receive, and
    // template registration.
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Typed body struct ---");
    {
        // readme:typed-body
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.note.add().file("sensors.qo").body(r).execute();
        // readme:end
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 4. Template registration — auto-generates type hints
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Template registration ---");
    // readme:template-register
    // templates().define() tells the Notecard the shape of your data.
    // template_of(Readings()) inspects your struct and generates type hints:
    //   float   → 14.1 (TFLOAT32)
    //   int16_t → 11   (TINT16)
    // After this, Notes in "sensors.qo" are stored as compact binary.
    api.note.templates().define("sensors.qo")
        .body(note::template_of(Readings()))
        .execute();
    // readme:end


    // ═════════════════════════════════════════════════════════════════════════
    // 5. Template + send — the production pattern
    //
    // Without a template, the Notecard stores each note as a JSON string.
    // With a template, it knows the shape of your data upfront and stores
    // notes as compact bit-packed binary — dramatically reducing flash usage
    // and sync bandwidth. Register the template once at startup, then send
    // notes normally.
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Template + send ---");
    {
        // readme:template-send
        // Register the template once at startup. template_of(Readings())
        // generates type hints from your struct's field types:
        //   float    → 14.1 (TFLOAT32)
        //   int16_t  → 11   (TINT16)
        api.note.templates().define("sensors.qo")
            .body(note::template_of(Readings()))
            .execute();

        // Then send notes as usual — the Notecard stores them compactly.
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.note.add().file("sensors.qo").body(r).execute();
        // readme:end
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 6. Receive and parse — note.get with .into(T&)
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Receive and parse ---");
    {
        // readme:receive
        Readings data{};  // same struct used for sending — zero boilerplate
        // .into(data) tells execute() to parse the Note's body directly into
        // the struct. Fields are matched by name — no manual JSON parsing.
        auto result = api.note.read("data.qi").into(data).execute();
        if (result) {
            // data.temperature and data.humidity are now populated
            (void)data.temperature;
            (void)data.humidity;
        }
        // readme:end
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 7. Command (fire-and-forget) — sends "cmd" instead of "req"
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Fire-and-forget command ---");
    {
        // readme:command
        // .command() instead of .execute() — sends the request as a "cmd" rather
        // than a "req". The Notecard processes it but does NOT send a response,
        // so there's nothing to wait for. Good for fire-and-forget telemetry.
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.note.add().file("sensors.qo").body(r).command();
        // readme:end
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 8. Compile-time JSON — zero-allocation constexpr buffer
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Compile-time JSON ---");
    {
        // readme:constexpr-json
        // note::json<lambda>() builds JSON entirely at compile time — the result
        // is baked into your binary as a string constant. Zero runtime cost.
        // The compiler auto-sizes the buffer to fit the output.
        constexpr auto json = note::json<[](auto& b) {
            b.add("req", "note.add");        // top-level field: request name
            b.add("file", "sensors.qo");     // top-level field: target Notefile
            b.begin_object("body");          // open a nested JSON object for "body"
                b.add("temp", 22.5);         //   body field
                b.add("humidity", 60);       //   body field
            b.end_object();                  // close the "body" object
            b.close();                       // finalize — no more fields allowed after this
        }>();

        // Proof this happened at compile time — static_assert only works on
        // values known during compilation:
        static_assert(json.view() ==
            R"({"req":"note.add","file":"sensors.qo","body":{"temp":22.5,"humidity":60}})");
        // readme:end

        std::printf("  >> %.*s\n", (int)json.size(), json.data());
    }


    std::puts("\nAll sending-notes examples completed.");
    return 0;
}
