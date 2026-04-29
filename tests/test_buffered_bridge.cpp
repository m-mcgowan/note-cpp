// Tests for IBufferedTransport's RequestSource bridges (Phase 5a step 6).
//
// IBufferedTransport historically only exposed string_view-shaped transact/send
// virtuals. Phase 5a step 6 added bridges that materialise a RequestSource into
// a stack scratch buffer, append the closing `}`, and forward to the buffered
// virtual. These tests verify each bridge feeds the buffered virtual the
// expected JSON string and propagates the response.

#include <doctest.h>

#include <note/request_source.hpp>
#include <note/transport.hpp>

#include <string>

using namespace note;

namespace {

// IBufferedTransport that records the string_view the bridge forwards in,
// and lets the test inject a canned response.
struct RecordingBufferedTransport : IBufferedTransport {
    std::string recorded_request;
    std::string recorded_send;
    std::string canned_response = R"({"ok":true})";
    int transact_count = 0;
    int send_count = 0;

    // Bring inherited overloads (string_view + RequestSource shapes) into
    // scope so the test can call all transact()/send() variants directly on
    // the derived type.
    using IBufferedTransport::transact;
    using IBufferedTransport::send;

    Result<string_view> transact(string_view request, uint32_t) override {
        ++transact_count;
        recorded_request.assign(request.data(), request.size());
        return string_view(canned_response);
    }
    Result<void> send(string_view request) override {
        ++send_count;
        recorded_send.assign(request.data(), request.size());
        return {};
    }
    void reset() override {}
    void abort() override {}

    struct NoopHal : Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        Result<size_t> read(uint8_t*, size_t, uint32_t) override { return Result<size_t>{size_t{0}}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return 0; }
        void delay(uint32_t) override {}
    } hal_;
    Hal& hal() override { return hal_; }
};

// Sink that records keys and string values for assertion.
struct CaptureSink : JsonSink {
    std::string captured_status;
    void on_string(string_view k, string_view v) override {
        if (k == "status") captured_status.assign(v.data(), v.size());
    }
};

} // namespace

TEST_CASE("IBufferedTransport bridge: transact(RequestSource, span<char>) materialises and forwards") {
    RecordingBufferedTransport transport;
    transport.canned_response = R"({"foo":"bar"})";

    auto build = [](JsonBuilder& b) {
        b.add("req", "test.run");
        b.add("count", json_int_t{42});
    };
    BuilderRequestSource src(build);

    char rsp_buf[256];
    auto rv = transport.transact(src.as_source(), span<char>(rsp_buf, sizeof(rsp_buf)), 1000);
    REQUIRE(rv.has_value());
    REQUIRE(transport.transact_count == 1);

    // The bridge must materialise a complete JSON object including the closing brace.
    CHECK(transport.recorded_request.size() > 0);
    CHECK(transport.recorded_request.front() == '{');
    CHECK(transport.recorded_request.back() == '}');
    CHECK(transport.recorded_request.find(R"("req":"test.run")") != std::string::npos);
    CHECK(transport.recorded_request.find(R"("count":42)") != std::string::npos);

    // Response is copied into the caller's buffer and returned by view.
    CHECK(std::string(rv->data(), rv->size()) == R"({"foo":"bar"})");
}

TEST_CASE("IBufferedTransport bridge: transact(RequestSource, JsonSink&) SAX-parses response") {
    RecordingBufferedTransport transport;
    transport.canned_response = R"({"status":"ok","value":7})";

    auto build = [](JsonBuilder& b) { b.add("req", "test.run"); };
    BuilderRequestSource src(build);

    CaptureSink sink;
    auto rv = transport.transact(src.as_source(), sink, 1000);
    REQUIRE(rv.has_value());
    REQUIRE(transport.transact_count == 1);
    CHECK(sink.captured_status == "ok");
}

TEST_CASE("IBufferedTransport bridge: send(RequestSource) materialises and forwards") {
    RecordingBufferedTransport transport;

    auto build = [](JsonBuilder& b) {
        b.add("cmd", "card.attn");
        b.add("mode", "rearm");
    };
    BuilderRequestSource src(build);

    auto rv = transport.send(src.as_source());
    REQUIRE(rv.has_value());
    REQUIRE(transport.send_count == 1);

    CHECK(transport.recorded_send.front() == '{');
    CHECK(transport.recorded_send.back() == '}');
    CHECK(transport.recorded_send.find(R"("cmd":"card.attn")") != std::string::npos);
    CHECK(transport.recorded_send.find(R"("mode":"rearm")") != std::string::npos);
}

TEST_CASE("IBufferedTransport bridge: oversize request returns Overflow") {
    RecordingBufferedTransport transport;

    // Build a request larger than the bridge's 1024-byte scratch buffer.
    std::string big(2048, 'x');
    auto build = [&big](JsonBuilder& b) { b.add("blob", string_view(big)); };
    BuilderRequestSource src(build);

    char rsp_buf[64];
    auto rv = transport.transact(src.as_source(), span<char>(rsp_buf, sizeof(rsp_buf)), 1000);
    REQUIRE_FALSE(rv.has_value());
    CHECK(rv.error().code == Error::Overflow);
    // Bridge must short-circuit before invoking the buffered virtual.
    CHECK(transport.transact_count == 0);
}
