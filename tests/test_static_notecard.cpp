// Tests for StaticNotecard — verifies that Api<StaticNotecard> correctly
// wires execute() and command() through to the transport.
//
// This catches the null-function-pointer bug where Api only set
// execute_fn_/command_fn_ for NcT=Notecard, leaving them null for
// StaticNotecard.

#include <note/static_notecard.hpp>
#include <note/api.hpp>
#include <note/arena.hpp>
#include <note/lexer/parse.hpp>

#include <doctest.h>
#include <deque>
#include <string>

using namespace note;

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
        last_request += '}';
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
#if !NOTE_JSONB
    // Under NOTE_JSONB the mock captures JSONB opcodes, not JSON text —
    // substring assertions on the captured bytes don't apply there.
    CHECK(nc.stack().transport.last_request.find("\"req\":\"card.temp\"")
          != std::string::npos);
#endif
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
#if !NOTE_JSONB
    CHECK(nc.stack().transport.last_request.find("\"req\":\"hub.set\"")
          != std::string::npos);
    CHECK(nc.stack().transport.last_request.find("\"product\":\"com.example.test\"")
          != std::string::npos);
#endif
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
#if !NOTE_JSONB
    CHECK(nc.stack().transport.last_request.find("\"cmd\":\"hub.set\"")
          != std::string::npos);
#endif
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
