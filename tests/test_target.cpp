#include "catch.hpp"
#include <note/target.hpp>

using namespace note;

// ---------------------------------------------------------------------------
// Radios values
// ---------------------------------------------------------------------------

TEST_CASE("Radios enum values are unique power-of-two bits") {
    REQUIRE(static_cast<uint8_t>(Radios::Cell) == 1);
    REQUIRE(static_cast<uint8_t>(Radios::WiFi) == 2);
    REQUIRE(static_cast<uint8_t>(Radios::CellWifi) == 4);
    REQUIRE(static_cast<uint8_t>(Radios::LoRa) == 8);
    REQUIRE(static_cast<uint8_t>(Radios::Skylo) == 16);
}

// ---------------------------------------------------------------------------
// RadiosSupport::supports — variant-based matching
// ---------------------------------------------------------------------------

TEST_CASE("RadiosSupport: empty is universal") {
    constexpr RadiosSupport s{};
    REQUIRE(s.supports(Radios::Cell));
    REQUIRE(s.supports(Radios::WiFi));
    REQUIRE(s.supports(Radios::CellWifi));
    REQUIRE(s.supports(Radios::LoRa));
    REQUIRE(s.supports(Radios::Skylo));
}

TEST_CASE("RadiosSupport: WiFi-only (card.sleep)") {
    constexpr auto s = RadiosSupport::from(Radios::WiFi);
    REQUIRE(s.supports(Radios::WiFi));
    REQUIRE_FALSE(s.supports(Radios::CellWifi));  // has WiFi radio but different family
    REQUIRE_FALSE(s.supports(Radios::Cell));
    REQUIRE_FALSE(s.supports(Radios::LoRa));
    REQUIRE_FALSE(s.supports(Radios::Skylo));
}

TEST_CASE("RadiosSupport: multiple variants (card.wifi)") {
    // card.wifi supports WiFi, CellWifi, and Skylo
    constexpr auto s = RadiosSupport::from(Radios::WiFi, Radios::CellWifi, Radios::Skylo);
    REQUIRE(s.supports(Radios::WiFi));
    REQUIRE(s.supports(Radios::CellWifi));
    REQUIRE(s.supports(Radios::Skylo));
    REQUIRE_FALSE(s.supports(Radios::Cell));
    REQUIRE_FALSE(s.supports(Radios::LoRa));
}

TEST_CASE("RadiosSupport: most endpoints (all except LoRa)") {
    constexpr auto s = RadiosSupport::from(Radios::Cell, Radios::CellWifi, Radios::Skylo, Radios::WiFi);
    REQUIRE(s.supports(Radios::Cell));
    REQUIRE(s.supports(Radios::CellWifi));
    REQUIRE(s.supports(Radios::Skylo));
    REQUIRE(s.supports(Radios::WiFi));
    REQUIRE_FALSE(s.supports(Radios::LoRa));
}

// ---------------------------------------------------------------------------
// Firmware
// ---------------------------------------------------------------------------

TEST_CASE("Firmware::as_int") {
    constexpr Firmware fw{5, 3, 1};
    REQUIRE(fw.as_int() == 50301u);
}

TEST_CASE("Firmware comparison operators") {
    constexpr Firmware a{7, 5, 1};
    constexpr Firmware b{7, 5, 2};
    constexpr Firmware c{8, 0, 0};
    constexpr Firmware zero{};
    REQUIRE(a >= a);
    REQUIRE(b >= a);
    REQUIRE(c >= a);
    REQUIRE_FALSE(a >= b);
    REQUIRE(a < b);
    REQUIRE(a < c);
    REQUIRE_FALSE(b < a);
    REQUIRE(a == a);
    REQUIRE_FALSE(a == b);
    REQUIRE(zero == Firmware{});
}

// ---------------------------------------------------------------------------
// C++20-only: Target, target<>(), concepts
// ---------------------------------------------------------------------------

#if __cplusplus >= 202002L

TEST_CASE("Target::supports delegates to RadiosSupport with radios identity") {
    using WiFiTarget = Target<Radios::WiFi>;
    constexpr auto wifi_only = RadiosSupport::from(Radios::WiFi);
    constexpr auto multi = RadiosSupport::from(Radios::Cell, Radios::WiFi);
    constexpr RadiosSupport universal{};

    REQUIRE(WiFiTarget::supports(wifi_only));
    REQUIRE(WiFiTarget::supports(multi));
    REQUIRE(WiFiTarget::supports(universal));
}

TEST_CASE("Target: CellWifi does NOT match WiFi-only endpoints") {
    using CellWifiTarget = Target<Radios::CellWifi>;
    constexpr auto wifi_only = RadiosSupport::from(Radios::WiFi);

    REQUIRE_FALSE(CellWifiTarget::supports(wifi_only));
}

TEST_CASE("Target::as_strict") {
    using T = Target<Radios::Cell>;
    using S = decltype(T::as_strict());
    REQUIRE(S::strict);
    REQUIRE(S::radios == Radios::Cell);
}

TEST_CASE("target<Radios>() factory") {
    auto t = target<Radios::Skylo>();
    REQUIRE(decltype(t)::radios == Radios::Skylo);
}

// ---------------------------------------------------------------------------
// Static assertions (compile-time verification)
// ---------------------------------------------------------------------------

// WiFi target supports WiFi-only endpoints
static_assert(Target<Radios::WiFi>::supports(RadiosSupport::from(Radios::WiFi)));
// CellWifi does NOT match WiFi-only (different variant)
static_assert(!Target<Radios::CellWifi>::supports(RadiosSupport::from(Radios::WiFi)));
// CellWifi matches endpoints that list CellWifi
static_assert(Target<Radios::CellWifi>::supports(RadiosSupport::from(Radios::WiFi, Radios::CellWifi)));
// Universal always matches
static_assert(Target<Radios::Cell>::supports(RadiosSupport{}));
static_assert(Target<Radios::LoRa>::supports(RadiosSupport{}));

// Concepts
static_assert(IsTarget<Target<Radios::Cell>>);
static_assert(IsTarget<MinFirmware<7, 5, 1>>);
static_assert(!IsTarget<Unconstrained>);
static_assert(IsUnconstrained<Unconstrained>);
static_assert(!IsUnconstrained<Target<Radios::Cell>>);
static_assert(!IsUnconstrained<MinFirmware<7, 5, 1>>);

// HasFirmwareCheck
static_assert(HasFirmwareCheck<MinFirmware<7, 5, 1>>);
static_assert(HasFirmwareCheck<Target<Radios::WiFi>>);
static_assert(!HasFirmwareCheck<Unconstrained>);

// ---------------------------------------------------------------------------
// MinFirmware target
// ---------------------------------------------------------------------------

TEST_CASE("MinFirmware: firmware_ok accepts endpoints at or below target version") {
    using Fw751 = MinFirmware<7, 5, 1>;
    REQUIRE(Fw751::firmware_ok(Firmware{7, 5, 1}));  // exact match
    REQUIRE(Fw751::firmware_ok(Firmware{7, 5, 0}));  // older endpoint
    REQUIRE(Fw751::firmware_ok(Firmware{5, 0, 0}));  // much older
    REQUIRE(Fw751::firmware_ok(Firmware{}));           // universal (no constraint)
    REQUIRE_FALSE(Fw751::firmware_ok(Firmware{7, 5, 2}));  // newer
    REQUIRE_FALSE(Fw751::firmware_ok(Firmware{8, 0, 0}));  // much newer
}

TEST_CASE("MinFirmware: radios is unconstrained") {
    using Fw = MinFirmware<9, 1, 1>;
    REQUIRE(Fw::supports(RadiosSupport::from(Radios::WiFi)));
    REQUIRE(Fw::supports(RadiosSupport::from(Radios::Cell)));
    REQUIRE(Fw::supports(RadiosSupport{}));
}

TEST_CASE("min_firmware<>() factory") {
    auto t = min_firmware<7, 5, 1>();
    REQUIRE(decltype(t)::version.major == 7);
    REQUIRE(decltype(t)::version.minor == 5);
    REQUIRE(decltype(t)::version.patch == 1);
}

// ---------------------------------------------------------------------------
// Target with firmware constraint
// ---------------------------------------------------------------------------

TEST_CASE("Target with firmware: combined radios + firmware check") {
    using WiFiFw = Target<Radios::WiFi, 7, 5, 1>;
    // Radios check
    REQUIRE(WiFiFw::supports(RadiosSupport::from(Radios::WiFi)));
    REQUIRE_FALSE(WiFiFw::supports(RadiosSupport::from(Radios::Cell)));
    // Firmware check
    REQUIRE(WiFiFw::firmware_ok(Firmware{7, 5, 1}));
    REQUIRE(WiFiFw::firmware_ok(Firmware{5, 0, 0}));
    REQUIRE(WiFiFw::firmware_ok(Firmware{}));
    REQUIRE_FALSE(WiFiFw::firmware_ok(Firmware{8, 0, 0}));
}

TEST_CASE("Target without firmware: firmware_ok always true") {
    using WiFi = Target<Radios::WiFi>;
    REQUIRE(WiFi::firmware_ok(Firmware{99, 99, 99}));
    REQUIRE(WiFi::firmware_ok(Firmware{}));
}

TEST_CASE("target<Radios, Major, Minor, Patch>() factory") {
    auto t = target<Radios::Cell, 5, 3, 1>();
    REQUIRE(decltype(t)::radios == Radios::Cell);
    REQUIRE(decltype(t)::firmware_version.as_int() == 50301u);
}

// ---------------------------------------------------------------------------
// target_supports — combined radios + firmware check
// ---------------------------------------------------------------------------

namespace {
    // Mock endpoint types for target_supports tests
    struct UniversalEndpoint {
        static constexpr RadiosSupport radios{};
        static constexpr Firmware min_firmware{};
    };
    struct WiFiOnlyEndpoint {
        static constexpr RadiosSupport radios = RadiosSupport::from(Radios::WiFi);
        static constexpr Firmware min_firmware{};
    };
    struct FirmwareGatedEndpoint {
        static constexpr RadiosSupport radios{};
        static constexpr Firmware min_firmware{9, 1, 1};
    };
    struct WiFiFirmwareEndpoint {
        static constexpr RadiosSupport radios = RadiosSupport::from(Radios::WiFi);
        static constexpr Firmware min_firmware{7, 5, 1};
    };
}

// Unconstrained supports everything
static_assert(target_supports<Unconstrained, UniversalEndpoint>());
static_assert(target_supports<Unconstrained, WiFiOnlyEndpoint>());
static_assert(target_supports<Unconstrained, FirmwareGatedEndpoint>());

// Radios-only target: checks radios, ignores firmware
static_assert(target_supports<Target<Radios::WiFi>, UniversalEndpoint>());
static_assert(target_supports<Target<Radios::WiFi>, WiFiOnlyEndpoint>());
static_assert(target_supports<Target<Radios::WiFi>, FirmwareGatedEndpoint>());
static_assert(!target_supports<Target<Radios::Cell>, WiFiOnlyEndpoint>());

// Firmware-only target: checks firmware, ignores radios
static_assert(target_supports<MinFirmware<9, 1, 1>, UniversalEndpoint>());
static_assert(target_supports<MinFirmware<9, 1, 1>, FirmwareGatedEndpoint>());
static_assert(target_supports<MinFirmware<9, 1, 1>, WiFiOnlyEndpoint>());
static_assert(!target_supports<MinFirmware<5, 0, 0>, FirmwareGatedEndpoint>());

// Combined target: both must pass
static_assert(target_supports<Target<Radios::WiFi, 9, 1, 1>, WiFiFirmwareEndpoint>());
static_assert(!target_supports<Target<Radios::Cell, 9, 1, 1>, WiFiFirmwareEndpoint>());
static_assert(!target_supports<Target<Radios::WiFi, 5, 0, 0>, WiFiFirmwareEndpoint>());

#endif // C++20
