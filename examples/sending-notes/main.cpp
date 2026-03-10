// Sending notes — every way to send data to and receive data from the Notecard.
//
// All example code in the accompanying README comes from this file, which is
// compiled as part of CI to verify correctness.
//
// Build & run:
//   c++ -std=c++2b -I ../../include main.cpp && ./a.out

#include <note/notecard.hpp>
#include <note/json_buf.hpp>
#include <note/api_context.hpp>
#include <note/body.hpp>

#include <cstdio>
#include <memory>
#include <string>

// ── Mock backend ────────────────────────────────────────────────────────────
// Replace with a real JSON library (cJSON, nlohmann-json, RapidJSON, etc.)
// on your platform. See examples/getting_started.cpp for a fuller mock.

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


// ── Body struct ─────────────────────────────────────────────────────────────
// Define once — use to send, receive, and register templates.

struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)
};


int main() {
    MockBackend backend;
    note::Notecard nc(backend,
        [](note::string_view request, uint32_t) -> note::Result<std::string> {
            std::printf("  >> %.*s\n", (int)request.size(), request.data());
            return std::string("{}");
        });

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
    api.noteAdd()
        .file("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temp", 22.5);
            b.add("humidity", int32_t{60});
        }))
        .execute();


    // ═════════════════════════════════════════════════════════════════════════
    // 3. Typed body struct — the recommended approach
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Typed body struct ---");
    {
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.noteAdd().file("sensors.qo").body(r).execute();
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 4. Template registration — auto-generates type hints
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Template registration ---");
    api.noteTemplate().set("sensors.qo")
        .body(note::template_of<Readings>())
        .execute();


    // ═════════════════════════════════════════════════════════════════════════
    // 5. Template + send — the production pattern
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Template + send ---");
    {
        // Register the template once at startup
        api.noteTemplate().set("sensors.qo")
            .body(note::template_of<Readings>())
            .execute();

        // Then send notes — the Notecard stores them compactly
        Readings r{.temperature = 22.5f, .humidity = 60};
        api.noteAdd().file("sensors.qo").body(r).execute();
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 6. Receive and parse — note.get with bodyAs<T>()
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Receive and parse ---");
    {
        auto result = api.noteGet().get().file("data.qi").execute();
        if (result) {
            auto data = result.bodyAs<Readings>();
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
        api.noteAdd().file("sensors.qo").body(r).command();
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
