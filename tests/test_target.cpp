#include "catch.hpp"
#include <note/target.hpp>

using namespace note;

// ---------------------------------------------------------------------------
// Rat bitfield operations
// ---------------------------------------------------------------------------

TEST_CASE("Rat bitfield OR") {
    constexpr auto r = Rat::Cell | Rat::WiFi;
    REQUIRE(static_cast<uint8_t>(r) == 3);
}

TEST_CASE("Rat bitfield AND") {
    constexpr auto r = (Rat::Cell | Rat::WiFi) & Rat::Cell;
    REQUIRE(static_cast<uint8_t>(r) == 1);
}

TEST_CASE("has_rat") {
    constexpr auto set = Rat::Cell | Rat::WiFi;
    REQUIRE(has_rat(set, Rat::Cell));
    REQUIRE(has_rat(set, Rat::WiFi));
    REQUIRE_FALSE(has_rat(set, Rat::Ntn));
    REQUIRE_FALSE(has_rat(set, Rat::LoRa));
}

// ---------------------------------------------------------------------------
// Hardware values
// ---------------------------------------------------------------------------

TEST_CASE("Hardware enum values are unique power-of-two bits") {
    REQUIRE(static_cast<uint8_t>(Hardware::Cell) == 1);
    REQUIRE(static_cast<uint8_t>(Hardware::WiFi) == 2);
    REQUIRE(static_cast<uint8_t>(Hardware::CellWifi) == 4);
    REQUIRE(static_cast<uint8_t>(Hardware::LoRa) == 8);
    REQUIRE(static_cast<uint8_t>(Hardware::Skylo) == 16);
}

TEST_CASE("rats_of extracts RAT bitmask from Hardware") {
    using U = uint8_t;
    REQUIRE(static_cast<U>(rats_of(Hardware::Cell)) == static_cast<U>(Rat::Cell));
    REQUIRE(static_cast<U>(rats_of(Hardware::WiFi)) == static_cast<U>(Rat::WiFi));
    REQUIRE(static_cast<U>(rats_of(Hardware::CellWifi)) == static_cast<U>(Rat::Cell | Rat::WiFi));
    REQUIRE(static_cast<U>(rats_of(Hardware::LoRa)) == static_cast<U>(Rat::LoRa));
    REQUIRE(static_cast<U>(rats_of(Hardware::Skylo)) == static_cast<U>(Rat::Cell | Rat::WiFi | Rat::Ntn));
}

// ---------------------------------------------------------------------------
// HardwareSupport::supports — variant-based matching
// ---------------------------------------------------------------------------

TEST_CASE("HardwareSupport: empty is universal") {
    constexpr HardwareSupport s{};
    REQUIRE(s.supports(Hardware::Cell));
    REQUIRE(s.supports(Hardware::WiFi));
    REQUIRE(s.supports(Hardware::CellWifi));
    REQUIRE(s.supports(Hardware::LoRa));
    REQUIRE(s.supports(Hardware::Skylo));
}

TEST_CASE("HardwareSupport: WiFi-only (card.sleep)") {
    constexpr auto s = HardwareSupport::from(Hardware::WiFi);
    REQUIRE(s.supports(Hardware::WiFi));
    REQUIRE_FALSE(s.supports(Hardware::CellWifi));  // has WiFi RAT, but different variant
    REQUIRE_FALSE(s.supports(Hardware::Cell));
    REQUIRE_FALSE(s.supports(Hardware::LoRa));
    REQUIRE_FALSE(s.supports(Hardware::Skylo));
}

TEST_CASE("HardwareSupport: multiple variants (card.wifi)") {
    // card.wifi supports WiFi, CellWifi, and Skylo
    constexpr auto s = HardwareSupport::from(Hardware::WiFi, Hardware::CellWifi, Hardware::Skylo);
    REQUIRE(s.supports(Hardware::WiFi));
    REQUIRE(s.supports(Hardware::CellWifi));
    REQUIRE(s.supports(Hardware::Skylo));
    REQUIRE_FALSE(s.supports(Hardware::Cell));
    REQUIRE_FALSE(s.supports(Hardware::LoRa));
}

TEST_CASE("HardwareSupport: most endpoints (all except LoRa)") {
    constexpr auto s = HardwareSupport::from(Hardware::Cell, Hardware::CellWifi, Hardware::Skylo, Hardware::WiFi);
    REQUIRE(s.supports(Hardware::Cell));
    REQUIRE(s.supports(Hardware::CellWifi));
    REQUIRE(s.supports(Hardware::Skylo));
    REQUIRE(s.supports(Hardware::WiFi));
    REQUIRE_FALSE(s.supports(Hardware::LoRa));
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

TEST_CASE("Target::supports delegates to HardwareSupport with hardware identity") {
    using WiFiTarget = Target<Hardware::WiFi>;
    constexpr auto wifi_only = HardwareSupport::from(Hardware::WiFi);
    constexpr auto multi = HardwareSupport::from(Hardware::Cell, Hardware::WiFi);
    constexpr HardwareSupport universal{};

    REQUIRE(WiFiTarget::supports(wifi_only));
    REQUIRE(WiFiTarget::supports(multi));
    REQUIRE(WiFiTarget::supports(universal));
}

TEST_CASE("Target: CellWifi does NOT match WiFi-only endpoints") {
    using CellWifiTarget = Target<Hardware::CellWifi>;
    constexpr auto wifi_only = HardwareSupport::from(Hardware::WiFi);

    REQUIRE_FALSE(CellWifiTarget::supports(wifi_only));
}

TEST_CASE("Target::as_strict") {
    using T = Target<Hardware::Cell>;
    using S = decltype(T::as_strict());
    REQUIRE(S::strict);
    REQUIRE(S::hardware == Hardware::Cell);
}

TEST_CASE("target<Hardware>() factory") {
    auto t = target<Hardware::Skylo>();
    REQUIRE(decltype(t)::hardware == Hardware::Skylo);
    // Skylo RATs: Cell|WiFi|Ntn = 1|2|4 = 7
    REQUIRE(static_cast<uint8_t>(decltype(t)::rats) ==
            static_cast<uint8_t>(Rat::Cell | Rat::WiFi | Rat::Ntn));
}

// ---------------------------------------------------------------------------
// Static assertions (compile-time verification)
// ---------------------------------------------------------------------------

// WiFi target supports WiFi-only endpoints
static_assert(Target<Hardware::WiFi>::supports(HardwareSupport::from(Hardware::WiFi)));
// CellWifi does NOT match WiFi-only (different variant)
static_assert(!Target<Hardware::CellWifi>::supports(HardwareSupport::from(Hardware::WiFi)));
// CellWifi matches endpoints that list CellWifi
static_assert(Target<Hardware::CellWifi>::supports(HardwareSupport::from(Hardware::WiFi, Hardware::CellWifi)));
// Universal always matches
static_assert(Target<Hardware::Cell>::supports(HardwareSupport{}));
static_assert(Target<Hardware::LoRa>::supports(HardwareSupport{}));

// Concepts
static_assert(IsTarget<Target<Hardware::Cell>>);
static_assert(IsTarget<MinFirmware<7, 5, 1>>);
static_assert(!IsTarget<Unconstrained>);
static_assert(IsUnconstrained<Unconstrained>);
static_assert(!IsUnconstrained<Target<Hardware::Cell>>);
static_assert(!IsUnconstrained<MinFirmware<7, 5, 1>>);

// HasFirmwareCheck
static_assert(HasFirmwareCheck<MinFirmware<7, 5, 1>>);
static_assert(HasFirmwareCheck<Target<Hardware::WiFi>>);
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

TEST_CASE("MinFirmware: hardware is unconstrained") {
    using Fw = MinFirmware<9, 1, 1>;
    REQUIRE(Fw::supports(HardwareSupport::from(Hardware::WiFi)));
    REQUIRE(Fw::supports(HardwareSupport::from(Hardware::Cell)));
    REQUIRE(Fw::supports(HardwareSupport{}));
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

TEST_CASE("Target with firmware: combined hardware + firmware check") {
    using WiFiFw = Target<Hardware::WiFi, 7, 5, 1>;
    // Hardware check
    REQUIRE(WiFiFw::supports(HardwareSupport::from(Hardware::WiFi)));
    REQUIRE_FALSE(WiFiFw::supports(HardwareSupport::from(Hardware::Cell)));
    // Firmware check
    REQUIRE(WiFiFw::firmware_ok(Firmware{7, 5, 1}));
    REQUIRE(WiFiFw::firmware_ok(Firmware{5, 0, 0}));
    REQUIRE(WiFiFw::firmware_ok(Firmware{}));
    REQUIRE_FALSE(WiFiFw::firmware_ok(Firmware{8, 0, 0}));
}

TEST_CASE("Target without firmware: firmware_ok always true") {
    using WiFi = Target<Hardware::WiFi>;
    REQUIRE(WiFi::firmware_ok(Firmware{99, 99, 99}));
    REQUIRE(WiFi::firmware_ok(Firmware{}));
}

TEST_CASE("target<Hardware, Major, Minor, Patch>() factory") {
    auto t = target<Hardware::Cell, 5, 3, 1>();
    REQUIRE(decltype(t)::hardware == Hardware::Cell);
    REQUIRE(decltype(t)::firmware_version.as_int() == 50301u);
}

// ---------------------------------------------------------------------------
// target_supports — combined hardware + firmware check
// ---------------------------------------------------------------------------

namespace {
    // Mock endpoint types for target_supports tests
    struct UniversalEndpoint {
        static constexpr HardwareSupport hardware{};
        static constexpr Firmware min_firmware{};
    };
    struct WiFiOnlyEndpoint {
        static constexpr HardwareSupport hardware = HardwareSupport::from(Hardware::WiFi);
        static constexpr Firmware min_firmware{};
    };
    struct FirmwareGatedEndpoint {
        static constexpr HardwareSupport hardware{};
        static constexpr Firmware min_firmware{9, 1, 1};
    };
    struct WiFiFirmwareEndpoint {
        static constexpr HardwareSupport hardware = HardwareSupport::from(Hardware::WiFi);
        static constexpr Firmware min_firmware{7, 5, 1};
    };
}

// Unconstrained supports everything
static_assert(target_supports<Unconstrained, UniversalEndpoint>());
static_assert(target_supports<Unconstrained, WiFiOnlyEndpoint>());
static_assert(target_supports<Unconstrained, FirmwareGatedEndpoint>());

// Hardware-only target: checks hardware, ignores firmware
static_assert(target_supports<Target<Hardware::WiFi>, UniversalEndpoint>());
static_assert(target_supports<Target<Hardware::WiFi>, WiFiOnlyEndpoint>());
static_assert(target_supports<Target<Hardware::WiFi>, FirmwareGatedEndpoint>());
static_assert(!target_supports<Target<Hardware::Cell>, WiFiOnlyEndpoint>());

// Firmware-only target: checks firmware, ignores hardware
static_assert(target_supports<MinFirmware<9, 1, 1>, UniversalEndpoint>());
static_assert(target_supports<MinFirmware<9, 1, 1>, FirmwareGatedEndpoint>());
static_assert(target_supports<MinFirmware<9, 1, 1>, WiFiOnlyEndpoint>());
static_assert(!target_supports<MinFirmware<5, 0, 0>, FirmwareGatedEndpoint>());

// Combined target: both must pass
static_assert(target_supports<Target<Hardware::WiFi, 9, 1, 1>, WiFiFirmwareEndpoint>());
static_assert(!target_supports<Target<Hardware::Cell, 9, 1, 1>, WiFiFirmwareEndpoint>());
static_assert(!target_supports<Target<Hardware::WiFi, 5, 0, 0>, WiFiFirmwareEndpoint>());

#endif // C++20
