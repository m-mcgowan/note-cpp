// Compile-time benchmark — measures how long this single TU takes to compile.
// Not a test binary — compiled with -ftime-report or just timed externally.
//
// Usage:
//   time c++ -std=c++20 -I include -fsyntax-only tests/bench_compile_time.cpp
//
// Includes all public headers and instantiates key templates to measure
// the cost of the note-cpp header-only library on compile time.

#include <note/notecard.hpp>
#include <note/notecard_api.hpp>
#include <note/api.hpp>
#include <note/body.hpp>
#include <note/json_buf.hpp>
#include <note/transact.hpp>
#include <note/link/serial.hpp>
#include <note/link/i2c.hpp>
#include <note/target.hpp>
#include <note/units.hpp>
#include <note/field.hpp>

// Force template instantiation of commonly used types.
namespace {

note::test::CallbackTransport make_transport() {
    return note::test::CallbackTransport(
        [](note::string_view, uint32_t) -> note::Result<note::string_view> { return "{}"; });
}

void instantiate() {
    note::backends::BufferJsonBackend<512, 64> backend;
    auto transport = make_transport();
    note::Notecard nc(backend, transport);
    note::Api api(nc);

    // Exercise a few endpoint types to measure codegen header cost.
    api.hub.set().product("test").mode("periodic").execute();
    api.card.version().execute();
    api.note.add().file("test.qo").execute();
    api.card.attn().arm().connected().motion().execute();

    // JsonBuf
    note::JsonBuf<64> body;
    body.add("temp", 22.5);
    body.add("humidity", int32_t{60});
    auto v = body.view();
    (void)v;

    // Consteval JsonBuf
#if __cplusplus >= 202002L
    constexpr auto j = note::json<[](auto& b) {
        b.add("temp", 22.5);
        b.close();
    }>();
    static_assert(j.view().size() > 0);
#endif
}

} // namespace
