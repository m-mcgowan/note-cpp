// Tests for the debug observability system.

#include <doctest.h>
#include <string>
#include "test_json_backend.hpp"
#include "test_notecard_factory.hpp"
#include <note/api.hpp>
#include <note/debug.hpp>

namespace {

struct DebugTracker {
    int wire_count = 0;
    int timing_count = 0;
    int transport_count = 0;
    std::string sent_json;
    std::string received_json;
    std::vector<note::TimingEvent> timing_events;

    note::DebugListener listener() {
        note::DebugListener d;
        d.ctx = this;
        d.on_wire = [](const note::WireEvent& ev, void* ctx) {
            auto* self = static_cast<DebugTracker*>(ctx);
            self->wire_count++;
            if (ev.direction == note::WireDirection::Send)
                self->sent_json = std::string(ev.json);
            else
                self->received_json = std::string(ev.json);
        };
        d.on_timing = [](note::TimingEvent ev, note::string_view, void* ctx) {
            auto* self = static_cast<DebugTracker*>(ctx);
            self->timing_count++;
            self->timing_events.push_back(ev);
        };
        d.on_transport = [](note::TransportEvent, uint32_t, void* ctx) {
            static_cast<DebugTracker*>(ctx)->transport_count++;
        };
        return d;
    }
};

struct Harness {
    note::test::TestJsonBackend backend;
    std::string last_req;
    note::test::CallbackTransport transport;
    note::Notecard nc;

    Harness()
        : transport(
            [this](note::string_view r, uint32_t) -> note::Result<note::string_view> {
                last_req = std::string(r);
                return note::string_view("{}");
            })
        , nc(note::test::make_test_notecard(backend, transport)) {}
};

} // namespace

// ---------------------------------------------------------------------------
// Wire data hooks
// ---------------------------------------------------------------------------

TEST_CASE("Debug: on_wire called with request JSON on execute") {
    Harness h;
    DebugTracker tracker;
    h.nc.set_debug(tracker.listener());

    note::Api api(h.nc);
    api.hub.set().product("com.example").execute();

    REQUIRE(tracker.wire_count >= 1);
    CHECK(tracker.sent_json.find("hub.set") != std::string::npos);
}

TEST_CASE("Debug: on_wire receives both request and response") {
    Harness h;
    DebugTracker tracker;
    h.nc.set_debug(tracker.listener());

    note::Api api(h.nc);
    api.card.version().execute();

    // Should have at least 2 wire events: send + receive
    CHECK(tracker.wire_count >= 2);
}

TEST_CASE("Debug: on_wire shows correct direction") {
    Harness h;

    std::vector<note::WireDirection> directions;
    note::DebugListener d;
    d.ctx = &directions;
    d.on_wire = [](const note::WireEvent& ev, void* ctx) {
        static_cast<std::vector<note::WireDirection>*>(ctx)->push_back(ev.direction);
    };
    h.nc.set_debug(d);

    note::Api api(h.nc);
    api.card.version().execute();

    REQUIRE(directions.size() >= 2);
    CHECK(directions[0] == note::WireDirection::Send);
    CHECK(directions[1] == note::WireDirection::Receive);
}

// ---------------------------------------------------------------------------
// Wire data hooks — streaming path
// ---------------------------------------------------------------------------

TEST_CASE("Debug: on_wire fires on streaming execute") {
    // Streaming harness with a mock HAL
    struct MockHal : note::Hal {
        uint8_t rx[256];
        size_t rx_len = 0;
        size_t rx_pos = 0;
        void queue(const char* s) {
            for (; *s; ++s) rx[rx_len++] = static_cast<uint8_t>(*s);
            rx[rx_len++] = '\r'; rx[rx_len++] = '\n';
        }
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t) override {
            if (rx_pos >= rx_len)
                return note::make_error(note::Error::ResponseLost, note::Cause::Timeout, "");
            size_t n = std::min(max_len, rx_len - rx_pos);
            for (size_t i = 0; i < n; ++i) buf[i] = rx[rx_pos++];
            return n;
        }
        bool reset() override { rx_pos = 0; return true; }
        bool write_line_terminator() override { return true; }
        void delay(uint32_t) override {}
        uint32_t millis() override { return 0; }
    };

    MockHal hal;
    hal.queue(R"({"version":"notecard-test"})");
    note::Protocol transport(static_cast<note::Hal&>(hal));
    auto nc = note::test::make_test_notecard(transport);

    bool saw_send = false;
    note::DebugListener d;
    d.ctx = &saw_send;
    d.on_wire = [](const note::WireEvent& ev, void* ctx) {
        if (ev.direction == note::WireDirection::Send) *static_cast<bool*>(ctx) = true;
    };
    nc.set_debug(d);

    note::Api api(nc);
    api.card.version().execute();

    CHECK(saw_send);
}

// ---------------------------------------------------------------------------
// Timing hooks
// ---------------------------------------------------------------------------

TEST_CASE("Debug: timing events bracket the transaction") {
    Harness h;
    DebugTracker tracker;
    h.nc.set_debug(tracker.listener());

    note::Api api(h.nc);
    api.card.version().execute();

    REQUIRE(!tracker.timing_events.empty());
    CHECK(tracker.timing_events.front() == note::TimingEvent::TransactionBegin);
    CHECK(tracker.timing_events.back() == note::TimingEvent::TransactionEnd);
}

TEST_CASE("Debug: timing events include build phase") {
    Harness h;
    DebugTracker tracker;
    h.nc.set_debug(tracker.listener());

    note::Api api(h.nc);
    api.hub.set().product("test").execute();

    auto& events = tracker.timing_events;
    auto has = [&](note::TimingEvent ev) {
        return std::find(events.begin(), events.end(), ev) != events.end();
    };
    CHECK(has(note::TimingEvent::BuildBegin));
    CHECK(has(note::TimingEvent::BuildEnd));
    CHECK(has(note::TimingEvent::TransactionBegin));
    CHECK(has(note::TimingEvent::TransactionEnd));
}

// ---------------------------------------------------------------------------
// No listener — zero callbacks
// ---------------------------------------------------------------------------

TEST_CASE("Debug: no listener — zero callbacks fired") {
    Harness h;
    // No set_debug() call — default DebugListener, all null

    int counter = 0;
    // Verify no callbacks are fired
    note::Api api(h.nc);
    api.card.version().execute();

    // If we reach here without crash, the null checks worked.
    // Also verify explicitly that the default listener is all null.
    auto& d = h.nc.debug();
    CHECK(d.on_wire == nullptr);
    CHECK(d.on_timing == nullptr);
    CHECK(d.on_transport == nullptr);
    (void)counter;
}

// ---------------------------------------------------------------------------
// clear_debug() disables callbacks
// ---------------------------------------------------------------------------

TEST_CASE("Debug: clear_debug() stops callbacks") {
    Harness h;
    DebugTracker tracker;
    h.nc.set_debug(tracker.listener());

    note::Api api(h.nc);
    api.card.version().execute();
    int count_before = tracker.wire_count;
    CHECK(count_before > 0);

    h.nc.clear_debug();
    api.card.version().execute();
    CHECK(tracker.wire_count == count_before);  // no new callbacks
}

// ---------------------------------------------------------------------------
// NoDebug policy — compile-time zero
// ---------------------------------------------------------------------------

TEST_CASE("Debug: NoDebug policy methods are constexpr no-ops") {
    note::NoDebug d;
    // These should all compile and do nothing
    d.wire("test", note::WireDirection::Send);
    d.timing(note::TimingEvent::TransactionBegin);
    d.transport(note::TransportEvent::Retry, 1);
    d.alloc(nullptr, 0);
    d.free(nullptr, 0);
    d.realloc(nullptr, nullptr, 0, 0);
    REQUIRE(true);  // if we get here, NoDebug compiles and does nothing
}
