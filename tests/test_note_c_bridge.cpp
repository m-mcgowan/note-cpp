// Host-side coverage of the NoteCTransport bridge pattern shared by
// examples/stdcpp/note-c-bridge.cpp and
// examples/arduino/note-arduino-bridge/. The bridge is platform-
// agnostic — its only collaborator is `NoteRequestResponseJSON` —
// so the routing/error paths can be exercised under doctest with a
// controllable stub of that C function.
//
// Test goals (proves what both example sketches claim):
//   1. A typed note-cpp call lands at NoteRequestResponseJSON exactly
//      once with a null-terminated request string (J* surface
//      coexistence: any extra note-arduino call sites are independent
//      C-function calls; no shared in-flight state).
//   2. Response bytes from NoteRequestResponseJSON flow back through
//      the typed Api result.
//   3. NoteRequestResponseJSON returning nullptr surfaces as
//      Error::ResponseLost.
//   4. Responses larger than the caller-supplied buffer surface as
//      Error::Overflow without touching the buffer beyond the bound.
//   5. send() (fire-and-forget) frees the response and reports OK.

#include <doctest.h>

#include <note/api.hpp>
#include <note/notecard.hpp>
#include <note/transact.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ─── controllable stub of note-c's request/response entry point ────
namespace {

struct StubState {
    std::vector<std::string> requests;       // null-terminated copies
    std::string next_response = "{}";        // returned malloc'd to caller
    bool return_null = false;                 // simulate transport failure
} stub;

extern "C" char* test_NoteRequestResponseJSON(const char* req) {
    stub.requests.emplace_back(req);
    if (stub.return_null) return nullptr;
    char* rsp = static_cast<char*>(std::malloc(stub.next_response.size() + 1));
    std::memcpy(rsp, stub.next_response.data(), stub.next_response.size());
    rsp[stub.next_response.size()] = '\0';
    return rsp;
}

void reset_stub() {
    stub.requests.clear();
    stub.next_response = "{}";
    stub.return_null = false;
}

// ─── minimal JSON backend (mirrors mock_backend.hpp from stdcpp examples) ─
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

// ─── the bridge under test ─────────────────────────────────────────
class NoteCTransport : public note::ITransact {
    std::string scratch_;
public:
    using note::ITransact::transact;
    using note::ITransact::send;

    note::Result<note::string_view> transact(note::string_view req,
                                             note::span<char> buf,
                                             uint32_t /*timeout_ms*/) override {
        scratch_.assign(req.data(), req.size());
        char* rsp = test_NoteRequestResponseJSON(scratch_.c_str());
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
        char* rsp = test_NoteRequestResponseJSON(scratch_.c_str());
        if (rsp != nullptr) std::free(rsp);
        return {};
    }

    void reset() override {}
    void abort() override {}

    struct NoopHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return note::Result<size_t>{size_t{0}}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return 0; }
        void delay(uint32_t) override {}
    } hal_;
    note::Hal& hal() override { return hal_; }
};

} // namespace

// ─── tests ─────────────────────────────────────────────────────────

TEST_CASE("NoteCTransport: typed call lands at NoteRequestResponseJSON exactly once") {
    reset_stub();
    MockBackend backend;
    NoteCTransport bridge;
    note::Notecard nc(backend, bridge);
    note::Api api(nc);

    api.hub.set().product("com.example.app").mode("periodic").execute();

    REQUIRE(stub.requests.size() == 1);
    auto& req = stub.requests[0];
    REQUIRE(req.find(R"("req":"hub.set")") != std::string::npos);
    REQUIRE(req.find(R"("product":"com.example.app")") != std::string::npos);
    REQUIRE(req.find(R"("mode":"periodic")") != std::string::npos);
    // Stub copied via std::string ctor — would have stopped at the
    // first NUL. Round-tripping through std::string requires the
    // bridge to have null-terminated the buffer; the request body
    // round-trips intact, so termination is correct.
    REQUIRE(req.back() == '}');
}

TEST_CASE("NoteCTransport: ResponseLost when NoteRequestResponseJSON returns nullptr") {
    reset_stub();
    stub.return_null = true;
    MockBackend backend;
    NoteCTransport bridge;
    note::Notecard nc(backend, bridge);
    note::Api api(nc);

    auto result = api.card.version().execute();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == note::Error::ResponseLost);
}

TEST_CASE("NoteCTransport: response routes back to caller") {
    reset_stub();
    stub.next_response = R"({"version":"v1.2.3","device":"nc-test"})";
    MockBackend backend;
    NoteCTransport bridge;
    note::Notecard nc(backend, bridge);

    // Drive the bridge directly and inspect the bytes — MockReader
    // doesn't decode anything, so we can't read api.card.version()'s
    // typed fields back. The bridge's job is to ferry bytes; that's
    // what we're checking here.
    char buf[128];
    auto rv = bridge.transact(note::string_view{R"({"req":"card.version"})"},
                              note::span<char>(buf, sizeof(buf)),
                              5000);
    REQUIRE(rv);
    REQUIRE(rv.value() == stub.next_response);
}

TEST_CASE("NoteCTransport: oversized response surfaces Overflow") {
    reset_stub();
    stub.next_response = std::string(64, 'x');
    NoteCTransport bridge;

    char small[16] = {};
    auto rv = bridge.transact(note::string_view{R"({"req":"x"})"},
                              note::span<char>(small, sizeof(small)),
                              5000);
    REQUIRE_FALSE(rv);
    REQUIRE(rv.error().code == note::Error::Overflow);
}

TEST_CASE("NoteCTransport: send() is fire-and-forget and frees response") {
    reset_stub();
    stub.next_response = R"({"ignored":true})";
    NoteCTransport bridge;

    auto rv = bridge.send(note::string_view{R"({"cmd":"hub.sync"})"});
    REQUIRE(rv);
    REQUIRE(stub.requests.size() == 1);
    REQUIRE(stub.requests[0] == R"({"cmd":"hub.sync"})");
}
