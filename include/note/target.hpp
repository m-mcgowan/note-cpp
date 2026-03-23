#pragma once

#include <cstdint>
#if __cplusplus >= 202002L
#include <concepts>
#endif

namespace note {

// ---------------------------------------------------------------------------
// Rat — Radio Access Technology bitfield
// ---------------------------------------------------------------------------

enum class Rat : uint8_t {
    Cell = 1,
    WiFi = 2,
    Ntn  = 4,
    LoRa = 8,
};

constexpr Rat operator|(Rat a, Rat b) {
    return static_cast<Rat>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr Rat operator&(Rat a, Rat b) {
    return static_cast<Rat>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}
constexpr bool has_rat(Rat set, Rat test) {
    return (static_cast<uint8_t>(set) & static_cast<uint8_t>(test)) != 0;
}

// ---------------------------------------------------------------------------
// Product — named Notecard products (composites of RATs)
//
// Each product is a specific Notecard SKU. Products with the same RAT
// capabilities may still have different API surfaces (e.g. the WiFi v2
// has card.sleep but a Cell+WiFi Notecard does not).
// ---------------------------------------------------------------------------

enum class Product : uint8_t {
    Cell     = 1 << 0,              // Cell-only Notecard
    WiFi     = 1 << 1,              // WiFi v2 (ESP32)
    CellWifi = 1 << 2,             // Cell+WiFi dual Notecard
    LoRa     = 1 << 3,              // LoRa Notecard
    Skylo    = 1 << 4,              // Satellite (Cell+WiFi+NTN)
};

/// Extract the RAT bitmask from a Product.
constexpr Rat rats_of(Product p) {
    switch (p) {
    case Product::Cell:     return Rat::Cell;
    case Product::WiFi:     return Rat::WiFi;
    case Product::CellWifi: return Rat::Cell | Rat::WiFi;
    case Product::LoRa:     return Rat::LoRa;
    case Product::Skylo:    return Rat::Cell | Rat::WiFi | Rat::Ntn;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Skus — product support set carried on each endpoint
//
// Stores a bitmask of which Products support this endpoint. This is
// distinct from RATs: a Cell+WiFi Notecard has WiFi capability but is a
// different product from the WiFi v2, and may not support WiFi-v2-only
// endpoints like card.sleep.
// ---------------------------------------------------------------------------

struct Skus {
    uint8_t products{};  // bitmask of Product enum values

    /// True if a target product can use this endpoint.
    /// products == 0 means universal — supported by all targets.
    constexpr bool supports(Product target) const {
        return products == 0
            || (products & static_cast<uint8_t>(target)) == static_cast<uint8_t>(target);
    }

    /// Construct from individual Product values.
    static constexpr Skus from(Product p) {
        return Skus{static_cast<uint8_t>(p)};
    }

    template<typename... Ps>
    static constexpr Skus from(Product p, Ps... rest) {
        return Skus{static_cast<uint8_t>(
            static_cast<uint8_t>(p) | from(rest...).products)};
    }
};

// ---------------------------------------------------------------------------
// Firmware — version triple (reserved for future min-firmware gating)
// ---------------------------------------------------------------------------

struct Firmware {
    uint8_t major{};
    uint8_t minor{};
    uint8_t patch{};

    constexpr uint32_t as_int() const {
        return major * 10000u + minor * 100u + patch;
    }
};

// ---------------------------------------------------------------------------
// Target types
// ---------------------------------------------------------------------------

/// Unconstrained target — all endpoints available, no filtering.
struct Unconstrained {};

#if __cplusplus >= 202002L

/// Constrained target — endpoints filtered by product identity.
/// When Strict=false (default), unsupported endpoints get [[deprecated]].
/// When Strict=true, unsupported endpoints are removed via requires.
template<Product P, bool Strict = false>
struct Target {
    static constexpr Product product = P;
    static constexpr Rat rats = rats_of(P);
    static constexpr bool strict = Strict;

    static constexpr bool supports(Skus s) { return s.supports(product); }

    static constexpr auto as_strict() { return Target<P, true>{}; }
};

// ---------------------------------------------------------------------------
// Factory helpers — use with constexpr/NTTP arguments:
//   make_api(nc, target<Product::Cell>())
//   make_api(nc, target<Product::WiFi>())
// ---------------------------------------------------------------------------

template<Product P>
constexpr auto target() {
    return Target<P>{};
}

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

template<typename T>
concept IsTarget = requires {
    { T::product } -> std::convertible_to<Product>;
    { T::strict } -> std::convertible_to<bool>;
};

template<typename T>
concept IsUnconstrained = std::same_as<T, Unconstrained>;

#endif // C++20

} // namespace note
