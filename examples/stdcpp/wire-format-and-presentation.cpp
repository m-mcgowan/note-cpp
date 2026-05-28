// Wire format × response presentation — walk the 2×2 matrix.
//
// note-cpp exposes two orthogonal axes for how requests cross the wire:
//
//   1. Wire format: JSON text or JSONB binary. Compile-time choice
//      (`-DNOTE_JSONB=1`). Selected by the transport; the typed API is
//      identical on both.
//
//   2. Response presentation: streaming (SAX → typed Response struct +
//      .into(T&)) or tree (a JsonBackend builds a walkable JsonReader).
//      Runtime choice, made by which Notecard constructor you call.
//
// This example runs the same `card.version` call against both
// presentation modes for whichever wire format the build selected.
// Recompile with `-DNOTE_JSONB=1` to see the JSONB cells; the demo
// code does not change.
//
// Build & run:
//   c++ -std=c++20 -I include examples/stdcpp/wire-format-and-presentation.cpp \
//       && ./a.out
//
// JSONB build:
//   c++ -std=c++20 -DNOTE_JSONB=1 -I include \
//       examples/stdcpp/wire-format-and-presentation.cpp && ./a.out
//
// The transport's request/response bytes are printed in hex so you can
// see the format difference on the wire.

#include <note/note.hpp>
#include <note/jsonb.hpp>
#include <note/link/cobs.hpp>
#include <note/backends/buffer.hpp>   // StaticJsonBackend (zero-heap tree mode)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>

using namespace note;

// ── A wire-format-aware mock HAL ────────────────────────────────────────────
//
// Captures transmitted bytes (logs them in hex) and serves a canned
// `card.version` response in whichever wire format the build selected.
// Under NOTE_JSONB, the canned response is built with the same
// StreamingJsonbBuilder + CobsStreamWriter primitives the library
// itself uses, so what the example demos is what the library produces.
class WireMockHal : public note::Hal {
public:
    std::deque<uint8_t> rx;
    std::string tx_hex;

    void prime_card_version() {
        rx.clear();
#if NOTE_JSONB
        // Build {:<COBS opcodes>:}\n with kBeginObject + "version" +
        // "device" + kEndObject. The on-wire response shape.
        std::string framed;
        framed.append("{:");
        struct StringWriter : note::JsonWriter {
            std::string& s;
            explicit StringWriter(std::string& ss) : s(ss) {}
            bool write(const char* d, size_t n) override {
                s.append(d, n);
                return true;
            }
        } w{framed};
        {
            note::CobsStreamWriter cobs(w, note::jsonb::kCobsXor);
            note::StreamingJsonbBuilder b(cobs);
            b.add("version", note::string_view("notecard-7.5.1"));
            b.add("device", note::string_view("dev:nc-example"));
            cobs.write(reinterpret_cast<const char*>(&note::jsonb::kEndObject), 1);
            cobs.flush();
        }
        framed.append(":}\n");
        for (char c : framed) rx.push_back(static_cast<uint8_t>(c));
#else
        const char* json = R"({"version":"notecard-7.5.1","device":"dev:nc-example"})";
        while (*json) rx.push_back(static_cast<uint8_t>(*json++));
        rx.push_back('\n');
#endif
    }

    bool transmit(const uint8_t* data, size_t len) override {
        char hex[4];
        for (size_t i = 0; i < len; ++i) {
            std::snprintf(hex, sizeof(hex), "%02x ", data[i]);
            tx_hex.append(hex);
        }
        return true;
    }

    bool write_line_terminator() override {
        if (!tx_hex.empty()) {
            std::printf("  >> wire request: %s\n", tx_hex.c_str());
            tx_hex.clear();
        }
        prime_card_version();   // freshly queue the response per request
        return true;
    }

    note::Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t) override {
        if (rx.empty())
            return note::make_error(note::Error::ResponseLost,
                                    note::Cause::Timeout, "no data");
        size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
        return n;
    }

    bool reset() override { return true; }
    void delay(uint32_t) override {}
    uint32_t millis() override { return 0; }
};

// ── The demo — wire-format and presentation agnostic ──────────────────────
//
// Same body runs in every cell of the matrix. The only thing the user
// changes between configurations is the Notecard ctor; the call site
// is unchanged.
template<typename ApiT>
void demo(ApiT& api) {
    auto r = api.card.version().execute();
    if (!r) {
        std::printf("  !! execute failed: %s\n", note::to_string(r.error()).c_str());
        return;
    }
    std::printf("  version=%s device=%s\n",
                std::string(r.version.data(), r.version.size()).c_str(),
                std::string(r.device.data(), r.device.size()).c_str());

    // Tree-only: r.body() is a walkable JsonReader pointer. In streaming
    // mode it returns nullptr; here we just report which path ran.
    if (r.was_streaming_parse()) {
        std::puts("  presentation: streaming (SAX → typed Response struct)");
    } else {
        std::puts("  presentation: tree (JsonBackend assembled a JsonReader)");
        if (auto* body = r.body()) {
            std::printf("  r.body() returned a walkable reader at %p\n",
                        static_cast<const void*>(body));
        }
    }
}

int main() {
#if NOTE_JSONB
    std::puts("=== Wire format: JSONB (NOTE_JSONB=1) ===");
#else
    std::puts("=== Wire format: JSON (default) ===");
    std::puts("    Rebuild with -DNOTE_JSONB=1 to see the JSONB cells.");
#endif

    WireMockHal hal;
    Protocol    transport{hal};

    std::puts("\n--- Cell 1: streaming presentation (no backend linked) ---");
    {
        Notecard nc(transport);
        Api      api(nc);
        demo(api);
    }

    std::puts("\n--- Cell 2: tree presentation (StaticJsonBackend) ---");
    {
        backends::StaticJsonBackend<512, 64> backend;
        Notecard nc(backend, transport);
        Api      api(nc);
        demo(api);
    }

    std::puts("\nThe demo() body is identical in every cell. The wire");
    std::puts("format is decided by NOTE_JSONB at compile time; the");
    std::puts("presentation by which Notecard ctor you call at runtime.");
    return 0;
}
