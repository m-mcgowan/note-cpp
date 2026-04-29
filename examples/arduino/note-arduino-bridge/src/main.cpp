// note-arduino + note-cpp coexistence on Arduino.
//
// Use this pattern when an existing project already uses
// [note-arduino](https://github.com/blues/note-arduino) and you want to
// adopt note-cpp's typed API incrementally — without ripping out the
// existing transport setup or replacing every J*-based call site at once.
//
// note-arduino's `Notecard.begin()` initialises note-c's HAL (Serial1
// or I2C, error functions, malloc/free). After that,
// `NoteRequestResponseJSON()` works against the configured bus. Both
// note-arduino's J*-shaped calls and note-cpp's typed Api end up at the
// same C function, so they share one Notecard without conflict.
//
// The bridge below mirrors `examples/stdcpp/note-c-bridge.cpp` — same
// code, just sitting inside an Arduino sketch this time. In a real
// project, link against note-arduino (which pulls note-c) and delete
// the `// stub:` block. The CI build replaces note-arduino with a
// header-only stub so the example compiles without the library
// installed.

#include <Arduino.h>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

// Disable note.hpp's blanket `using namespace note;` so that
// note-arduino's global `Notecard` and note-cpp's `note::Notecard`
// stay distinct. This is the key gotcha — note-arduino lives in the
// global namespace, note-cpp scopes its symbols under `note::`.
#define NOTE_USING_NAMESPACE 0
#include <note/api.hpp>
#include <note/notecard.hpp>
#include <note/transact.hpp>

// ─────────────────────────────────────────────────────────────────────
// stub: note-arduino — replace with `#include <Notecard.h>` in a real
// project. The stub mirrors the surface a coexistence example uses
// (Notecard::begin, plus the underlying NoteRequestResponseJSON entry
// point both libraries route through).
// ─────────────────────────────────────────────────────────────────────
class Notecard {
public:
    void begin(HardwareSerial&, uint32_t /*baud*/) {
        // Real note-arduino: configures note-c's HAL via NoteSetFn*
        // (malloc/free, debug, serial transmit/receive). After this,
        // NoteRequestResponseJSON works against the configured bus.
    }
};

extern "C" char* NoteRequestResponseJSON(const char* /*reqJSON*/) {
    // Real note-c: serialises the request, sends it over the
    // configured transport, parses the response into a fresh
    // heap-allocated null-terminated string the caller must free.
    char* rsp = static_cast<char*>(std::malloc(3));
    std::strcpy(rsp, "{}");
    return rsp;
}
// ─────────────────────────────────────────────────────────────────────
// end stub
// ─────────────────────────────────────────────────────────────────────

// Minimal JSON backend — note-cpp needs a backend to build requests
// and parse responses. In a real project pick whichever JSON library
// you already use (cJSON, nlohmann, etc.) and wrap it in JsonBackend;
// see tests/integration/ for working examples.
struct MockBuilder : note::JsonBuilder {
    using JsonBuilder::add;
    std::string buf_ = "{";
    bool first_ = true;
    void sep() { if (!first_) buf_ += ','; first_ = false; }
    MockBuilder& add(note::string_view k, bool v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += v ? "true" : "false"; return *this;
    }
    MockBuilder& add(note::string_view k, note::json_int_t v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += std::to_string(v); return *this;
    }
    MockBuilder& add(note::string_view k, double v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += std::to_string(v); return *this;
    }
    MockBuilder& add(note::string_view k, note::string_view v) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":\""; buf_ += v; buf_ += '"'; return *this;
    }
    MockBuilder& add_raw(note::string_view k, note::string_view fragment) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":"; buf_ += fragment; return *this;
    }
    MockBuilder& begin_object(note::string_view k) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":{"; first_ = true; return *this;
    }
    MockBuilder& end_object() override { buf_ += '}'; first_ = false; return *this; }
    MockBuilder& begin_array(note::string_view k) override {
        sep(); buf_ += '"'; buf_ += k; buf_ += "\":["; first_ = true; return *this;
    }
    MockBuilder& end_array() override { buf_ += ']'; first_ = false; return *this; }
    note::string_view to_view() override { buf_ += '}'; return buf_; }
};
struct MockReader : note::JsonReader {
    bool has(note::string_view) const override { return false; }
    bool get_bool(note::string_view, bool d) const override { return d; }
    note::json_int_t get_int(note::string_view, note::json_int_t d) const override { return d; }
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

// Bridge: routes note-cpp transactions through note-c's request/response
// entry point, the same one note-arduino's J*-shaped methods call into.
class NoteCTransport : public note::ITransact {
    std::string scratch_;
public:
    using note::ITransact::transact;
    using note::ITransact::send;

    note::Result<note::string_view> transact(note::string_view req,
                                             note::span<char> buf,
                                             uint32_t /*timeout_ms*/) override {
        // NoteRequestResponseJSON expects null-terminated input.
        scratch_.assign(req.data(), req.size());
        char* rsp = NoteRequestResponseJSON(scratch_.c_str());
        if (rsp == nullptr)
            return note::make_error(note::Error::ResponseLost, NOTE_ERR("no response"));
        size_t rsp_len = std::strlen(rsp);
        if (rsp_len >= buf.size()) {
            std::free(rsp);
            return note::make_error(note::Error::Overflow, NOTE_ERR("response exceeds buffer"));
        }
        std::memcpy(buf.data(), rsp, rsp_len);
        std::free(rsp);
        return note::string_view(buf.data(), rsp_len);
    }

    note::Result<void> send(note::string_view req) override {
        scratch_.assign(req.data(), req.size());
        char* rsp = NoteRequestResponseJSON(scratch_.c_str());
        if (rsp != nullptr) std::free(rsp);
        return {};
    }

    void reset() override {}
    void abort() override {}

    // Hal stub — note-c (via note-arduino) owns the actual hardware,
    // so this is purely a placeholder so the inherited Notecard timing
    // path has something safe to call.
    struct NoopHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return note::Result<size_t>{size_t{0}}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return ::millis(); }
        void delay(uint32_t ms) override { ::delay(ms); }
    } hal_;
    note::Hal& hal() override { return hal_; }
};

// note-arduino global — the existing project's entry point.
Notecard notecard;

// note-cpp bridge — sits on top of the same C runtime.
MockBackend cpp_backend;
NoteCTransport bridge;
note::Notecard cpp_nc(cpp_backend, bridge);
note::Api api(cpp_nc);

void setup() {
    Serial.begin(115200);

    // note-arduino setup — initialises note-c's HAL.
    notecard.begin(Serial1, 9600);

    // From here both APIs work against the same Notecard.

    // ─── note-arduino style ──────────────────────────────────────────
    // Existing code keeps working. In a real project this would be
    // notecard.newRequest("card.version") + JAddStringToObject(req, ...)
    // + notecard.sendRequest(req); for brevity (and to avoid stubbing
    // the J* surface) we call the underlying C function directly.
    char* rsp = NoteRequestResponseJSON(R"({"req":"card.version"})");
    if (rsp != nullptr) {
        Serial.println(rsp);
        std::free(rsp);
    }

    // ─── note-cpp style ──────────────────────────────────────────────
    // Same Notecard, same wire, typed at compile time.
    api.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .execute();
}

void loop() {}
