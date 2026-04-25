// Tests for json_fmt — compile-time validated JSON templates.

#include <doctest.h>

#if __cplusplus >= 202002L
#include <note/json_fmt.hpp>

TEST_CASE("json_fmt untyped placeholders") {
    auto r = note::json_fmt<R"({"temp":{},"count":{}})">(22.5f, 42);
    REQUIRE(r.view() == R"({"temp":22.5,"count":42})");
}

TEST_CASE("json_fmt typed placeholders") {
    auto r = note::json_fmt<R"({"temp":{f},"count":{i},"active":{b},"name":{s}})">(
        22.5f, 42, true, "sensor-1");
    REQUIRE(r.view() == R"({"temp":22.5,"count":42,"active":true,"name":"sensor-1"})");
}

TEST_CASE("json_fmt with literal values preserved") {
    auto r = note::json_fmt<R"({"version":"1.0","temp":{}})">(22.5f);
    REQUIRE(r.view() == R"({"version":"1.0","temp":22.5})");
}

TEST_CASE("json_fmt string escaping — quotes") {
    auto r = note::json_fmt<R"({"msg":{s}})">(R"(hello "world")");
    REQUIRE(r.view() == R"({"msg":"hello \"world\""})");
}

TEST_CASE("json_fmt string escaping — backslash") {
    auto r = note::json_fmt<R"({"path":{s}})">("C:\\Users\\test");
    REQUIRE(r.view() == R"({"path":"C:\\Users\\test"})");
}

TEST_CASE("json_fmt bool values") {
    auto r1 = note::json_fmt<R"({"a":{b}})">(true);
    REQUIRE(r1.view() == R"({"a":true})");
    auto r2 = note::json_fmt<R"({"a":{b}})">(false);
    REQUIRE(r2.view() == R"({"a":false})");
}

TEST_CASE("json_fmt negative numbers") {
    auto r = note::json_fmt<R"({"v":{i}})">(int32_t{-42});
    REQUIRE(r.view() == R"({"v":-42})");
}

TEST_CASE("json_fmt empty object with no placeholders") {
    auto r = note::json_fmt<R"({"status":"ok"})">();
    REQUIRE(r.view() == R"({"status":"ok"})");
}

TEST_CASE("json_fmt nested literal object preserved") {
    auto r = note::json_fmt<R"({"meta":{"v":1},"temp":{}})">(22.5f);
    REQUIRE(r.view() == R"({"meta":{"v":1},"temp":22.5})");
}

// Compile-time validation (these are static_asserts, not runtime tests)
static_assert(note::detail::count_placeholders(R"({"a":{},"b":{}})") == 2);
static_assert(note::detail::count_placeholders(R"({"a":{i}})") == 1);
static_assert(note::detail::count_placeholders(R"({"a":"fixed"})") == 0);
static_assert(note::detail::validate_fmt_structure(R"({"a":{}})"));
static_assert(note::detail::validate_fmt_structure(R"({"a":{i},"b":{s}})"));
static_assert(!note::detail::validate_fmt_structure(R"({"a":{},,})"));
static_assert(!note::detail::validate_fmt_structure(R"([{},{}])"));

#endif // C++20
