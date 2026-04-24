#pragma once

#include <cstdint>
#if __cplusplus >= 202002L
#include <concepts>
#endif

namespace note {

// ---------------------------------------------------------------------------
// Radios — named product families identified by their radio set
//
// Each value represents a Blues Notecard product family. The values are
// named after the radio combination the family carries, and the set also
// determines the API-endpoint surface (e.g. WiFi-v2 / NOTE-ESP has
// card.sleep; Cell+WiFi does not, even though both can talk WiFi).
//
// Matches the family labels Blues uses in its own API documentation.
// ---------------------------------------------------------------------------

enum class Radios : uint8_t {
    Cell     = 1 << 0,  // cellular-only family (narrowband / mid / wide)
    WiFi     = 1 << 1,  // WiFi-v2 family (NOTE-ESP)
    CellWifi = 1 << 2,  // cellular + integrated WiFi (W-suffix SKUs)
    LoRa     = 1 << 3,  // LoRa family (NOTE-LWEU / NOTE-LWUS)
    Skylo    = 1 << 4,  // integrated satellite (X-suffix SKUs) — Blues product line name
};

// ---------------------------------------------------------------------------
// RadiosSupport — endpoint support bitmask
//
// Each endpoint carries a RadiosSupport bitmask of which product families
// support it. An empty mask (variants == 0) means universally supported.
// ---------------------------------------------------------------------------

struct RadiosSupport {
    uint8_t variants{};  // bitmask of Radios enum values

    /// True if the target family can use this endpoint.
    /// variants == 0 means universal — supported by all targets.
    constexpr bool supports(Radios target) const {
        return variants == 0
            || (variants & static_cast<uint8_t>(target)) == static_cast<uint8_t>(target);
    }

    /// Construct from individual Radios values.
    static constexpr RadiosSupport from(Radios r) {
        return RadiosSupport{static_cast<uint8_t>(r)};
    }

    template<typename... Rs>
    static constexpr RadiosSupport from(Radios r, Rs... rest) {
        return RadiosSupport{static_cast<uint8_t>(
            static_cast<uint8_t>(r) | from(rest...).variants)};
    }
};

// ---------------------------------------------------------------------------
// Mcu — internal Notecard microcontroller
//
// Per Blues: Notecard revision 1.x uses STM32L4; 2.x uses STM32U5xxx
// unless otherwise identified. Explicit exceptions are NOTE-ESP (ESP32-S3)
// and the NOTE-LORA family (STM32WL / STM32WLE5).
//
// Mcu is useful as a coarse compile-time filter (e.g. STM32L4 is known to
// lack CTX/RTX pins entirely), but the authoritative per-SKU story lives
// in `NotecardSku` / `SkuInfo` below.
// ---------------------------------------------------------------------------

enum class Mcu : uint8_t {
    Unknown = 0,
    Stm32L4,
    Stm32U5,
    Stm32Wl,
    Stm32Wle5,
    Esp32S3,
};

// ---------------------------------------------------------------------------
// TxnPinSupport — does this SKU expose the CTX/RTX transaction-handshake pins?
//
// Some SKUs require the host to drive RTX high and wait for CTX high before
// each transaction (and release RTX afterwards so the Notecard can sleep).
// See note-arduino's `NoteTxn_Arduino` and note-c's `NoteSetFnTransaction`.
// ---------------------------------------------------------------------------

enum class TxnPinSupport : uint8_t {
    Unknown = 0,   ///< Not confirmed — treat as possibly-required.
    None,          ///< No CTX/RTX pins on this SKU. Handshake compiles out.
    Muxed,         ///< Available via AUX2/AUX3 after card.aux configuration.
    Dedicated,     ///< Dedicated, always-available pins.
};

// ---------------------------------------------------------------------------
// SkuInfo — per-SKU capability record
//
// The `NotecardSku` enum and `info_for()` lookup are generated from
// `tools/codegen/metadata/skus.json` into `note/sku_info.hpp`.
// ---------------------------------------------------------------------------

struct SkuInfo {
    Radios         radios;
    Mcu            mcu;
    TxnPinSupport  txn;
};

// ---------------------------------------------------------------------------
// RadiosType / McuType — single-dimension compile-time target wrappers
//
// These wrap an enum value as a distinct type so CTAD on Notecard can
// deduce the correct template parameter from a call-site factory variable.
// The factory namespaces (`note::radios::*` / `note::mcu::*`) declare the
// idiomatic constants to pass at construction sites.
//
// `McuType::has_txn_pins` is the coarse MCU-level filter (STM32L4 is
// known to never have the CTX/RTX pins); use `SkuType` (in sku_info.hpp)
// for authoritative per-SKU answers.
// ---------------------------------------------------------------------------

template<Radios R>
struct RadiosType {
    static constexpr Radios value = R;
};

template<Mcu M>
struct McuType {
    static constexpr Mcu value = M;

    /// Coarse MCU-level CTX/RTX availability. STM32L4 is known to never have
    /// the pins; STM32U5 is variable by SKU (defaults to "possibly"); the
    /// dedicated-pin MCUs (ESP32, STM32WL/STM32WLE5) always have them.
    static constexpr bool has_txn_pins = (M != Mcu::Stm32L4 && M != Mcu::Unknown);
};

// ---------------------------------------------------------------------------
// Axis factory namespaces
//
// Users construct Notecard wrappers by passing axis values at the call site.
// Each axis namespace carries one constant per enum value, no angle brackets.
//
//   note::arduino::Notecard nc(note::radios::WIFI);
//   note::arduino::Notecard nc(note::mcu::STM32L4);
//   note::arduino::Notecard nc(note::sku::NOTE_ESP);  // in sku_info.hpp
//
// Names follow Blues' own conventions (`radios` are the product-family
// labels from the API docs; `mcu` matches common MCU part names).
// ---------------------------------------------------------------------------

namespace radios {
inline constexpr auto CELL      = RadiosType<Radios::Cell>{};
inline constexpr auto WIFI      = RadiosType<Radios::WiFi>{};
inline constexpr auto CELL_WIFI = RadiosType<Radios::CellWifi>{};
inline constexpr auto LORA      = RadiosType<Radios::LoRa>{};
inline constexpr auto SKYLO     = RadiosType<Radios::Skylo>{};
} // namespace radios

namespace mcu {
inline constexpr auto STM32L4   = McuType<Mcu::Stm32L4>{};
inline constexpr auto STM32U5   = McuType<Mcu::Stm32U5>{};
inline constexpr auto STM32WL   = McuType<Mcu::Stm32Wl>{};
inline constexpr auto STM32WLE5 = McuType<Mcu::Stm32Wle5>{};
inline constexpr auto ESP32S3   = McuType<Mcu::Esp32S3>{};
} // namespace mcu

// ---------------------------------------------------------------------------
// Firmware — version triple for compile-time firmware gating
// ---------------------------------------------------------------------------

struct Firmware {
    uint8_t major{};
    uint8_t minor{};
    uint8_t patch{};

    constexpr uint32_t as_int() const {
        return major * 10000u + minor * 100u + patch;
    }

    constexpr bool operator>=(const Firmware& other) const {
        return as_int() >= other.as_int();
    }
    constexpr bool operator<(const Firmware& other) const {
        return as_int() < other.as_int();
    }
    constexpr bool operator==(const Firmware& other) const {
        return as_int() == other.as_int();
    }
};

// ---------------------------------------------------------------------------
// Target types
// ---------------------------------------------------------------------------

/// Unconstrained target — all endpoints available, no filtering.
struct Unconstrained {};

#if __cplusplus >= 202002L

/// Minimum firmware version constraint.
/// Use as a standalone Api target or combine with a Radios target.
///
///   Api api(nc, min_firmware<7,5,1>());          // firmware only
///   Api api(nc, target<Radios::WiFi>());         // radios only
///   Api api(nc, target<Radios::WiFi, 7,5,1>());  // both
///
template<unsigned Major, unsigned Minor = 0, unsigned Patch = 0, bool Strict = false>
struct MinFirmware {
    static constexpr Firmware version{
        static_cast<uint8_t>(Major),
        static_cast<uint8_t>(Minor),
        static_cast<uint8_t>(Patch)};
    static constexpr bool strict = Strict;

    // Radios: unconstrained (accepts everything)
    static constexpr Radios radios{};
    static constexpr bool supports(RadiosSupport) { return true; }

    // Firmware: constrained
    static constexpr bool firmware_ok(Firmware min) {
        return min.as_int() == 0 || version >= min;
    }

    static constexpr auto as_strict() {
        return MinFirmware<Major, Minor, Patch, true>{};
    }
};

/// Constrained target — endpoints filtered by product family and/or firmware.
/// When Strict=false (default), unsupported endpoints get [[deprecated]].
/// When Strict=true, unsupported endpoints are removed via requires.
template<Radios R, unsigned FwMajor = 0, unsigned FwMinor = 0,
         unsigned FwPatch = 0, bool Strict = false>
struct Target {
    static constexpr Radios radios = R;
    static constexpr bool strict = Strict;

    static constexpr Firmware firmware_version{
        static_cast<uint8_t>(FwMajor),
        static_cast<uint8_t>(FwMinor),
        static_cast<uint8_t>(FwPatch)};

    static constexpr bool supports(RadiosSupport s) { return s.supports(radios); }

    // Firmware: constrained only if version was specified (non-zero)
    static constexpr bool firmware_ok(Firmware min) {
        if constexpr (FwMajor == 0 && FwMinor == 0 && FwPatch == 0)
            return true;  // no firmware constraint
        else
            return min.as_int() == 0 || firmware_version >= min;
    }

    static constexpr auto as_strict() {
        return Target<R, FwMajor, FwMinor, FwPatch, true>{};
    }
};

// ---------------------------------------------------------------------------
// Factory helpers
// ---------------------------------------------------------------------------

/// Radios-only target:  target<Radios::WiFi>()
template<Radios R>
constexpr auto target() {
    return Target<R>{};
}

/// Radios + firmware target:  target<Radios::WiFi, 7, 5, 1>()
template<Radios R, unsigned Major, unsigned Minor = 0, unsigned Patch = 0>
constexpr auto target() {
    return Target<R, Major, Minor, Patch>{};
}

/// Firmware-only target:  min_firmware<7, 5, 1>()
template<unsigned Major, unsigned Minor = 0, unsigned Patch = 0>
constexpr auto min_firmware() {
    return MinFirmware<Major, Minor, Patch>{};
}

// ---------------------------------------------------------------------------
// Concepts
// ---------------------------------------------------------------------------

template<typename T>
concept IsTarget = requires {
    { T::radios } -> std::convertible_to<Radios>;
    { T::strict } -> std::convertible_to<bool>;
};

/// True if the target has a firmware_ok() method (all targets except Unconstrained).
template<typename T>
concept HasFirmwareCheck = requires(Firmware fw) {
    { T::firmware_ok(fw) } -> std::convertible_to<bool>;
};

template<typename T>
concept IsUnconstrained = std::same_as<T, Unconstrained>;

/// Check if a target supports an endpoint (radios + firmware).
/// E must have static members `radios` (RadiosSupport) and `min_firmware` (Firmware).
template<typename T, typename E>
consteval bool target_supports() {
    if constexpr (IsUnconstrained<T>) return true;
    else return T::supports(E::radios) && T::firmware_ok(E::min_firmware);
}

#endif // C++20

} // namespace note
