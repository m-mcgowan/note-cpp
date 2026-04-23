// Environment variables with env.get — all the ways to use it.
//
// The Notecard exposes a key/value environment-variable store. Values
// come from Notecard local defaults (env.default), the host (env.set),
// or Notehub (synced from the project). env.get is how the firmware
// reads them. Four modes:
//
//   1. One variable  → response.text is the value.
//   2. Many variables → response body is an object; stream into a struct.
//   3. All variables  → same as (2) but without a names list.
//   4. Conditional   → .time(t) returns results only if changed since t.
//
// This example uses a streaming transport so `.into(cfg)` actually
// populates the struct — that path is SAX-based and lives in the
// streaming transport, not the buffered backend. See docs on how to
// switch between streaming and buffered in production.
//
// Build & run:
//   c++ -std=c++20 -I include examples/stdcpp/env-vars.cpp && ./a.out

#include <note/allocator.hpp>
#include <note/api.hpp>
#include <note/body.hpp>
#include <note/notecard.hpp>
#include <note/streaming_transport.hpp>
#include <note/transport_hal.hpp>

#include <cstdio>
#include <deque>
#include <string>
#include <string_view>

using namespace note;

// Fields map one-to-one to env-var names on the Notecard. StructSink
// matches body-object keys to struct fields by name. Fields without a
// matching env var are left default-constructed.
struct DeviceConfig {
    note::string_view region;
    note::string_view locale;
    note::json_int_t  interval;
    NOTE_FIELDS(region, locale, interval)
};

// Streaming mock HAL. Serves one canned JSON response per request; the
// response is selected by inspecting the transmitted bytes so each of
// the four env.get modes gets the right answer.
class CannedHal : public note::TransportHal {
public:
    std::string                tx_buf;
    std::deque<uint8_t>        rx;

    void prime(const std::string& response) {
        rx.clear();
        for (char c : response) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back('\n');
    }

    bool transmit(const uint8_t* data, size_t len) override {
        tx_buf.append(reinterpret_cast<const char*>(data), len);
        return true;
    }

    // StreamingTransport calls this after the JSON body to finish the
    // line. We treat it as the request-complete signal: print the captured
    // request and prime the canned response.
    bool write_line_terminator() override {
        if (!tx_buf.empty()) {
            std::printf("  >> %s\n", tx_buf.c_str());
            if (tx_buf.find("\"req\":") != std::string::npos)
                choose_response(tx_buf);
            tx_buf.clear();
        }
        return true;
    }

    note::Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t) override {
        if (rx.empty())
            return note::make_error(note::Error::ResponseLost, note::Cause::Timeout, "no data");
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
        return n;
    }

    bool reset() override { return true; }
    void delay(uint32_t) override {}
    uint32_t millis() override { return 0; }

private:
    // Pick the canned response based on the request line just transmitted.
    void choose_response(const std::string& req) {
        if (req.find(R"("name":"region")") != std::string::npos)
            prime(R"({"text":"us-east-1","time":1700000000})");
        else if (req.find(R"("names":)") != std::string::npos)
            prime(R"({"body":{"region":"us-east-1","locale":"en-US","interval":300},"time":1700000000})");
        else if (req.find("env.modified") != std::string::npos)
            prime(R"({"time":1700000000})");
        else if (req.find("env.get") != std::string::npos)
            prime(R"({"body":{"region":"us-east-1","locale":"en-US","interval":300,"debug":"false"},"time":1700000000})");
        else
            prime("{}");
    }
};

int main() {
    CannedHal hal;
    note::StreamingTransport transport{hal};
    note::Notecard nc(transport);
    Api api(nc);

    // ── 1. Single variable ─────────────────────────────────────────────
    // response.text holds the value; response.time is the store's
    // last-modified timestamp.
    std::puts("\n--- single: env.get(\"region\") ---");
    {
        auto r = api.env.get().name("region").execute();
        if (r) {
            std::printf("  region = %.*s (time=%lld)\n",
                        (int)r.text.value().size(), r.text.value().data(),
                        static_cast<long long>(r.time));
        }
    }

    // ── 2. Multiple variables → struct ─────────────────────────────────
    // `.names({...})` returns the ArrayField (not the request), so set
    // names on the request object directly, then chain .into/.execute.
    std::puts("\n--- multi: env.get + names, .into(cfg) ---");
    {
        DeviceConfig cfg{};
        auto req = api.env.get();
        req.names = {"region", "locale", "interval"};
        auto r = req.into(cfg).execute();
        if (r) {
            std::printf("  region=%.*s locale=%.*s interval=%lld\n",
                        (int)cfg.region.size(), cfg.region.data(),
                        (int)cfg.locale.size(), cfg.locale.data(),
                        static_cast<long long>(cfg.interval));
        }
    }

    // ── 3. All variables → struct ──────────────────────────────────────
    // No names list — the Notecard returns every env var it knows about.
    // Struct fields with matching names get filled; extras are ignored.
    std::puts("\n--- all: env.get().into(cfg) ---");
    {
        DeviceConfig cfg{};
        auto r = api.env.get().into(cfg).execute();
        if (r) {
            std::printf("  region=%.*s locale=%.*s interval=%lld\n",
                        (int)cfg.region.size(), cfg.region.data(),
                        (int)cfg.locale.size(), cfg.locale.data(),
                        static_cast<long long>(cfg.interval));
        }
    }

    // ── 4. Conditional: only if changed since last poll ────────────────
    std::puts("\n--- conditional: .time(last_seen) + into(cfg) ---");
    {
        DeviceConfig cfg{};
        note::json_int_t last_seen = 1699999999;
        auto req = api.env.get();
        req.names = {"region", "locale", "interval"};
        req.time = last_seen;
        auto r = req.into(cfg).execute();
        if (r) {
            if (static_cast<long long>(r.time) > last_seen) {
                std::printf("  changed — region=%.*s interval=%lld\n",
                            (int)cfg.region.size(), cfg.region.data(),
                            static_cast<long long>(cfg.interval));
            } else {
                std::puts("  no change since last poll");
            }
        }
    }

    // ── Related: setting variables and cheap change detection ──────────
    //   env.default  — host-side fallback (used only if unset elsewhere)
    //   env.set      — host-side authoritative value (overrides Notehub)
    //   env.modified — just the last-changed timestamp (no body parse)
    std::puts("\n--- env.default / env.set / env.modified ---");
    api.env.setDefault("interval", "300").execute();
    api.env.set("debug").text("true").execute();
    if (auto r = api.env.modified().execute(); r) {
        std::printf("  env store last modified: %lld\n",
                    static_cast<long long>(r.time));
    }

    std::puts("\nAll env.get patterns demonstrated.");
    return 0;
}
