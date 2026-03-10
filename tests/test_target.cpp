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
// Product values
// ---------------------------------------------------------------------------

TEST_CASE("Product enum values") {
    REQUIRE(static_cast<uint8_t>(Product::Cell) == 1);
    REQUIRE(static_cast<uint8_t>(Product::CellWifi) == 3);
    REQUIRE(static_cast<uint8_t>(Product::WiFi) == 2);
    REQUIRE(static_cast<uint8_t>(Product::LoRa) == 8);
    REQUIRE(static_cast<uint8_t>(Product::Skylo) == 7);
}

// ---------------------------------------------------------------------------
// Product + Rat composition
// ---------------------------------------------------------------------------

TEST_CASE("Product + Rat composition") {
    constexpr auto r = Product::Cell + Rat::Ntn;
    REQUIRE(static_cast<uint8_t>(r) == (1 | 4)); // Cell | Ntn = 5
}

// ---------------------------------------------------------------------------
// Skus::supports
// ---------------------------------------------------------------------------

TEST_CASE("Skus: empty is universal") {
    constexpr Skus s{};
    REQUIRE(s.supports(Rat::Cell));
    REQUIRE(s.supports(Rat::LoRa));
    REQUIRE(s.supports(Rat::WiFi));
}

TEST_CASE("Skus: overlap matching") {
    constexpr Skus s{Rat::Cell | Rat::WiFi};
    REQUIRE(s.supports(Rat::Cell));
    REQUIRE(s.supports(Rat::WiFi));
    REQUIRE_FALSE(s.supports(Rat::LoRa));
    REQUIRE_FALSE(s.supports(Rat::Ntn));
}

TEST_CASE("Skus: composite target overlaps") {
    constexpr Skus s{Rat::Cell | Rat::WiFi};
    constexpr auto target_rats = Rat::Cell | Rat::Ntn;
    REQUIRE(s.supports(target_rats)); // Cell overlaps
}

// ---------------------------------------------------------------------------
// Firmware
// ---------------------------------------------------------------------------

TEST_CASE("Firmware::as_int") {
    constexpr Firmware fw{5, 3, 1};
    REQUIRE(fw.as_int() == 50301u);
}

// ---------------------------------------------------------------------------
// C++20-only: Target, target<>(), concepts
// ---------------------------------------------------------------------------

#if __cplusplus >= 202002L

TEST_CASE("Target::supports delegates to Skus") {
    using CellTarget = Target<Rat::Cell>;
    constexpr Skus cell_wifi{Rat::Cell | Rat::WiFi};
    constexpr Skus wifi_only{Rat::WiFi};
    constexpr Skus universal{};

    REQUIRE(CellTarget::supports(cell_wifi));
    REQUIRE_FALSE(CellTarget::supports(wifi_only));
    REQUIRE(CellTarget::supports(universal));
}

TEST_CASE("Target::as_strict") {
    using T = Target<Rat::Cell>;
    using S = decltype(T::as_strict());
    REQUIRE(S::strict);
    REQUIRE(static_cast<uint8_t>(S::rats) == static_cast<uint8_t>(Rat::Cell));
}

TEST_CASE("target<Product>() factory") {
    auto t = target<Product::Skylo>();
    REQUIRE(static_cast<uint8_t>(decltype(t)::rats) == 7); // Cell|WiFi|Ntn
}

TEST_CASE("target<Rat>() factory") {
    auto t = target<Rat::LoRa>();
    REQUIRE(static_cast<uint8_t>(decltype(t)::rats) == 8);
}

// ---------------------------------------------------------------------------
// Static assertions (compile-time verification)
// ---------------------------------------------------------------------------

static_assert(Target<Rat::Cell>::supports(Skus{Rat::Cell | Rat::WiFi}));
static_assert(!Target<Rat::Cell>::supports(Skus{Rat::WiFi}));
static_assert(Target<Rat::Cell>::supports(Skus{})); // universal
static_assert(!Target<Rat::LoRa>::supports(Skus{Rat::Cell | Rat::WiFi | Rat::Ntn}));
static_assert(Target<Rat::Cell | Rat::WiFi>::supports(Skus{Rat::WiFi}));

// Concepts
static_assert(IsTarget<Target<Rat::Cell>>);
static_assert(!IsTarget<Unconstrained>);
static_assert(IsUnconstrained<Unconstrained>);
static_assert(!IsUnconstrained<Target<Rat::Cell>>);

#endif // C++20
