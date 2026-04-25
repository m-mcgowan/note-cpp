// Tests for note::ErrorMessage — the tagged RAM/flash string carrier
// used by ErrorInfo. Host path only exercises the non-Harvard code
// (where PROGMEM reads collapse to plain loads); actual LPM semantics
// need on-device verification.
#include <doctest.h>
#include <note/error.hpp>
#include <note/progmem.hpp>
#include <note/types.hpp>

using note::ErrorMessage;
using note::FlashString;
using note::string_view;

// ---------------------------------------------------------------------------
// RAM-backed construction (the Phase 1 path — unchanged in Phase 2)
// ---------------------------------------------------------------------------
TEST_CASE("ErrorMessage defaults to empty", "[error_message]") {
    ErrorMessage m;
    REQUIRE(m.size() == 0);
    REQUIRE(m.empty());
    REQUIRE_FALSE(m.is_flash());
}

TEST_CASE("ErrorMessage from string_view", "[error_message]") {
    ErrorMessage m{string_view{"send_failed"}};
    REQUIRE(m.size() == 11);
    REQUIRE_FALSE(m.is_flash());
    REQUIRE(m == string_view{"send_failed"});
    REQUIRE(m[0] == 's');
    REQUIRE(m[10] == 'd');
}

TEST_CASE("ErrorMessage from const char*", "[error_message]") {
    const char* s = "hello";
    ErrorMessage m{s};
    REQUIRE(m.size() == 5);
    REQUIRE(m == string_view{"hello"});
}

// ---------------------------------------------------------------------------
// Flash-backed construction (Phase 2). On non-AVR hosts this is still
// a plain pointer — we're checking the API wiring, not LPM reads.
// ---------------------------------------------------------------------------
static constexpr char k_err[] = "arena exhausted";

TEST_CASE("ErrorMessage from FlashString", "[error_message]") {
    ErrorMessage m{note::flash(k_err)};
    REQUIRE(m.is_flash());
    REQUIRE(m.size() == std::size(k_err) - 1);
    REQUIRE(m == string_view{"arena exhausted"});
    REQUIRE(m[0] == 'a');
    REQUIRE(m[5] == ' ');   // "arena"[5] is the space before "exhausted"
}

TEST_CASE("ErrorMessage comparison rejects wrong length", "[error_message]") {
    ErrorMessage ram{string_view{"abc"}};
    ErrorMessage flash{note::flash(k_err)};
    REQUIRE_FALSE(ram == string_view{"abcd"});
    REQUIRE_FALSE(flash == string_view{"arena"});
}

TEST_CASE("ErrorMessage comparison rejects mismatched content", "[error_message]") {
    ErrorMessage m{note::flash(k_err)};
    REQUIRE_FALSE(m == string_view{"arena exhaustef"});   // last char differs
}

// ---------------------------------------------------------------------------
// ErrorInfo uses ErrorMessage transparently
// ---------------------------------------------------------------------------
TEST_CASE("ErrorInfo constructs from RAM literal", "[error_message]") {
    note::ErrorInfo e{note::Error::SendFailed, note::Cause::Timeout, "boom"};
    REQUIRE(e.code == note::Error::SendFailed);
    REQUIRE(e.cause == note::Cause::Timeout);
    REQUIRE(e.message == string_view{"boom"});
    REQUIRE_FALSE(e.message.is_flash());
}

TEST_CASE("ErrorInfo constructs from FlashString", "[error_message]") {
    note::ErrorInfo e{note::Error::Overflow, note::flash(k_err)};
    REQUIRE(e.code == note::Error::Overflow);
    REQUIRE(e.message.is_flash());
    REQUIRE(e.message == string_view{"arena exhausted"});
}

TEST_CASE("to_string(ErrorInfo) handles flash-backed message", "[error_message]") {
    note::ErrorInfo e{note::Error::Overflow, note::flash(k_err)};
    auto formatted = note::to_string(e);
    // Format: "overflow: arena exhausted"
    REQUIRE(string_view{formatted} == string_view{"overflow: arena exhausted"});
}
