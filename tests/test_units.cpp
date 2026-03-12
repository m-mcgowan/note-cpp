#include "catch.hpp"
#include <note/units.hpp>

using namespace note;
using namespace note::literals;

// ---------------------------------------------------------------------------
// Literals
// ---------------------------------------------------------------------------

TEST_CASE("Minutes literals") {
    REQUIRE(60_mins == 60);
    REQUIRE(60_minutes == 60);
}

TEST_CASE("Seconds literals") {
    REQUIRE(300_s == 300);
    REQUIRE(300_seconds == 300);
}

TEST_CASE("Milliseconds literal") {
    REQUIRE(500_ms == 500);
}

TEST_CASE("Hours literals") {
    REQUIRE(2_hours == 2);
    REQUIRE(2_hr == 2);
    REQUIRE(2_h == 2);
}

TEST_CASE("Days literals") {
    REQUIRE(7_days == 7);
    REQUIRE(7_d == 7);
}

// ---------------------------------------------------------------------------
// Implicit conversions (larger → smaller)
// ---------------------------------------------------------------------------

TEST_CASE("Hours → Minutes") {
    Minutes m = 2_hours;
    REQUIRE(m.count == 120);
}

TEST_CASE("Days → Minutes") {
    Minutes m = 7_days;
    REQUIRE(m.count == 10080);
}

TEST_CASE("Minutes → Seconds") {
    Seconds s = 5_mins;
    REQUIRE(s.count == 300);
}

TEST_CASE("Hours → Seconds") {
    Seconds s = 1_hours;
    REQUIRE(s.count == 3600);
}

TEST_CASE("Days → Seconds") {
    Seconds s = 1_days;
    REQUIRE(s.count == 86400);
}

TEST_CASE("Seconds → Milliseconds") {
    Milliseconds ms = 3_s;
    REQUIRE(ms.count == 3000);
}

TEST_CASE("Minutes → Milliseconds") {
    Milliseconds ms = 1_mins;
    REQUIRE(ms.count == 60000);
}

TEST_CASE("Hours → Milliseconds") {
    Milliseconds ms = 1_hours;
    REQUIRE(ms.count == 3600000);
}

TEST_CASE("Days → Hours") {
    Hours h = 2_days;
    REQUIRE(h.count == 48);
}

// ---------------------------------------------------------------------------
// Constexpr evaluation
// ---------------------------------------------------------------------------

TEST_CASE("Conversions are constexpr") {
    static_assert(Minutes(2_hours).count == 120);
    static_assert(Minutes(7_days).count == 10080);
    static_assert(Seconds(5_mins).count == 300);
    static_assert(Seconds(1_hours).count == 3600);
    static_assert(Seconds(1_days).count == 86400);
    static_assert(Hours(2_days).count == 48);
    static_assert(Milliseconds(3_s).count == 3000);
}

// ---------------------------------------------------------------------------
// int32_t conversion
// ---------------------------------------------------------------------------

TEST_CASE("int32_t round-trip") {
    Minutes m = 60;
    int32_t v = m;
    REQUIRE(v == 60);

    Seconds s = 300;
    REQUIRE(static_cast<int32_t>(s) == 300);

    Hours h = 24;
    REQUIRE(static_cast<int32_t>(h) == 24);

    Days d = 7;
    REQUIRE(static_cast<int32_t>(d) == 7);
}

// ---------------------------------------------------------------------------
// Default construction
// ---------------------------------------------------------------------------

TEST_CASE("Default construction is zero") {
    REQUIRE(Minutes{}.count == 0);
    REQUIRE(Seconds{}.count == 0);
    REQUIRE(Milliseconds{}.count == 0);
    REQUIRE(Hours{}.count == 0);
    REQUIRE(Days{}.count == 0);
}
