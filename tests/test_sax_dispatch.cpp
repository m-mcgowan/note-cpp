#include "catch.hpp"
#include "test_sax_exerciser.hpp"
#include <note/lexer/sax_adapter.hpp>
#include <note/json_sax.hpp>  // NullSink

#include <string>
#include <vector>

// Recording sink -- captures events for verification.
struct SaxRecordingSink {
    struct Event {
        enum Tag { Null, Bool, Int, Float, String, ObjBegin, ObjEnd, ArrBegin, ArrEnd, Reset };
        Tag tag;
        std::string key;
        bool b = false;
        int32_t i = 0;
        double f = 0.0;
        std::string s;
    };
    std::vector<Event> events;

    void push(Event::Tag tag, note::string_view k) {
        Event e{}; e.tag = tag; e.key = std::string(k.data(), k.size());
        events.push_back(e);
    }

    void on_null(note::string_view k) { push(Event::Null, k); }
    void on_bool(note::string_view k, bool v) {
        Event e{}; e.tag = Event::Bool; e.key = std::string(k.data(), k.size()); e.b = v;
        events.push_back(e);
    }
    void on_int(note::string_view k, int32_t v) {
        Event e{}; e.tag = Event::Int; e.key = std::string(k.data(), k.size()); e.i = v;
        events.push_back(e);
    }
    void on_float(note::string_view k, double v) {
        Event e{}; e.tag = Event::Float; e.key = std::string(k.data(), k.size()); e.f = v;
        events.push_back(e);
    }
    void on_string(note::string_view k, note::string_view v) {
        Event e{}; e.tag = Event::String; e.key = std::string(k.data(), k.size());
        e.s = std::string(v.data(), v.size());
        events.push_back(e);
    }
    void on_number(note::string_view, note::string_view) {}
    void on_object_begin(note::string_view k) { push(Event::ObjBegin, k); }
    void on_object_end(note::string_view k) { push(Event::ObjEnd, k); }
    void on_array_begin(note::string_view k) { push(Event::ArrBegin, k); }
    void on_array_end(note::string_view k) { push(Event::ArrEnd, k); }
    void reset() { push(Event::Reset, {}); }
};

TEST_CASE("SaxEvent: construction and field access") {
    using E = note::SaxEvent;

    auto e1 = E::make_bool("flag", true);
    REQUIRE(e1.tag == E::Bool);
    REQUIRE(e1.key == "flag");
    REQUIRE(e1.b == true);

    auto e2 = E::make_int("count", 42);
    REQUIRE(e2.tag == E::Int);
    REQUIRE(e2.i == 42);

    auto e3 = E::make_float("value", 3.14);
    REQUIRE(e3.tag == E::Float);
    REQUIRE(e3.f == Approx(3.14));

    auto e4 = E::make_string("name", "hello");
    REQUIRE(e4.tag == E::String);
    REQUIRE(note::string_view(e4.sv.data, e4.sv.len) == "hello");

    auto e5 = E::make_null("key");
    REQUIRE(e5.tag == E::Null);
    REQUIRE(e5.key == "key");

    auto e6 = E::make_object_begin("obj");
    REQUIRE(e6.tag == E::ObjectBegin);

    auto e7 = E::make_object_end("obj");
    REQUIRE(e7.tag == E::ObjectEnd);

    auto e8 = E::make_array_begin("arr");
    REQUIRE(e8.tag == E::ArrayBegin);

    auto e9 = E::make_array_end("arr");
    REQUIRE(e9.tag == E::ArrayEnd);

    auto e10 = E::make_reset();
    REQUIRE(e10.tag == E::Reset);
}

TEST_CASE("make_sax_dispatch: single dispatch routes all event types") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);

    note::SaxEvent ev;

    ev = note::SaxEvent::make_null("k1");
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_bool("k2", true);
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_int("k3", 42);
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_float("k4", 1.5);
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_string("k5", "val");
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_object_begin("k6");
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_object_end("k7");
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_array_begin("k8");
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_array_end("k9");
    dispatch.dispatch(dispatch.sink, ev);

    ev = note::SaxEvent::make_reset();
    dispatch.dispatch(dispatch.sink, ev);

    REQUIRE(sink.events.size() == 10);
    REQUIRE(sink.events[0].tag == SaxRecordingSink::Event::Null);
    REQUIRE(sink.events[0].key == "k1");
    REQUIRE(sink.events[1].tag == SaxRecordingSink::Event::Bool);
    REQUIRE(sink.events[1].b == true);
    REQUIRE(sink.events[2].tag == SaxRecordingSink::Event::Int);
    REQUIRE(sink.events[2].i == 42);
    REQUIRE(sink.events[3].tag == SaxRecordingSink::Event::Float);
    REQUIRE(sink.events[3].f == Approx(1.5));
    REQUIRE(sink.events[4].tag == SaxRecordingSink::Event::String);
    REQUIRE(sink.events[4].s == "val");
    REQUIRE(sink.events[5].tag == SaxRecordingSink::Event::ObjBegin);
    REQUIRE(sink.events[6].tag == SaxRecordingSink::Event::ObjEnd);
    REQUIRE(sink.events[7].tag == SaxRecordingSink::Event::ArrBegin);
    REQUIRE(sink.events[8].tag == SaxRecordingSink::Event::ArrEnd);
    REQUIRE(sink.events[9].tag == SaxRecordingSink::Event::Reset);
}

TEST_CASE("make_sax_dispatch: NullSink compiles and dispatches without crash") {
    note::NullSink null_sink;
    auto dispatch = note::make_sax_dispatch(null_sink);

    auto ev = note::SaxEvent::make_bool("x", false);
    dispatch.dispatch(dispatch.sink, ev);
    ev = note::SaxEvent::make_string("y", "z");
    dispatch.dispatch(dispatch.sink, ev);
    ev = note::SaxEvent::make_reset();
    dispatch.dispatch(dispatch.sink, ev);
}

// ═══════════════════════════════════════════════════════════════════════
// SaxAdapter unit tests — direct tests for the lexer-to-sink bridge
// ═══════════════════════════════════════════════════════════════════════

// Helpers to construct LexerEvent values.
static note::LexerEvent make_key_char(char c) {
    note::LexerEvent e; e.tag = note::LexerEvent::KeyChar; e.ch = c; return e;
}
static note::LexerEvent make_key_end() {
    note::LexerEvent e; e.tag = note::LexerEvent::KeyEnd; return e;
}
static note::LexerEvent make_str_char(char c) {
    note::LexerEvent e; e.tag = note::LexerEvent::StringChar; e.ch = c; return e;
}
static note::LexerEvent make_str_end() {
    note::LexerEvent e; e.tag = note::LexerEvent::StringEnd; return e;
}
static note::LexerEvent make_obj_begin() {
    note::LexerEvent e; e.tag = note::LexerEvent::ObjectBegin; return e;
}
static note::LexerEvent make_obj_end() {
    note::LexerEvent e; e.tag = note::LexerEvent::ObjectEnd; return e;
}
static note::LexerEvent make_arr_begin() {
    note::LexerEvent e; e.tag = note::LexerEvent::ArrayBegin; return e;
}
static note::LexerEvent make_arr_end() {
    note::LexerEvent e; e.tag = note::LexerEvent::ArrayEnd; return e;
}
static note::LexerEvent make_integer(int32_t v) {
    note::LexerEvent e; e.tag = note::LexerEvent::Integer; e.integer = v; return e;
}
static note::LexerEvent make_float(double v) {
    note::LexerEvent e; e.tag = note::LexerEvent::Float; e.floating = v; return e;
}
static note::LexerEvent make_bool_ev(bool v) {
    note::LexerEvent e; e.tag = note::LexerEvent::Bool; e.boolean = v; return e;
}
static note::LexerEvent make_null_ev() {
    note::LexerEvent e; e.tag = note::LexerEvent::Null; return e;
}
static note::LexerEvent make_error_ev(const char* msg) {
    note::LexerEvent e; e.tag = note::LexerEvent::Error; e.error = msg; return e;
}

// Feed a key string (char by char + end) to the adapter.
static void feed_key(note::SaxAdapter& a, const char* key) {
    for (const char* p = key; *p; ++p)
        a.on_event(make_key_char(*p));
    a.on_event(make_key_end());
}

// Feed a string value (char by char + end) to the adapter.
static void feed_string(note::SaxAdapter& a, const char* val) {
    for (const char* p = val; *p; ++p)
        a.on_event(make_str_char(*p));
    a.on_event(make_str_end());
}

TEST_CASE("SaxAdapter: simple key-value integer") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    feed_key(adapter, "count");
    adapter.on_event(make_integer(42));

    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].tag == SaxRecordingSink::Event::Int);
    CHECK(sink.events[0].key == "count");
    CHECK(sink.events[0].i == 42);
}

TEST_CASE("SaxAdapter: string accumulation") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    feed_key(adapter, "name");
    feed_string(adapter, "hello");

    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].tag == SaxRecordingSink::Event::String);
    CHECK(sink.events[0].s == "hello");
}

TEST_CASE("SaxAdapter: nested objects push/pop key") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    feed_key(adapter, "outer");
    adapter.on_event(make_obj_begin());  // pushes "outer"
    feed_key(adapter, "inner");
    adapter.on_event(make_integer(99));
    adapter.on_event(make_obj_end());    // pops, restores "outer"

    // After pop, the key should be restored to "outer"
    feed_key(adapter, "after");
    adapter.on_event(make_integer(1));

    REQUIRE(sink.events.size() == 4);  // ObjBegin, Int(inner), ObjEnd, Int(after)
    CHECK(sink.events[0].tag == SaxRecordingSink::Event::ObjBegin);
    CHECK(sink.events[0].key == "outer");
    CHECK(sink.events[1].key == "inner");
    CHECK(sink.events[2].tag == SaxRecordingSink::Event::ObjEnd);
    CHECK(sink.events[3].key == "after");
}

TEST_CASE("SaxAdapter: 8 levels of nesting (max depth)") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[512];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    // Push 8 nested objects — exactly at kMaxDepth
    const char* keys[] = {"a", "b", "c", "d", "e", "f", "g", "h"};
    for (int i = 0; i < 8; ++i) {
        feed_key(adapter, keys[i]);
        adapter.on_event(make_obj_begin());
    }

    // Add a value at the deepest level
    feed_key(adapter, "val");
    adapter.on_event(make_integer(42));

    // Pop all 8 levels
    for (int i = 0; i < 8; ++i)
        adapter.on_event(make_obj_end());

    // Verify the deepest value was captured
    bool found_val = false;
    for (auto& e : sink.events) {
        if (e.tag == SaxRecordingSink::Event::Int && e.key == "val") {
            CHECK(e.i == 42);
            found_val = true;
        }
    }
    CHECK(found_val);

    // After all pops, key should be restored properly
    // The last ObjectEnd should have the key of the outermost pushed key
    auto& last_end = sink.events.back();
    CHECK(last_end.tag == SaxRecordingSink::Event::ObjEnd);
}

TEST_CASE("SaxAdapter: >8 levels does not crash (stack overflow)") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[512];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    // Push 10 levels — exceeds kMaxDepth (8)
    for (int i = 0; i < 10; ++i) {
        feed_key(adapter, "x");
        adapter.on_event(make_obj_begin());
    }

    // Value at depth 10
    feed_key(adapter, "deep");
    adapter.on_event(make_integer(1));

    // Pop all 10
    for (int i = 0; i < 10; ++i)
        adapter.on_event(make_obj_end());

    // Should not crash. Keys beyond depth 8 are lost but events flow.
    CHECK(sink.events.size() > 0);
}

TEST_CASE("SaxAdapter: long key truncation (>key_cap)") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    // Small key buffer to test truncation
    uint8_t rbuf[8];
    char key_buf[8];
    char val_buf[64];
    note::SaxStreamBuf buf{rbuf, sizeof(rbuf), key_buf, sizeof(key_buf), val_buf, sizeof(val_buf)};
    note::SaxAdapter adapter(buf, dispatch);

    feed_key(adapter, "this_is_a_very_long_key");
    adapter.on_event(make_integer(1));

    REQUIRE(sink.events.size() == 1);
    // Key should be truncated to key_cap (8 chars)
    CHECK(sink.events[0].key == "this_is_");
}

TEST_CASE("SaxAdapter: value buffer overflow (>val_cap)") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    uint8_t rbuf[8];
    char key_buf[32];
    char val_buf[4];  // tiny value buffer
    note::SaxStreamBuf buf{rbuf, sizeof(rbuf), key_buf, sizeof(key_buf), val_buf, sizeof(val_buf)};
    note::SaxAdapter adapter(buf, dispatch);

    feed_key(adapter, "s");
    feed_string(adapter, "abcdefgh");  // 8 chars, only 4 fit

    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].s == "abcd");  // truncated to 4
}

TEST_CASE("SaxAdapter: array depth tracking") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    feed_key(adapter, "arr");
    adapter.on_event(make_arr_begin());
    adapter.on_event(make_integer(1));
    adapter.on_event(make_integer(2));
    adapter.on_event(make_arr_end());

    REQUIRE(sink.events.size() == 4);
    CHECK(sink.events[0].tag == SaxRecordingSink::Event::ArrBegin);
    CHECK(sink.events[3].tag == SaxRecordingSink::Event::ArrEnd);
}

TEST_CASE("SaxAdapter: array end without begin does not crash") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    // ArrayEnd with no matching ArrayBegin
    adapter.on_event(make_arr_end());
    // Should not crash — in_array_depth_ stays at 0
    CHECK(sink.events.size() == 1);
}

TEST_CASE("SaxAdapter: error event captured") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    CHECK(adapter.error() == nullptr);
    adapter.on_event(make_error_ev("bad json"));
    CHECK(std::string(adapter.error()) == "bad json");
    // Error events are NOT dispatched to the sink
    CHECK(sink.events.empty());
}

TEST_CASE("SaxAdapter: reset clears state") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    adapter.on_event(make_error_ev("err"));
    feed_key(adapter, "x");
    adapter.on_event(make_obj_begin());

    adapter.reset();

    CHECK(adapter.error() == nullptr);
    // Reset should dispatch a Reset event
    REQUIRE(sink.events.size() == 2);  // ObjBegin + Reset
    CHECK(sink.events.back().tag == SaxRecordingSink::Event::Reset);
}

TEST_CASE("SaxAdapter: bool and null events") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    feed_key(adapter, "flag");
    adapter.on_event(make_bool_ev(true));
    feed_key(adapter, "empty");
    adapter.on_event(make_null_ev());

    REQUIRE(sink.events.size() == 2);
    CHECK(sink.events[0].tag == SaxRecordingSink::Event::Bool);
    CHECK(sink.events[0].b == true);
    CHECK(sink.events[1].tag == SaxRecordingSink::Event::Null);
}

TEST_CASE("SaxAdapter: float events") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    feed_key(adapter, "pi");
    adapter.on_event(make_float(3.14));

    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].tag == SaxRecordingSink::Event::Float);
    CHECK(sink.events[0].f == Approx(3.14));
}

TEST_CASE("SaxAdapter: pop_key underflow does not crash") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    // ObjectEnd without matching ObjectBegin — pop on empty stack
    adapter.on_event(make_obj_end());
    CHECK(sink.events.size() == 1);
    CHECK(sink.events[0].tag == SaxRecordingSink::Event::ObjEnd);
}

TEST_CASE("SaxAdapter: operator() delegates to on_event") {
    SaxRecordingSink sink;
    auto dispatch = note::make_sax_dispatch(sink);
    char storage[256];
    note::SaxStreamBuf buf(storage);
    note::SaxAdapter adapter(buf, dispatch);

    adapter(make_null_ev());
    REQUIRE(sink.events.size() == 1);
    CHECK(sink.events[0].tag == SaxRecordingSink::Event::Null);
}
