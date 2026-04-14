#pragma once

/// @file test_sax_exerciser.hpp
/// Pumps all SaxEvent types through a SaxDispatch to ensure every switch
/// branch is covered. Call once per make_sax_dispatch instantiation.

#include <note/lexer/sax_adapter.hpp>

namespace note::test {

/// Send one of each SaxEvent type through the dispatch.
/// This covers every case in the make_sax_dispatch switch for the
/// instantiation that produced this dispatch table.
inline void exercise_all_events(SaxDispatch d) {
    d.dispatch(d.sink,SaxEvent::make_null("_"));
    d.dispatch(d.sink,SaxEvent::make_bool("_", false));
    d.dispatch(d.sink,SaxEvent::make_int("_", 0));
    d.dispatch(d.sink,SaxEvent::make_float("_", 0.0));
    d.dispatch(d.sink,SaxEvent::make_string("_", "_"));
    d.dispatch(d.sink,SaxEvent::make_object_begin("_"));
    d.dispatch(d.sink,SaxEvent::make_object_end("_"));
    d.dispatch(d.sink,SaxEvent::make_array_begin("_"));
    d.dispatch(d.sink,SaxEvent::make_array_end("_"));
    d.dispatch(d.sink,SaxEvent::make_reset());
}

} // namespace note::test
