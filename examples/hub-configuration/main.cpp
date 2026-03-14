// Hub configuration — connection setup with type-safe units and named constants.
//
// All example code in the accompanying README comes from this file, which is
// compiled as part of CI to verify correctness.
//
// Build & run:
//   c++ -std=c++2b -I ../../include main.cpp && ./a.out

#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/voltage_variable.hpp>

#include <cstdio>
#include <memory>
#include <string>

// ── Mock backend (see examples/getting_started.cpp for details) ─────────────

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


int main() {
    MockBackend backend;
    note::Notecard nc(backend,
        [](note::string_view request, uint32_t) -> note::Result<note::string_view> {
            std::printf("  >> %.*s\n", (int)request.size(), request.data());
            return note::string_view("{}");
        });

    note::Api api(nc);
    using namespace note::literals;


    // ═════════════════════════════════════════════════════════════════════════
    // 1. Basic hub.set — fluent and direct styles
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Fluent style ---");
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60)
        .inbound(120)
        .execute();

    std::puts("\n--- Direct assignment ---");
    {
        auto req = api.hub.set();
        req.product = "com.example.app";
        req.mode = "continuous";
        req.execute();
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 2. Type-safe units — prevent accidental unit mixing
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Type-safe units ---");
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60_mins)       // Minutes literal
        .inbound(120_minutes)    // Long-form also works
        .seconds(300_s)          // Seconds literal
        .execute();

    // Raw integers still work — they implicitly convert to the correct unit:
    api.hub.set()
        .outbound(60)            // int → Minutes (outbound is in minutes)
        .seconds(300)            // int → Seconds (seconds is in seconds)
        .execute();

    // But you can't mix them up — this would be a compile error:
    //   api.hubSet().outbound(60_s);  // error: Seconds ≠ Minutes

    // Hours and Days convert implicitly to smaller units:
    std::puts("\n--- Hours and Days ---");
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(2_hours)       // Hours → Minutes (= 120 on the wire)
        .inbound(7_days)         // Days → Minutes (= 10080 on the wire)
        .execute();

    // Works across the API — anywhere a Seconds field is expected:
    api.card.sleep()
        .seconds(12_hours)       // Hours → Seconds (= 43200 on the wire)
        .execute();

    api.card.attn()
        .mode("arm")
        .seconds(5_mins)         // Minutes → Seconds (= 300 on the wire)
        .execute();


    // ═════════════════════════════════════════════════════════════════════════
    // 3. Named constants — self-documenting special values
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Named constants ---");
    {
        using outbound_t = note::api::HubSet::outbound_t;
        using inbound_t  = note::api::HubSet::inbound_t;

        // Reset outbound to default (sends -1 on the wire)
        api.hub.set().outbound(outbound_t::reset).execute();

        // Manual sync only — no automatic outbound (sends 0)
        api.hub.set().outbound(outbound_t::manual).execute();

        // Same constants exist for inbound
        api.hub.set().inbound(inbound_t::reset).execute();
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 4. Consteval validation — catch typos at compile time
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- Consteval validation ---");
    api.hub.set()
        .product("com.example.app")
        .mode(note::api::HubSet::validatedMode("periodic"))
        .execute();

    // This would fail at compile time:
    //   .mode(note::api::HubSet::validatedMode("perioidc"))
    //   error: "hub.set: invalid value for 'mode'"


    // ═════════════════════════════════════════════════════════════════════════
    // 5. Voltage-variable strings — adaptive sync based on power
    // ═════════════════════════════════════════════════════════════════════════

    // Raw string — works but easy to get the format wrong:
    std::puts("\n--- Voltage-variable sync (raw string) ---");
    api.hub.set()
        .mode("periodic")
        .voutbound("usb:5;high:15;normal:60;low:240;dead:0")
        .execute();

    // Builder — type-safe, built directly on the field:
    std::puts("\n--- Voltage-variable sync (builder) ---");
    {
        auto req = api.hub.set();
        req.mode = "periodic";
        req.voutbound.usb(5).high(15).normal(60).low(240).dead(0);
        req.vinbound.usb(5).high(30).normal(120).low(1440).dead(0);
        req.execute();
    }


    // ═════════════════════════════════════════════════════════════════════════
    // 6. USB-variable booleans — automatic mode switching on USB power
    // ═════════════════════════════════════════════════════════════════════════

    std::puts("\n--- USB-variable sync ---");
    // Stay continuous on USB, fall back to periodic on battery
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .outbound(60_mins)
        .uperiodic(true)
        .execute();

    // Stay continuous on USB, fall back to minimum on battery
    api.hub.set()
        .mode("minimum")
        .umin(true)
        .execute();

    // Stay continuous on USB, fall back to off on battery
    api.hub.set()
        .mode("off")
        .uoff(true)
        .execute();


    std::puts("\nAll hub-configuration examples completed.");
    return 0;
}
