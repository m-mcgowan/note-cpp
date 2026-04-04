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
//   - Register a template (note::template_of<Readings>() generates type hints)

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};


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
    nc.request("note.add", [](note::JsonBuilder& b) {
        b.add("file", "sensors.qo");
        b.add("body", R"({"temp":22.5,"humidity":60})");
    });


    // ═════════════════════════════════════════════════════════════════════════
    // 2. Builder body — construct body fields with a lambda
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Builder body ---");
    api.note.add()
        .file("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);
            b.add("humidity", int32_t{60});
        }))
        .execute();


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
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.note.add().file("sensors.qo").body(r).execute();
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 4. Template registration — auto-generates type hints
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Template registration ---");
    api.note.templates().define("sensors.qo")
        .body(note::template_of<Readings>())
        .execute();


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
        // Register the template once at startup. template_of<Readings>()
        // generates type hints from your struct's field types:
        //   float    → 14.1 (TFLOAT32)
        //   int16_t  → 11   (TINT16)
        api.note.templates().define("sensors.qo")
            .body(note::template_of<Readings>())
            .execute();

        // Then send notes as usual — the Notecard stores them compactly.
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.note.add().file("sensors.qo").body(r).execute();
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 6. Receive and parse — note.get with .into(T&)
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Receive and parse ---");
    {
        Readings data{};
        auto result = api.note.read("data.qi").into(data).execute();
        if (result) {
            (void)data.temperature;
            (void)data.humidity;
        }
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 7. Command (fire-and-forget) — sends "cmd" instead of "req"
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Fire-and-forget command ---");
    {
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.note.add().file("sensors.qo").body(r).command();
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 8. Compile-time JSON — zero-allocation constexpr buffer
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Compile-time JSON ---");
    {
        constexpr auto json = note::json<[](auto& b) {
            b.add("req", "note.add");
            b.add("file", "sensors.qo");
            b.begin_object("body");
                b.add("temp", 22.5);
                b.add("humidity", 60);
            b.end_object();
            b.close();
        }>();

        static_assert(json.view() ==
            R"({"req":"note.add","file":"sensors.qo","body":{"temp":22.5,"humidity":60}})");

        std::printf("  >> %.*s\n", (int)json.size(), json.data());
    }


    std::puts("\nAll sending-notes examples completed.");
    return 0;
}
