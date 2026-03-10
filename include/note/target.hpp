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
// ---------------------------------------------------------------------------

enum class Product : uint8_t {
    Cell     = 1,                   // Cell
    CellWifi = 3,                   // Cell | WiFi
    WiFi     = 2,                   // WiFi
    LoRa     = 8,                   // LoRa
    Skylo    = 7,                   // Cell | WiFi | Ntn
};

/// Compose a Product with an additional Rat.
/// Example: Product::Cell + Rat::Ntn → Cell|Ntn
constexpr Rat operator+(Product p, Rat r) {
    return static_cast<Rat>(static_cast<uint8_t>(p)) | r;
}

// ---------------------------------------------------------------------------
// Skus — SKU support set carried on each endpoint
// ---------------------------------------------------------------------------

struct Skus {
    Rat rats{};

    /// True if a target with the given RATs can use this endpoint.
    /// Empty rats (0) means universal — supported by all targets.
    constexpr bool supports(Rat target) const {
        return static_cast<uint8_t>(rats) == 0
            || (static_cast<uint8_t>(rats) & static_cast<uint8_t>(target)) != 0;
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

/// Constrained target — endpoints are filtered by RAT overlap.
/// When Strict=false (default), unsupported endpoints get [[deprecated]].
/// When Strict=true, unsupported endpoints are removed via requires.
template<Rat Rats, bool Strict = false>
struct Target {
    static constexpr Rat rats = Rats;
    static constexpr bool strict = Strict;

    static constexpr bool supports(Skus s) { return s.supports(rats); }

    static constexpr auto as_strict() { return Target<Rats, true>{}; }
};

// ---------------------------------------------------------------------------
// Factory helpers — use with constexpr/NTTP arguments:
//   make_api(nc, target<Product::Cell>())
//   make_api(nc, target<Product::Cell + Rat::Ntn>())
// ---------------------------------------------------------------------------

template<Product P>
constexpr auto target() {
    return Target<static_cast<Rat>(static_cast<uint8_t>(P))>{};
}

template<Rat R>
constexpr auto target() {
    return Target<R>{};
}

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

template<typename T>
concept IsTarget = requires {
    { T::rats } -> std::convertible_to<Rat>;
    { T::strict } -> std::convertible_to<bool>;
};

template<typename T>
concept IsUnconstrained = std::same_as<T, Unconstrained>;

#endif // C++20

} // namespace note
