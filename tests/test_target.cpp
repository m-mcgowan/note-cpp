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

TEST_CASE("Product enum values are unique power-of-two bits") {
    REQUIRE(static_cast<uint8_t>(Product::Cell) == 1);
    REQUIRE(static_cast<uint8_t>(Product::WiFi) == 2);
    REQUIRE(static_cast<uint8_t>(Product::CellWifi) == 4);
    REQUIRE(static_cast<uint8_t>(Product::LoRa) == 8);
    REQUIRE(static_cast<uint8_t>(Product::Skylo) == 16);
}

TEST_CASE("rats_of extracts RAT bitmask from Product") {
    using U = uint8_t;
    REQUIRE(static_cast<U>(rats_of(Product::Cell)) == static_cast<U>(Rat::Cell));
    REQUIRE(static_cast<U>(rats_of(Product::WiFi)) == static_cast<U>(Rat::WiFi));
    REQUIRE(static_cast<U>(rats_of(Product::CellWifi)) == static_cast<U>(Rat::Cell | Rat::WiFi));
    REQUIRE(static_cast<U>(rats_of(Product::LoRa)) == static_cast<U>(Rat::LoRa));
    REQUIRE(static_cast<U>(rats_of(Product::Skylo)) == static_cast<U>(Rat::Cell | Rat::WiFi | Rat::Ntn));
}

// ---------------------------------------------------------------------------
// Skus::supports — product-based matching
// ---------------------------------------------------------------------------

TEST_CASE("Skus: empty is universal") {
    constexpr Skus s{};
    REQUIRE(s.supports(Product::Cell));
    REQUIRE(s.supports(Product::WiFi));
    REQUIRE(s.supports(Product::CellWifi));
    REQUIRE(s.supports(Product::LoRa));
    REQUIRE(s.supports(Product::Skylo));
}

TEST_CASE("Skus: WiFi-only (card.sleep)") {
    constexpr auto s = Skus::from(Product::WiFi);
    REQUIRE(s.supports(Product::WiFi));
    REQUIRE_FALSE(s.supports(Product::CellWifi));  // has WiFi RAT, but different product
    REQUIRE_FALSE(s.supports(Product::Cell));
    REQUIRE_FALSE(s.supports(Product::LoRa));
    REQUIRE_FALSE(s.supports(Product::Skylo));
}

TEST_CASE("Skus: multiple products (card.wifi)") {
    // card.wifi supports WiFi, CellWifi, and Skylo
    constexpr auto s = Skus::from(Product::WiFi, Product::CellWifi, Product::Skylo);
    REQUIRE(s.supports(Product::WiFi));
    REQUIRE(s.supports(Product::CellWifi));
    REQUIRE(s.supports(Product::Skylo));
    REQUIRE_FALSE(s.supports(Product::Cell));
    REQUIRE_FALSE(s.supports(Product::LoRa));
}

TEST_CASE("Skus: most endpoints (all except LoRa)") {
    constexpr auto s = Skus::from(Product::Cell, Product::CellWifi, Product::Skylo, Product::WiFi);
    REQUIRE(s.supports(Product::Cell));
    REQUIRE(s.supports(Product::CellWifi));
    REQUIRE(s.supports(Product::Skylo));
    REQUIRE(s.supports(Product::WiFi));
    REQUIRE_FALSE(s.supports(Product::LoRa));
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

TEST_CASE("Target::supports delegates to Skus with product identity") {
    using WiFiTarget = Target<Product::WiFi>;
    constexpr auto wifi_only = Skus::from(Product::WiFi);
    constexpr auto multi = Skus::from(Product::Cell, Product::WiFi);
    constexpr Skus universal{};

    REQUIRE(WiFiTarget::supports(wifi_only));
    REQUIRE(WiFiTarget::supports(multi));
    REQUIRE(WiFiTarget::supports(universal));
}

TEST_CASE("Target: CellWifi does NOT match WiFi-only endpoints") {
    using CellWifiTarget = Target<Product::CellWifi>;
    constexpr auto wifi_only = Skus::from(Product::WiFi);

    REQUIRE_FALSE(CellWifiTarget::supports(wifi_only));
}

TEST_CASE("Target::as_strict") {
    using T = Target<Product::Cell>;
    using S = decltype(T::as_strict());
    REQUIRE(S::strict);
    REQUIRE(S::product == Product::Cell);
}

TEST_CASE("target<Product>() factory") {
    auto t = target<Product::Skylo>();
    REQUIRE(decltype(t)::product == Product::Skylo);
    // Skylo RATs: Cell|WiFi|Ntn = 1|2|4 = 7
    REQUIRE(static_cast<uint8_t>(decltype(t)::rats) ==
            static_cast<uint8_t>(Rat::Cell | Rat::WiFi | Rat::Ntn));
}

// ---------------------------------------------------------------------------
// Static assertions (compile-time verification)
// ---------------------------------------------------------------------------

// WiFi target supports WiFi-only endpoints
static_assert(Target<Product::WiFi>::supports(Skus::from(Product::WiFi)));
// CellWifi does NOT match WiFi-only (different product)
static_assert(!Target<Product::CellWifi>::supports(Skus::from(Product::WiFi)));
// CellWifi matches endpoints that list CellWifi
static_assert(Target<Product::CellWifi>::supports(Skus::from(Product::WiFi, Product::CellWifi)));
// Universal always matches
static_assert(Target<Product::Cell>::supports(Skus{}));
static_assert(Target<Product::LoRa>::supports(Skus{}));

// Concepts
static_assert(IsTarget<Target<Product::Cell>>);
static_assert(!IsTarget<Unconstrained>);
static_assert(IsUnconstrained<Unconstrained>);
static_assert(!IsUnconstrained<Target<Product::Cell>>);

#endif // C++20
