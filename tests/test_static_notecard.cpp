// Tests for StaticNotecard — verifies that Api<StaticNotecard> correctly
// wires execute() and command() through to the transport.
//
// This catches the null-function-pointer bug where Api only set
// execute_fn_/command_fn_ for NcT=Notecard, leaving them null for
// StaticNotecard.

#include <note/static_notecard.hpp>
#include <note/api.hpp>
#include <note/arena.hpp>
#include <note/json_buf.hpp>
#include <note/json_view.hpp>
#include <note/lexer/parse.hpp>

#if NOTE_JSONB
#include <note/jsonb.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/lexer/sax_adapter.hpp>
#include <cstring>
#include <vector>
#endif

#include <doctest.h>
#include <cstring>
#include <deque>
#include <string>

using namespace note;

#if NOTE_JSONB
namespace {

/// Decode a captured JSONB opcode stream back to JSON text for substring
/// assertions. The mock under NOTE_JSONB captures whatever
/// RequestSource::emit() paints — that's raw JSONB opcodes (the protocol
/// layer's CobsStreamWriter is not in the path). This helper feeds those
/// opcodes through JsonbParser → a JSON-emitting SAX sink to produce the
/// equivalent compact JSON text. Same substring assertions then work
/// unchanged across NOTE_MINIMAL (JSONB) and the default (JSON) builds.
inline std::string jsonb_to_json(const std::string& bytes) {
    struct MemReader {
        const uint8_t* data;
        size_t pos;
        size_t end;
        Result<size_t> operator()(uint8_t* buf, size_t max, uint32_t) {
            const size_t avail = end - pos;
            if (avail == 0) return size_t{0};
            const size_t n = max < avail ? max : avail;
            std::memcpy(buf, data + pos, n);
            pos += n;
            return n;
        }
    } reader{
        reinterpret_cast<const uint8_t*>(bytes.data()), 0, bytes.size()
    };

    struct JsonEmitter {
        std::string out;
        std::vector<bool> first_stack;

        void sep_and_key(string_view k) {
            if (!first_stack.empty()) {
                if (first_stack.back()) first_stack.back() = false;
                else out += ',';
            }
            if (!k.empty()) {
                out += '"';
                out.append(k.data(), k.size());
                out += "\":";
            }
        }
        void on_object_begin(string_view k) { sep_and_key(k); out += '{'; first_stack.push_back(true); }
        void on_object_end  (string_view)   { out += '}'; if (!first_stack.empty()) first_stack.pop_back(); }
        void on_array_begin (string_view k) { sep_and_key(k); out += '['; first_stack.push_back(true); }
        void on_array_end   (string_view)   { out += ']'; if (!first_stack.empty()) first_stack.pop_back(); }
        void on_bool        (string_view k, bool v) { sep_and_key(k); out += v ? "true" : "false"; }
        void on_int         (string_view k, json_int_t v) { sep_and_key(k); out += std::to_string(v); }
        void on_float       (string_view k, double v) {
            sep_and_key(k);
            char b[32];
            std::snprintf(b, sizeof b, "%.6g", v);
            out += b;
        }
        void on_string      (string_view k, string_view v) {
            sep_and_key(k);
            out += '"';
            out.append(v.data(), v.size());
            out += '"';
        }
        void on_null        (string_view k) { sep_and_key(k); out += "null"; }
        void reset          ()              { out.clear(); first_stack.clear(); }
    } emitter;

    char scratch[256];
    SaxStreamBuf buf(scratch);
    auto dispatch = make_sax_dispatch(emitter);
    jsonb_parse_streaming(reader, 1000, buf, dispatch);
    return emitter.out;
}

} // namespace
#endif

// ---------------------------------------------------------------------------
// Minimal mock transport stack for StaticNotecard<Stack>
// ---------------------------------------------------------------------------

struct MockTransport {
    std::deque<uint8_t> rx;
    std::string last_request;
    int transact_count = 0;
    int send_count = 0;

    // Error injection
    Error next_error = Error::NoError;
    int fail_count = 0;

    // Timing
    uint32_t now_ms = 0;
    uint32_t total_delay_ms = 0;
    int reset_count = 0;

    void queue_response(const std::string& json) {
        for (char c : json) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back('\n');
    }

    void capture_request(RequestSource src) {
        last_request.clear();
        struct Writer : JsonWriter {
            std::string& out;
            explicit Writer(std::string& o) : out(o) {}
            bool write(const char* data, size_t len) override {
                out.append(data, len);
                return true;
            }
        } writer(last_request);
        src.emit(writer);
        // RequestSource emits the opening container only — Protocol
        // appends the closer on the wire. The mock substitutes for
        // Protocol here; closer matches the active wire format so
        // last_request is a self-contained, parseable representation
        // in either mode.
#if NOTE_JSONB
        last_request += static_cast<char>(jsonb::kEndObject);
#else
        last_request += '}';
#endif
    }

    /// Captured request as JSON text, regardless of wire format. Under
    /// NOTE_JSONB the captured opcode stream is decoded back to JSON via
    /// the JSONB parser; in the default build last_request already is
    /// JSON. Lets substring assertions be wire-format-agnostic.
    std::string last_request_as_json() const {
#if NOTE_JSONB
        return jsonb_to_json(last_request);
#else
        return last_request;
#endif
    }

    void feed_response(SaxDispatch dispatch) {
        if (rx.empty()) return;
        std::string resp;
        while (!rx.empty() && rx.front() != '\n') {
            resp += static_cast<char>(rx.front());
            rx.pop_front();
        }
        if (!rx.empty()) rx.pop_front();
        char storage[384];
        SaxStreamBuf buf(storage);
        SaxAdapter adapter(buf, dispatch);
        DefaultLexer lexer;
        detail::lex_feed_loop(lexer, reinterpret_cast<const uint8_t*>(resp.data()),
                              resp.size(), adapter);
    }

    Result<void> transact_dispatch(RequestSource src, SaxDispatch dispatch,
                                   uint32_t /*timeout_ms*/, detail::NcErrorCapture& /*nc_err*/) {
        ++transact_count;
        capture_request(src);
        if (fail_count > 0) {
            --fail_count;
            return make_error(next_error, Cause::HalError, "mock failure");
        }
        feed_response(dispatch);
        return {};
    }

    // Raw passthrough used by StaticNotecard::transact_raw() forwarders.
    // Captures the request as plain text and replays the queued response
    // line into the caller's buffer (same semantics as Protocol::transact_raw).
    int transact_raw_count = 0;
    Result<string_view> transact_raw(string_view request, char* buf, size_t bufsize,
                                     uint32_t /*timeout_ms*/) {
        ++transact_raw_count;
        last_request = std::string(request);
        if (rx.empty())
            return make_error(Error::ResponseLost, Cause::HalError, "no queued response");
        std::string resp;
        while (!rx.empty() && rx.front() != '\n') {
            resp += static_cast<char>(rx.front());
            rx.pop_front();
        }
        if (!rx.empty()) rx.pop_front();
        if (resp.size() > bufsize)
            return make_error(Error::Overflow, Cause::HalError, "response too large");
        std::memcpy(buf, resp.data(), resp.size());
        return string_view(buf, resp.size());
    }

    Result<void> send(RequestSource src) {
        ++send_count;
        capture_request(src);
        return {};
    }

    bool reset() { ++reset_count; return true; }
    uint32_t millis() { return now_ms; }
    void delay(uint32_t ms) { now_ms += ms; total_delay_ms += ms; }

    /// MockTransport doubles as its own Hal — millis/delay/reset already
    /// satisfy the duck-typed StaticNotecard hal() contract.
    MockTransport& hal() { return *this; }
};

/// Stack that owns the mock transport (matches the pattern of SerialTransportStack).
struct MockStack {
    MockTransport transport;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("StaticNotecard execute sends request and parses response") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    Api api(nc);

    // Queue a response for card.temp
    nc.stack().transport.queue_response("{\"value\":22.5}");

    auto result = api.card.temp().read().execute();
    REQUIRE(result);
    CHECK(result.value == 22.5);
    CHECK(nc.stack().transport.transact_count == 1);
    CHECK(nc.stack().transport.last_request_as_json().find("\"req\":\"card.temp\"")
          != std::string::npos);
}

TEST_CASE("StaticNotecard execute with void response") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    Api api(nc);

    nc.stack().transport.queue_response("{}");

    auto result = api.hub.set()
        .product("com.example.test")
        .mode("periodic")
        .execute();
    REQUIRE(result);
    CHECK(nc.stack().transport.transact_count == 1);
    auto req = nc.stack().transport.last_request_as_json();
    CHECK(req.find("\"req\":\"hub.set\"") != std::string::npos);
    CHECK(req.find("\"product\":\"com.example.test\"") != std::string::npos);
}

TEST_CASE("StaticNotecard command sends fire-and-forget") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    Api api(nc);

    auto result = api.hub.set()
        .product("com.example.test")
        .command();
    REQUIRE(result);
    CHECK(nc.stack().transport.send_count == 1);
    CHECK(nc.stack().transport.last_request_as_json().find("\"cmd\":\"hub.set\"")
          != std::string::npos);
}

TEST_CASE("StaticNotecard via resource group factory") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    Api api(nc);

    nc.stack().transport.queue_response("{\"value\":22.5}");

    // card.temp uses a factory (CardTempFactory) — same bug path
    auto temp = api.card.temp().read().execute();
    REQUIRE(temp);
    CHECK(temp.value == 22.5);
}

// ---------------------------------------------------------------------------
// Error paths
// ---------------------------------------------------------------------------

#if !NOTE_NO_RETRY

TEST_CASE("StaticNotecard: transport error propagates") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    nc.set_retry_policy(RetryPolicy{0, 0, 0});
    Api api(nc);

    nc.stack().transport.fail_count = 1;
    nc.stack().transport.next_error = Error::SendFailed;

    auto result = api.card.temp().read().execute();
    CHECK(!result);
    CHECK(result.error().code == Error::SendFailed);
}

TEST_CASE("StaticNotecard: void response transport error propagates") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    nc.set_retry_policy(RetryPolicy{0, 0, 0});
    Api api(nc);

    nc.stack().transport.fail_count = 1;
    nc.stack().transport.next_error = Error::SendFailed;

    auto result = api.hub.set().product("test").execute();
    CHECK(!result);
    CHECK(result.error().code == Error::SendFailed);
}

// ---------------------------------------------------------------------------
// Retry logic
// ---------------------------------------------------------------------------

TEST_CASE("StaticNotecard: retry on SendFailed succeeds on second try") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    nc.set_retry_policy(RetryPolicy{.max_retries = 2, .retry_delay_ms = 0, .timeout_ms = 0});
    Api api(nc);

    nc.stack().transport.fail_count = 1;
    nc.stack().transport.next_error = Error::SendFailed;
    nc.stack().transport.queue_response("{\"value\":22.5}");

    auto result = api.card.temp().read().execute();
    CHECK(result);
    CHECK(nc.stack().transport.transact_count == 2);
    CHECK(nc.stack().transport.reset_count == 1);
}

TEST_CASE("StaticNotecard: no retry on non-retryable error") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    nc.set_retry_policy(RetryPolicy{.max_retries = 5, .retry_delay_ms = 0, .timeout_ms = 0});
    Api api(nc);

    nc.stack().transport.fail_count = 99;
    nc.stack().transport.next_error = Error::Json;

    auto result = api.card.temp().read().execute();
    CHECK(!result);
    CHECK(nc.stack().transport.transact_count == 1);
}

// ---------------------------------------------------------------------------
// Timing enforcement
// ---------------------------------------------------------------------------

TEST_CASE("StaticNotecard: inter-transaction gap enforced") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    nc.set_retry_policy(RetryPolicy{0, 0, 0});
    nc.set_inter_transaction_gap(50);
    Api api(nc);

    // First transaction — no delay
    nc.stack().transport.queue_response("{\"value\":1.0}");
    api.card.temp().read().execute();
    CHECK(nc.stack().transport.total_delay_ms == 0);

    // Second transaction immediately after — should delay
    nc.stack().transport.queue_response("{\"value\":2.0}");
    api.card.temp().read().execute();
    CHECK(nc.stack().transport.total_delay_ms == 50);
}

TEST_CASE("StaticNotecard: timing enforced for commands") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    nc.set_retry_policy(RetryPolicy{0, 0, 0});
    nc.set_inter_transaction_gap(30);
    Api api(nc);

    // First command
    api.hub.set().product("test").command();
    // Second command immediately — should delay
    api.hub.set().product("test").command();
    CHECK(nc.stack().transport.total_delay_ms == 30);
}

#endif // !NOTE_NO_RETRY

// ---------------------------------------------------------------------------
// Request IDs
// ---------------------------------------------------------------------------

#if !NOTE_NO_REQUEST_IDS

TEST_CASE("StaticNotecard: request IDs enabled by default") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    Api api(nc);

    nc.stack().transport.queue_response("{}");
    api.hub.set().product("test").execute();
    CHECK(nc.stack().transport.last_request.find("\"id\":") != std::string::npos);
}

TEST_CASE("StaticNotecard: request IDs disabled") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    nc.set_request_ids(false);
    Api api(nc);

    nc.stack().transport.queue_response("{}");
    api.hub.set().product("test").execute();
    CHECK(nc.stack().transport.last_request.find("\"id\":") == std::string::npos);
}

TEST_CASE("StaticNotecard: request IDs increment") {
    alignas(4) char arena_buf[256];
    MonotonicArena arena(arena_buf);

    StaticNotecard<MockStack> nc(arena_allocator(arena));
    Api api(nc);

    nc.stack().transport.queue_response("{}");
    api.hub.set().product("test").execute();
    auto req1 = nc.stack().transport.last_request;

    nc.stack().transport.queue_response("{}");
    api.hub.set().product("test").execute();
    auto req2 = nc.stack().transport.last_request;

    // IDs should differ between calls
    CHECK(req1 != req2);
}

#endif // !NOTE_NO_REQUEST_IDS

// ---------------------------------------------------------------------------
// transact_raw forwarders — collapse the AVR-style raw call site.

TEST_CASE("StaticNotecard::transact_raw(string_view, char*, size_t)") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);
    StaticNotecard<MockStack> nc(arena_allocator(arena));

    nc.stack().transport.queue_response(R"({"value":42.0})");
    char rsp[64];
    auto r = nc.transact_raw(string_view(R"({"req":"card.temp"})"), rsp, sizeof(rsp));
    REQUIRE(r.has_value());
    REQUIRE(*r == R"({"value":42.0})");
    CHECK(nc.stack().transport.transact_raw_count == 1);
    CHECK(nc.stack().transport.last_request == R"({"req":"card.temp"})");
}

TEST_CASE("StaticNotecard::transact_raw forwards .view() (JsonBuf, json<...>)") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);
    StaticNotecard<MockStack> nc(arena_allocator(arena));

    nc.stack().transport.queue_response(R"({"value":99.5})");
    JsonBuf<64> req;
    req.add("req", "card.temp");

    char rsp[64];
    auto r = nc.transact_raw(req, rsp);  // .view() forwarder + char[N] deduction
    REQUIRE(r.has_value());
    REQUIRE(*r == R"({"value":99.5})");
}

TEST_CASE("StaticNotecard::transact_raw composes with note::JsonView unwrap") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);
    StaticNotecard<MockStack> nc(arena_allocator(arena));

    nc.stack().transport.queue_response(R"({"value":3.14})");
    JsonBuf<64> req;
    req.add("req", "card.temp");

    char rsp[64];
    // The Result<string_view>-unwrapping JsonView ctor lets the user skip
    // the explicit r.has_value()/*r dance for best-effort field reads.
    JsonView v(nc.transact_raw(req, rsp));
    CHECK(v.get_float("value", 0.0f) == doctest::Approx(3.14f));
}

TEST_CASE("StaticNotecard::transact_raw default timeout") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);
    StaticNotecard<MockStack> nc(arena_allocator(arena));

    nc.stack().transport.queue_response("{}");
    char rsp[16];
    // No timeout argument — defaults to 10000 ms.
    auto r = nc.transact_raw(string_view(R"({"req":"card.status"})"), rsp);
    REQUIRE(r.has_value());
}

// ---------------------------------------------------------------------------
// transact_raw_inplace — single buffer for both request render and response.

TEST_CASE("StaticNotecard::transact_raw_inplace single-buffer round-trip") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);
    StaticNotecard<MockStack> nc(arena_allocator(arena));

    nc.stack().transport.queue_response(R"({"value":42.0})");
    char buf[64];
    auto r = nc.transact_raw_inplace(buf, [](auto& w) {
        w.add("req", "card.temp");
    });
    REQUIRE(r.has_value());
    REQUIRE(*r == R"({"value":42.0})");
    // Mock recorded the request — confirms the lambda's bytes reached the wire.
    CHECK(nc.stack().transport.transact_raw_count == 1);
    CHECK(nc.stack().transport.last_request == R"({"req":"card.temp"})");
}

TEST_CASE("transact_raw_inplace composes with JsonView") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);
    StaticNotecard<MockStack> nc(arena_allocator(arena));

    nc.stack().transport.queue_response(R"({"value":3.14})");
    char buf[64];
    JsonView v(nc.transact_raw_inplace(buf, [](auto& w) {
        w.add("req", "card.temp");
    }));
    CHECK(v.get_float("value", 0.0f) == doctest::Approx(3.14f));
}

TEST_CASE("transact_raw_inplace nested object renders correctly") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);
    StaticNotecard<MockStack> nc(arena_allocator(arena));

    nc.stack().transport.queue_response("{}");
    char buf[128];
    auto r = nc.transact_raw_inplace(buf, [](auto& w) {
        w.add("req", "note.add");
        w.add("file", "sensors.qo");
        w.begin_object("body");
            w.add("temperature", 22.5);
            w.add("humidity", 60);
        w.end_object();
    });
    REQUIRE(r.has_value());
    CHECK(nc.stack().transport.last_request ==
          R"({"req":"note.add","file":"sensors.qo","body":{"temperature":22.5,"humidity":60}})");
}

TEST_CASE("transact_raw_inplace overflow returns Error::Overflow") {
    alignas(4) char arena_buf[64];
    MonotonicArena arena(arena_buf);
    StaticNotecard<MockStack> nc(arena_allocator(arena));

    char buf[8];  // way too small for the rendered JSON
    auto r = nc.transact_raw_inplace(buf, [](auto& w) {
        w.add("req", "card.temp");  // ~20 bytes — won't fit
    });
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == Error::Overflow);
    // No transport call was made (overflow short-circuits).
    CHECK(nc.stack().transport.transact_raw_count == 0);
}
