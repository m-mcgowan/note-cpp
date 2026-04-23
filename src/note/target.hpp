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
// Hardware — named Notecard hardware variants (composites of RATs)
//
// Each variant is a distinct Notecard hardware family. Variants with the
// same RAT capabilities may still have different API surfaces (e.g. the
// WiFi v2 has card.sleep but a Cell+WiFi Notecard does not).
// ---------------------------------------------------------------------------

enum class Hardware : uint8_t {
    Cell     = 1 << 0,              // Cell-only Notecard
    WiFi     = 1 << 1,              // WiFi v2 (ESP32)
    CellWifi = 1 << 2,             // Cell+WiFi dual Notecard
    LoRa     = 1 << 3,              // LoRa Notecard
    Skylo    = 1 << 4,              // Satellite (Cell+WiFi+NTN)
};

/// Extract the RAT bitmask from a Hardware variant.
constexpr Rat rats_of(Hardware h) {
    switch (h) {
    case Hardware::Cell:     return Rat::Cell;
    case Hardware::WiFi:     return Rat::WiFi;
    case Hardware::CellWifi: return Rat::Cell | Rat::WiFi;
    case Hardware::LoRa:     return Rat::LoRa;
    case Hardware::Skylo:    return Rat::Cell | Rat::WiFi | Rat::Ntn;
    }
    return {};
}

// ---------------------------------------------------------------------------
// HardwareSupport — hardware support set carried on each endpoint
//
// Stores a bitmask of which Hardware variants support this endpoint. This
// is distinct from RATs: a Cell+WiFi Notecard has WiFi capability but is a
// different variant from the WiFi v2, and may not support WiFi-v2-only
// endpoints like card.sleep.
// ---------------------------------------------------------------------------

struct HardwareSupport {
    uint8_t variants{};  // bitmask of Hardware enum values

    /// True if a target hardware variant can use this endpoint.
    /// variants == 0 means universal — supported by all targets.
    constexpr bool supports(Hardware target) const {
        return variants == 0
            || (variants & static_cast<uint8_t>(target)) == static_cast<uint8_t>(target);
    }

    /// Construct from individual Hardware values.
    static constexpr HardwareSupport from(Hardware h) {
        return HardwareSupport{static_cast<uint8_t>(h)};
    }

    template<typename... Hs>
    static constexpr HardwareSupport from(Hardware h, Hs... rest) {
        return HardwareSupport{static_cast<uint8_t>(
            static_cast<uint8_t>(h) | from(rest...).variants)};
    }
};

// Backward compatibility aliases
using Product [[deprecated("Use Hardware instead")]] = Hardware;
using Skus [[deprecated("Use HardwareSupport instead")]] = HardwareSupport;

// Forward-looking alias: current `Hardware` enum represents radio
// capability (Cell / WiFi / LoRa / …), not the internal MCU. `Radio` is
// the eventual preferred name; `Hardware` remains as a synonym.
using Radio = Hardware;

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
// TxnPinSupport — does this SKU expose the CTX/RTX transaction-gate pins?
//
// Some SKUs require the host to drive RTX high and wait for CTX high before
// each transaction (and release RTX afterwards so the Notecard can sleep).
// See note-arduino's `NoteTxn_Arduino` and note-c's `NoteSetFnTransaction`.
// ---------------------------------------------------------------------------

enum class TxnPinSupport : uint8_t {
    Unknown = 0,   ///< Not confirmed — treat as possibly-required.
    None,          ///< No CTX/RTX pins on this SKU. Gate compiles out.
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
    Radio          radio;
    Mcu            mcu;
    TxnPinSupport  txn;
};

// ---------------------------------------------------------------------------
// RadioType / McuType — single-dimension compile-time target wrappers
//
// These wrap an enum value as a distinct type so CTAD on Notecard can
// deduce the correct template parameter from a call-site factory variable.
// The factory variables below (`radio<>` / `mcu<>`) are the idiomatic way
// to pass these at construction sites.
//
// Each type exposes the capability traits it can derive from the value it
// wraps. `McuType::has_txn_pins` is the coarse MCU-level filter (STM32L4
// is known to never have the CTX/RTX pins); use `SkuType` (in sku_info.hpp)
// for authoritative per-SKU answers.
// ---------------------------------------------------------------------------

template<Radio R>
struct RadioType {
    static constexpr Radio value = R;
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
// Factory variables
//
//   radio<Radio::Cell>           → RadioType<Radio::Cell>
//   mcu<Mcu::Stm32U5>            → McuType<Mcu::Stm32U5>
//
// SKU factory `sku<NotecardSku::X>` is declared in note/sku_info.hpp.
// ---------------------------------------------------------------------------

template<Radio R> inline constexpr RadioType<R> radio{};
template<Mcu M>   inline constexpr McuType<M>   mcu{};

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
/// Use as a standalone Api target or combine with a hardware Target.
///
///   Api api(nc, min_firmware<7,5,1>());           // firmware only
///   Api api(nc, target<Hardware::WiFi>());         // hardware only
///   Api api(nc, target<Hardware::WiFi, 7,5,1>());  // both
///
template<unsigned Major, unsigned Minor = 0, unsigned Patch = 0, bool Strict = false>
struct MinFirmware {
    static constexpr Firmware version{
        static_cast<uint8_t>(Major),
        static_cast<uint8_t>(Minor),
        static_cast<uint8_t>(Patch)};
    static constexpr bool strict = Strict;

    // Hardware: unconstrained (accepts everything)
    static constexpr Hardware hardware{};
    static constexpr bool supports(HardwareSupport) { return true; }

    // Firmware: constrained
    static constexpr bool firmware_ok(Firmware min) {
        return min.as_int() == 0 || version >= min;
    }

    static constexpr auto as_strict() {
        return MinFirmware<Major, Minor, Patch, true>{};
    }
};

/// Constrained target — endpoints filtered by hardware variant and/or firmware.
/// When Strict=false (default), unsupported endpoints get [[deprecated]].
/// When Strict=true, unsupported endpoints are removed via requires.
template<Hardware H, unsigned FwMajor = 0, unsigned FwMinor = 0,
         unsigned FwPatch = 0, bool Strict = false>
struct Target {
    static constexpr Hardware hardware = H;
    static constexpr Rat rats = rats_of(H);
    static constexpr bool strict = Strict;

    static constexpr Firmware firmware_version{
        static_cast<uint8_t>(FwMajor),
        static_cast<uint8_t>(FwMinor),
        static_cast<uint8_t>(FwPatch)};

    static constexpr bool supports(HardwareSupport s) { return s.supports(hardware); }

    // Firmware: constrained only if version was specified (non-zero)
    static constexpr bool firmware_ok(Firmware min) {
        if constexpr (FwMajor == 0 && FwMinor == 0 && FwPatch == 0)
            return true;  // no firmware constraint
        else
            return min.as_int() == 0 || firmware_version >= min;
    }

    static constexpr auto as_strict() {
        return Target<H, FwMajor, FwMinor, FwPatch, true>{};
    }
};

// ---------------------------------------------------------------------------
// Factory helpers
// ---------------------------------------------------------------------------

/// Hardware-only target:  target<Hardware::WiFi>()
template<Hardware H>
constexpr auto target() {
    return Target<H>{};
}

/// Hardware + firmware target:  target<Hardware::WiFi, 7, 5, 1>()
template<Hardware H, unsigned Major, unsigned Minor = 0, unsigned Patch = 0>
constexpr auto target() {
    return Target<H, Major, Minor, Patch>{};
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
    { T::hardware } -> std::convertible_to<Hardware>;
    { T::strict } -> std::convertible_to<bool>;
};

/// True if the target has a firmware_ok() method (all targets except Unconstrained).
template<typename T>
concept HasFirmwareCheck = requires(Firmware fw) {
    { T::firmware_ok(fw) } -> std::convertible_to<bool>;
};

template<typename T>
concept IsUnconstrained = std::same_as<T, Unconstrained>;

/// Check if a target supports an endpoint (hardware + firmware).
/// E must have static members `hardware` (HardwareSupport) and `min_firmware` (Firmware).
template<typename T, typename E>
consteval bool target_supports() {
    if constexpr (IsUnconstrained<T>) return true;
    else return T::supports(E::hardware) && T::firmware_ok(E::min_firmware);
}

#endif // C++20

} // namespace note
