// Tests for BareNotecard — raw JSON passthrough.
// Written as expected developer experience: construct with a transport,
// send/transact raw JSON strings.

#include "catch.hpp"
#include <note/bare_notecard.hpp>
#include <cstring>

namespace {

// Mock HAL that captures transmitted bytes and returns a canned response.
struct MockHal : note::TransportHal {
    std::string last_sent;
    std::string canned_response;
    size_t rsp_pos = 0;

    explicit MockHal(const char* response = "{}") : canned_response(response) {
        canned_response += "\r\n";
    }

    bool transmit(const uint8_t* data, size_t len) override {
        last_sent.append(reinterpret_cast<const char*>(data), len);
        return true;
    }
    note::Result<size_t> read(uint8_t* buf, size_t max, uint32_t) override {
        if (rsp_pos >= canned_response.size()) return size_t(0);
        size_t n = std::min(max, canned_response.size() - rsp_pos);
        std::memcpy(buf, canned_response.data() + rsp_pos, n);
        rsp_pos += n;
        return n;
    }
    bool reset() override { return true; }
    bool write_line_terminator() override {
        last_sent += "\r\n";
        rsp_pos = 0;
        return true;
    }
    void delay(uint32_t) override {}
    uint32_t millis() override { return 0; }
};

} // namespace

// ---------------------------------------------------------------------------
// Basic usage
// ---------------------------------------------------------------------------

TEST_CASE("BareNotecard transact sends JSON and returns response") {
    MockHal hal(R"({"version":"notecard-7.2.1"})");
    note::StreamingTransport transport(hal);
    note::BareNotecard bare(transport);

    char buf[256];
    auto rsp = bare.transact(R"({"req":"card.version"})", buf);
    REQUIRE(rsp);
    REQUIRE(std::string(*rsp) == R"({"version":"notecard-7.2.1"})");
    REQUIRE(hal.last_sent.find(R"({"req":"card.version"})") != std::string::npos);
}

TEST_CASE("BareNotecard send fires and forgets") {
    MockHal hal;
    note::StreamingTransport transport(hal);
    note::BareNotecard bare(transport);

    auto result = bare.send(R"({"cmd":"hub.set","product":"com.example"})");
    REQUIRE(result);
    REQUIRE(hal.last_sent.find("hub.set") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST_CASE("BareNotecard rejects malformed JSON on transact") {
    MockHal hal;
    note::StreamingTransport transport(hal);
    note::BareNotecard bare(transport);

    char buf[256];
    REQUIRE(!bare.transact("not json", buf));
    REQUIRE(!bare.transact("{missing close", buf));
    REQUIRE(!bare.transact("", buf));
}

TEST_CASE("BareNotecard rejects malformed JSON on send") {
    MockHal hal;
    note::StreamingTransport transport(hal);
    note::BareNotecard bare(transport);

    REQUIRE(!bare.send("not json"));
    REQUIRE(!bare.send("{\"key\":}"));
}

// ---------------------------------------------------------------------------
// Nested JSON preservation
// ---------------------------------------------------------------------------

TEST_CASE("BareNotecard preserves nested objects in response") {
    MockHal hal(R"({"version":"7.2.1","body":{"org":"blues","product":"feather"}})");
    note::StreamingTransport transport(hal);
    note::BareNotecard bare(transport);

    char buf[512];
    auto rsp = bare.transact(R"({"req":"card.version"})", buf);
    REQUIRE(rsp);
    auto s = std::string(*rsp);
    REQUIRE(s.find("\"body\":{") != std::string::npos);
    REQUIRE(s.find("\"org\":\"blues\"") != std::string::npos);
}

TEST_CASE("BareNotecard preserves arrays in response") {
    MockHal hal(R"({"files":["data.qi","config.db"],"total":2})");
    note::StreamingTransport transport(hal);
    note::BareNotecard bare(transport);

    char buf[512];
    auto rsp = bare.transact(R"({"req":"file.changes"})", buf);
    REQUIRE(rsp);
    auto s = std::string(*rsp);
    REQUIRE(s.find("\"files\":[") != std::string::npos);
    REQUIRE(s.find("\"data.qi\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Notecard error responses pass through
// ---------------------------------------------------------------------------

TEST_CASE("BareNotecard passes through Notecard error responses") {
    MockHal hal(R"({"err":"not a valid request"})");
    note::StreamingTransport transport(hal);
    note::BareNotecard bare(transport);

    char buf[256];
    auto rsp = bare.transact(R"({"req":"bogus.request"})", buf);
    // Transport succeeded — the error is in the JSON, not a transport failure
    REQUIRE(rsp);
    REQUIRE(std::string(*rsp).find("\"err\"") != std::string::npos);
}
