// Layered transport — IByteTransport (pure bytes) + ITransact (wire format
// / presentation) — exercises the canonical HalByteTransport + JsonRequestTransport
// pair end-to-end on the same MockHal scaffolding used by Protocol's tests.
//
// Two layers:
//   - HalByteTransport : IByteTransport over note::Hal
//   - JsonRequestTransport     : ITransact wrapping any IByteTransport
//
// Tests run a full transaction through JsonRequestTransport(HalByteTransport(hal)),
// validating CRC injection, sax-streaming parse on sink mode, and
// buffered-mode reads to a caller span.

#include <doctest.h>

#include <note/transport.hpp>
#include <note/hal_byte_transport.hpp>
#include <note/json_request_transport.hpp>
#include <note/lexer/parse.hpp>
#include <note/json_sax.hpp>
#include <note/json_buf.hpp>
#include <note/request_source.hpp>

#include <cstring>
#include <deque>
#include <string>

namespace {

class MockHal : public note::Hal {
public:
    std::deque<uint8_t> rx;
    std::string transmitted;

    void queue_response(const std::string& s) {
        for (char c : s) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back(static_cast<uint8_t>('\n'));
    }

    bool transmit(const uint8_t* data, size_t len) override {
        transmitted.append(reinterpret_cast<const char*>(data), len);
        return true;
    }
    note::Result<size_t> read(uint8_t* buf, size_t max, uint32_t) override {
        if (rx.empty())
            return note::make_error(note::Error::ResponseLost,
                                    note::Cause::Timeout, "no data");
        size_t n = std::min(max, rx.size());
        for (size_t i = 0; i < n; ++i) { buf[i] = rx.front(); rx.pop_front(); }
        return n;
    }
    bool reset() override { return true; }
    bool write_line_terminator() override {
        transmitted += "\r\n";
        return true;
    }
    void delay(uint32_t) override {}
    uint32_t millis() override { return 0; }
};

struct CaptureSink : note::JsonSink {
    std::string str_value;
    bool got_str = false;
    note::json_int_t int_value = 0;
    bool got_int = false;
    bool got_bool = false;
    bool bool_value = false;

    void on_string(note::string_view, note::string_view v) override {
        if (!got_str) { str_value.assign(v.data(), v.size()); got_str = true; }
    }
    void on_int(note::string_view, note::json_int_t v) override {
        if (!got_int) { int_value = v; got_int = true; }
    }
    void on_bool(note::string_view, bool v) override {
        if (!got_bool) { bool_value = v; got_bool = true; }
    }
};

struct EmitReq {
    void operator()(note::JsonBuilder& b) const {
        b.add("req", note::string_view("card.version"));
    }
};

} // namespace

TEST_CASE("layered: JsonRequestTransport over HalByteTransport — sink-mode end-to-end") {
    MockHal hal;
    hal.queue_response(R"({"ok":true,"n":42})");

    note::HalByteTransport byte_tx(hal);
    note::JsonRequestTransport transact(byte_tx);

    EmitReq emit;
    note::BuilderRequestSource<EmitReq> src(emit);
    CaptureSink sink;

    auto rv = transact.transact(src.as_source(), sink, 5000);
    REQUIRE(rv.has_value());

    // Wire bytes show the request painted with CRC trailer + line terminator.
    CHECK(hal.transmitted.find(R"("req":"card.version")") != std::string::npos);
    CHECK(hal.transmitted.find(R"("crc":"0001:)") != std::string::npos);
    CHECK(hal.transmitted.back() == '\n');

    CHECK(sink.got_bool);
    CHECK(sink.bool_value == true);
    CHECK(sink.got_int);
    CHECK(sink.int_value == 42);
}

TEST_CASE("layered: JsonRequestTransport over HalByteTransport — buffered-mode end-to-end") {
    MockHal hal;
    hal.queue_response(R"({"foo":"bar"})");

    note::HalByteTransport byte_tx(hal);
    note::JsonRequestTransport transact(byte_tx);

    EmitReq emit;
    note::BuilderRequestSource<EmitReq> src(emit);
    char buf[256];

    auto rv = transact.transact(src.as_source(),
                                note::span<char>(buf, sizeof(buf)), 5000);
    REQUIRE(rv.has_value());
    CHECK(*rv == R"({"foo":"bar"})");
    CHECK(hal.transmitted.find(R"("req":"card.version")") != std::string::npos);
}

TEST_CASE("layered: JsonRequestTransport — send (fire-and-forget) over byte transport") {
    MockHal hal;
    note::HalByteTransport byte_tx(hal);
    note::JsonRequestTransport transact(byte_tx);

    EmitReq emit;
    note::BuilderRequestSource<EmitReq> src(emit);
    auto rv = transact.send(src.as_source());
    REQUIRE(rv.has_value());

    CHECK(hal.transmitted.find(R"("req":"card.version")") != std::string::npos);
    CHECK(hal.transmitted.find(R"("crc":"0001:)") != std::string::npos);
}

TEST_CASE("layered: EndOfFrame is distinct from ResponseLost at byte layer") {
    MockHal hal;
    hal.queue_response(R"({"x":1})");
    note::HalByteTransport byte_tx(hal);

    // Reading without begin_transaction (idle) is harmless — the byte
    // transport returns whatever the HAL serves. Real failure scenarios
    // come from frame-aware semantics, exercised by the full transact.

    REQUIRE(byte_tx.begin_transaction(1000).has_value());
    uint8_t buf[64];
    bool got_end = false;
    for (size_t guard = 0; guard < 128; ++guard) {
        auto r = byte_tx.read(buf, sizeof(buf), 1000);
        if (!r) {
            CHECK(r.error().code == note::Error::EndOfFrame);
            got_end = true;
            break;
        }
    }
    CHECK(got_end);
    byte_tx.end_transaction();
}
