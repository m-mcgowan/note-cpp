// Tests for Notecard::ping() — a stripped-down `echo`-based connectivity
// probe. Single attempt, no retry, no CRC, no reset on failure.
//
// Wire shape (from note-c PR #239 NotePing): the request is
// {"req":"echo","text":"<16-char nonce>"}. The Notecard echoes the nonce
// back in the response's `text` field; ping() succeeds iff the response
// text matches the request nonce byte-for-byte.

#include <doctest.h>

#include <note/notecard.hpp>
#include <note/transact.hpp>

#include <cstdio>
#include <cstring>
#include <string>

using namespace note;

namespace {

// Pull the 16-char nonce out of a request like {"req":"echo","text":"ABCD..."}.
// Returns empty string if the shape doesn't match — the test asserts the
// shape before reading the nonce, so this only needs to be precise enough
// for well-formed requests.
std::string extract_nonce(string_view req) {
    std::string s(req);
    auto pos = s.find(R"("text":")");
    if (pos == std::string::npos) return {};
    return s.substr(pos + 8, 16);
}

} // namespace

TEST_CASE("ping: success when response text matches request nonce") {
    std::string last_request;
    char rsp_buf[64];

    test::CallbackTransport t(
        [&](string_view req, uint32_t) -> Result<string_view> {
            last_request = std::string(req);
            std::string nonce = extract_nonce(req);
            int n = std::snprintf(rsp_buf, sizeof(rsp_buf),
                                  R"({"cmd":"echo","text":"%s"})", nonce.c_str());
            return string_view(rsp_buf, static_cast<size_t>(n));
        });

    Notecard nc(nullptr, t);
    auto rv = nc.ping();
    CHECK(rv);
    CHECK(last_request.find(R"("req":"echo")") != std::string::npos);
    CHECK(last_request.find(R"("text":")") != std::string::npos);
    // 16-character nonce — extract it and check the length.
    auto nonce = extract_nonce(last_request);
    CHECK(nonce.size() == 16);
}

TEST_CASE("ping: failure when response text does not match the sent nonce") {
    test::CallbackTransport t(
        [&](string_view, uint32_t) -> Result<string_view> {
            return string_view(R"({"cmd":"echo","text":"WRONG_NONCE_XXXX"})");
        });

    Notecard nc(nullptr, t);
    auto rv = nc.ping();
    CHECK_FALSE(rv);
}

TEST_CASE("ping: failure when response has no text field") {
    test::CallbackTransport t(
        [&](string_view, uint32_t) -> Result<string_view> {
            return string_view("{}");
        });

    Notecard nc(nullptr, t);
    auto rv = nc.ping();
    CHECK_FALSE(rv);
}

TEST_CASE("ping: propagates transport errors") {
    test::CallbackTransport t(
        [&](string_view, uint32_t) -> Result<string_view> {
            return make_error(Error::ResponseLost, NOTE_ERR("timeout"));
        });

    Notecard nc(nullptr, t);
    auto rv = nc.ping();
    CHECK_FALSE(rv);
}

TEST_CASE("ping: successive calls produce different nonces") {
    std::string first_request;
    std::string second_request;
    int call = 0;
    char rsp_buf[64];

    test::CallbackTransport t(
        [&](string_view req, uint32_t) -> Result<string_view> {
            if (call++ == 0) first_request = std::string(req);
            else              second_request = std::string(req);
            std::string nonce = extract_nonce(req);
            int n = std::snprintf(rsp_buf, sizeof(rsp_buf),
                                  R"({"text":"%s"})", nonce.c_str());
            return string_view(rsp_buf, static_cast<size_t>(n));
        });

    Notecard nc(nullptr, t);
    auto a = nc.ping();
    auto b = nc.ping();
    REQUIRE(a);
    REQUIRE(b);
    auto first_nonce  = extract_nonce(first_request);
    auto second_nonce = extract_nonce(second_request);
    REQUIRE(first_nonce.size()  == 16);
    REQUIRE(second_nonce.size() == 16);
    CHECK(first_nonce != second_nonce);
}
