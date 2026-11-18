// Regression tests for streaming-parse handling of nested response objects.
//
// BUG 1: a key inside a nested "body" object must NOT overwrite a same-named
//        top-level response field. Confirmed on hardware via dfu.get, whose
//        response has a top-level "length" and a nested "body":{"length":...}.
//        The generated streaming sink for a body-less response had no object
//        depth tracking, so the nested key leaked to the top-level field.
//
// Drives the generated per-type Response::Sink directly through the streaming
// SAX parser, isolating the sink from the binary-receive execute path.
#include <doctest.h>

#include <note/api/dfu_get.hpp>
#include <note/api/dfu_status.hpp>
#include <note/diag.hpp>
#include <note/json_sax_streaming.hpp>
#include <note/arena.hpp>
#include <note/string_pool.hpp>

#include <algorithm>
#include <cstring>

namespace {

// Feed a JSON c-string to a sink via sax_parse_streaming and return the error.
template <typename SinkT>
note::string_view drive_sink(const char* json, SinkT& sink) {
    size_t pos = 0;
    const size_t len = std::strlen(json);
    auto read_fn = [&](uint8_t* buf, size_t max, uint32_t) -> note::Result<size_t> {
        size_t n = std::min(max, len - pos);
        std::memcpy(buf, json + pos, n);
        pos += n;
        return n;
    };
    return note::sax_parse_streaming(read_fn, 5000, sink);
}

} // namespace

// ---------------------------------------------------------------------------
// BUG 1: a body-less response (dfu.get) must not let a nested body key
// overwrite a same-named top-level field.
TEST_CASE("streaming sink: nested body.length does not overwrite top-level length") {
    using R = note::api::DfuGet::Response;
    R rsp;
    char pool_buf[1024];
    note::MonotonicArena arena(pool_buf);
    note::StringPool pool(note::arena_allocator(arena));
    R::Sink sink(rsp, pool);

    // Shape from the hardware trace: top-level length 8192, nested body.length
    // is the image size 1048736.
    const char* json =
        R"({"id":5,"status":"2435fdec1be66998ad80a46e0b533dde","length":8192,"cobs":8193,)"
        R"("body":{"crc32":2236908510,"length":1048736,"md5":"f7c2fc1bf62ca2516da2df478ca8065c",)"
        R"("name":"spike.bin","type":"firmware"},"crc":"0005:33CA30EF"})";

    auto err = drive_sink(json, sink);
    REQUIRE(err.empty());

    CHECK(rsp.length.value() == 8192);   // regression: was 1048736 (nested body leaked)
    CHECK(rsp.cobs.value() == 8193);
    CHECK(rsp.status.value() == "2435fdec1be66998ad80a46e0b533dde");
}

namespace {
const char* g_captured_warn = nullptr;
}

// ---------------------------------------------------------------------------
// BUG 2 guard: body() must not fail silently when a body was present on the
// wire but dropped (streaming mode, no .into() wired). It reports the misuse
// (soft log by default; this test captures it via the hook).
TEST_CASE("streaming sink: body() reports misuse when a dropped body was present") {
    using R = note::api::DfuStatus::Response;
    R rsp;
    char pool_buf[1024];
    note::MonotonicArena arena(pool_buf);
    note::StringPool pool(note::arena_allocator(arena));
    R::Sink sink(rsp, pool);   // no set_body_handler() -> body is dropped

    const char* json =
        R"({"id":10,"status":"successfully downloaded","mode":"outboard-ready","on":true,)"
        R"("body":{"crc32":2236908510,"length":1048736,"name":"spike.bin","type":"firmware"}})";

    g_captured_warn = nullptr;
    note::detail::streaming_body_warn_hook() = [](const char* m) { g_captured_warn = m; };

    auto err = drive_sink(json, sink);
    REQUIRE(err.empty());

    CHECK(rsp.was_streaming_parse());
    const note::JsonReader* b = rsp.body();   // null: no tree built in streaming
    CHECK(b == nullptr);
    CHECK(g_captured_warn != nullptr);         // the guard fired

    note::detail::streaming_body_warn_hook() = nullptr;  // restore
}

// A response with NO body on the wire must NOT trigger the guard.
TEST_CASE("streaming sink: body() stays quiet when no body was present") {
    using R = note::api::DfuStatus::Response;
    R rsp;
    char pool_buf[512];
    note::MonotonicArena arena(pool_buf);
    note::StringPool pool(note::arena_allocator(arena));
    R::Sink sink(rsp, pool);

    const char* json = R"({"id":11,"status":"idle","mode":"idle","on":false})";

    g_captured_warn = nullptr;
    note::detail::streaming_body_warn_hook() = [](const char* m) { g_captured_warn = m; };

    auto err = drive_sink(json, sink);
    REQUIRE(err.empty());

    CHECK(rsp.body() == nullptr);
    CHECK(g_captured_warn == nullptr);         // no body on wire -> no warning

    note::detail::streaming_body_warn_hook() = nullptr;
}
