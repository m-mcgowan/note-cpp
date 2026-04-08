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

#include "catch.hpp"
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

    void queue_response(const std::string& json) {
        for (char c : json) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back('\n');
    }

    // Non-template transact_dispatch — matches what StaticNotecard calls
    Result<void> transact_dispatch(BuildFn build_fn, void* ctx, SaxDispatch dispatch,
                                   uint32_t /*timeout_ms*/, detail::NcErrorCapture& /*nc_err*/) {
        ++transact_count;

        // Capture the built request JSON
        last_request.clear();
        struct Writer : JsonWriter {
            std::string& out;
            explicit Writer(std::string& o) : out(o) {}
            bool write(const char* data, size_t len) override {
                out.append(data, len);
                return true;
            }
        } writer(last_request);
        StreamingJsonBuilder builder(writer);
        build_fn(builder, ctx);
        last_request += '}';

        // Feed queued response through dispatch
        if (!rx.empty()) {
            std::string resp;
            while (!rx.empty() && rx.front() != '\n') {
                resp += static_cast<char>(rx.front());
                rx.pop_front();
            }
            if (!rx.empty()) rx.pop_front(); // consume \n

            // Parse response via SaxDispatch
            char storage[384];
            SaxStreamBuf buf(storage);
            SaxAdapter adapter(buf, dispatch);
            DefaultLexer lexer;
            detail::lex_feed_loop(lexer, reinterpret_cast<const uint8_t*>(resp.data()),
                                  resp.size(), adapter);
        }
        return {};
    }

    Result<void> send(BuildFn build_fn, void* ctx) {
        ++send_count;
        last_request.clear();
        struct Writer : JsonWriter {
            std::string& out;
            explicit Writer(std::string& o) : out(o) {}
            bool write(const char* data, size_t len) override {
                out.append(data, len);
                return true;
            }
        } writer(last_request);
        StreamingJsonBuilder builder(writer);
        build_fn(builder, ctx);
        last_request += '}';
        return {};
    }

    bool reset() { return true; }
    uint32_t millis() { return 0; }
    void delay(uint32_t) {}
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
    CHECK(nc.stack().transport.last_request.find("\"req\":\"card.temp\"")
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
    CHECK(nc.stack().transport.last_request.find("\"req\":\"hub.set\"")
          != std::string::npos);
    CHECK(nc.stack().transport.last_request.find("\"product\":\"com.example.test\"")
          != std::string::npos);
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
    CHECK(nc.stack().transport.last_request.find("\"cmd\":\"hub.set\"")
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
