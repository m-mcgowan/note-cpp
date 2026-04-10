#include "catch.hpp"
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
