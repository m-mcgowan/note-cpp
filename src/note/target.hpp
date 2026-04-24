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
// Axis category tags — classify a type as one of the four composition axes
//
// Each axis type (RadiosType / McuType / FwConstraint / SkuType) carries a
// `using axis_category = ...;` typedef. ComposedTarget queries the tag to
// extract the right dimensions without needing per-axis template
// specializations. SkuType lives in sku_info.hpp (codegenned) and picks up
// its tag there.
// ---------------------------------------------------------------------------

struct radios_axis {};
struct mcu_axis {};
struct fw_axis {};
struct sku_axis {};

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
    using axis_category = radios_axis;
    static constexpr Radios value = R;
};

template<Mcu M>
struct McuType {
    using axis_category = mcu_axis;
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
//   note::arduino::Notecard nc(note::radios::NOTE_WIFI);
//   note::arduino::Notecard nc(note::mcu::NOTE_STM32L4);
//   note::arduino::Notecard nc(note::sku::NOTE_ESP);  // in sku_info.hpp
//
// The `NOTE_` prefix mirrors the sku namespace and is mandatory, not
// stylistic: Arduino cores for STM32 / ESP32 boards define bare names
// like `STM32L4`, `ESP32S3`, `WIFI` as preprocessor macros for HAL
// conditional compilation. Those macros replace tokens before namespace
// resolution, so even a fully qualified `note::mcu::STM32L4` would
// substitute to `note::mcu::(1)` and fail to compile. Prefixing with
// `NOTE_` keeps the identifier collision-free while preserving Blues'
// all-caps axis-constant convention.
// ---------------------------------------------------------------------------

namespace radios {
inline constexpr auto NOTE_CELL      = RadiosType<Radios::Cell>{};
inline constexpr auto NOTE_WIFI      = RadiosType<Radios::WiFi>{};
inline constexpr auto NOTE_CELL_WIFI = RadiosType<Radios::CellWifi>{};
inline constexpr auto NOTE_LORA      = RadiosType<Radios::LoRa>{};
inline constexpr auto NOTE_SKYLO     = RadiosType<Radios::Skylo>{};
} // namespace radios

namespace mcu {
inline constexpr auto NOTE_STM32L4   = McuType<Mcu::Stm32L4>{};
inline constexpr auto NOTE_STM32U5   = McuType<Mcu::Stm32U5>{};
inline constexpr auto NOTE_STM32WL   = McuType<Mcu::Stm32Wl>{};
inline constexpr auto NOTE_STM32WLE5 = McuType<Mcu::Stm32Wle5>{};
inline constexpr auto NOTE_ESP32S3   = McuType<Mcu::Esp32S3>{};
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
// FwConstraint<Major, Minor, Patch> — firmware axis tag
//
// Wraps a Firmware version as a distinct type so the variadic Notecard CTAD
// can deduce the firmware dimension from a call-site constant. Codegen emits
// one `inline constexpr` per unique min-firmware value in the spec into
// `note/fw_versions.hpp` under `namespace note::fw`:
//
//   note::arduino::Notecard nc(note::fw::v7_5_1);
//
// Unlike `MinFirmware`, this is only a tag — ComposedTarget extracts
// `version` and provides the Target contract.
// ---------------------------------------------------------------------------

template<unsigned Major, unsigned Minor = 0, unsigned Patch = 0>
struct FwConstraint {
    using axis_category = fw_axis;
    static constexpr Firmware version{
        static_cast<uint8_t>(Major),
        static_cast<uint8_t>(Minor),
        static_cast<uint8_t>(Patch)};
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

// ---------------------------------------------------------------------------
// ComposedTarget — variadic axis composition
//
// Users pass a pack of axis values at Notecard construction (CTAD in Task 6).
// Each axis type (RadiosType / McuType / FwConstraint / SkuType) contributes
// one or more dimensions that are folded into a single Target-contract type:
//
//   Notecard nc(sku::NOTE_ESP, fw::v7_5_1);
//   Notecard nc(sku::NOTE_ESP, sku::NOTE_NBGLWX);   // multi-SKU intersection
//   Notecard nc(radios::NOTE_WIFI);
//
// Fold rules
//   firmware      max of every FwConstraint's version
//   has_txn_pins  AND of every SKU/MCU axis's has_txn_pins
//   supports(s)   every radios-contributing axis must be in s
//
// Cross-axis conflict: an explicit RadiosType disagreeing with a SKU's
// implied radios is a static_assert. Multi-SKU with differing radios is
// legal (the supports() AND-fold over-constrains naturally). Redundancy
// (consistent) compiles fine.
//
// `ComposedTarget<>` (empty pack) behaves universally (supports everything,
// no firmware constraint) without being the same type as `Unconstrained`.
// ---------------------------------------------------------------------------

namespace detail {

template<typename T>
concept HasAxisCategory = requires { typename T::axis_category; };

template<typename T>
concept IsRadiosAxis = HasAxisCategory<T>
    && std::same_as<typename T::axis_category, radios_axis>;

template<typename T>
concept IsMcuAxis = HasAxisCategory<T>
    && std::same_as<typename T::axis_category, mcu_axis>;

template<typename T>
concept IsFwAxis = HasAxisCategory<T>
    && std::same_as<typename T::axis_category, fw_axis>;

template<typename T>
concept IsSkuAxis = HasAxisCategory<T>
    && std::same_as<typename T::axis_category, sku_axis>;

/// Axis contributes a Radios value (either explicit RadiosType or SKU's implied).
template<typename Ax>
constexpr bool axis_has_radios = IsRadiosAxis<Ax> || IsSkuAxis<Ax>;

/// Extract the Radios value contributed by an axis; callers should gate
/// via `axis_has_radios<Ax>` first.
template<typename Ax>
consteval Radios radios_of() {
    if constexpr (IsRadiosAxis<Ax>) return Ax::value;
    else if constexpr (IsSkuAxis<Ax>) return Ax::radios;
    else return Radios{};
}

/// Axis contributes a has_txn_pins bit (SKU or MCU).
template<typename Ax>
constexpr bool axis_has_txn_pins = IsSkuAxis<Ax> || IsMcuAxis<Ax>;

template<typename Ax>
consteval bool txn_pins_of() {
    if constexpr (IsSkuAxis<Ax>) return Ax::has_txn_pins;
    else if constexpr (IsMcuAxis<Ax>) return Ax::has_txn_pins;
    else return true;
}

}  // namespace detail

/// Variadic composition target. Users should use the `ComposedTarget<Axes...>`
/// alias; the implementation takes an explicit Strict flag so `.as_strict()`
/// can flip it without producing a new template.
template<bool Strict, typename... Axes>
struct ComposedTargetImpl {
    static_assert(
        (detail::HasAxisCategory<Axes> && ...),
        "ComposedTarget axes must be recognized axis types "
        "(SkuType, RadiosType, McuType, or FwConstraint).");

    // --- Folded dimensions ---------------------------------------------

    static consteval Radios _compute_radios() {
        Radios r{};
        ([&] {
            if constexpr (detail::axis_has_radios<Axes>) {
                if (static_cast<uint8_t>(r) == 0) r = detail::radios_of<Axes>();
            }
        }(), ...);
        return r;
    }

    static consteval Firmware _compute_firmware() {
        Firmware fw{};
        ([&] {
            if constexpr (detail::IsFwAxis<Axes>) {
                if (Axes::version.as_int() > fw.as_int()) fw = Axes::version;
            }
        }(), ...);
        return fw;
    }

    static consteval bool _compute_txn_pins() {
        bool result = true;
        ([&] {
            if constexpr (detail::axis_has_txn_pins<Axes>) {
                if (!detail::txn_pins_of<Axes>()) result = false;
            }
        }(), ...);
        return result;
    }

    // Representative Radios value (first radios-contributing axis's value,
    // else Radios{} — meaning "no radios constraint declared").
    static constexpr Radios radios = _compute_radios();
    static constexpr Firmware firmware_version = _compute_firmware();
    static constexpr bool has_txn_pins = _compute_txn_pins();
    static constexpr bool strict = Strict;

    // --- Cross-axis conflict check -------------------------------------
    // Explicit RadiosType value (0 if none provided).
    static consteval Radios _explicit_radios() {
        Radios r{};
        ([&] {
            if constexpr (detail::IsRadiosAxis<Axes>) {
                if (static_cast<uint8_t>(r) == 0) r = Axes::value;
            }
        }(), ...);
        return r;
    }

    static consteval bool _radios_consistent() {
        constexpr Radios explicit_r = _explicit_radios();
        if (static_cast<uint8_t>(explicit_r) == 0) return true;
        bool ok = true;
        ([&] {
            if constexpr (detail::IsSkuAxis<Axes>) {
                if (Axes::radios != explicit_r) ok = false;
            } else if constexpr (detail::IsRadiosAxis<Axes>) {
                if (Axes::value != explicit_r) ok = false;
            }
        }(), ...);
        return ok;
    }

    static_assert(_radios_consistent(),
        "ComposedTarget: an explicit RadiosType axis disagrees with another "
        "radios-contributing axis (e.g. sku::NOTE_ESP + radios::NOTE_CELL). "
        "Remove the conflicting axis.");

    // Explicit McuType value (Mcu::Unknown if none provided).
    static consteval Mcu _explicit_mcu() {
        Mcu m{};
        ([&] {
            if constexpr (detail::IsMcuAxis<Axes>) {
                if (m == Mcu{}) m = Axes::value;
            }
        }(), ...);
        return m;
    }

    static consteval bool _mcu_consistent() {
        constexpr Mcu explicit_m = _explicit_mcu();
        if (explicit_m == Mcu{}) return true;
        bool ok = true;
        ([&] {
            if constexpr (detail::IsSkuAxis<Axes>) {
                if (Axes::mcu != explicit_m) ok = false;
            } else if constexpr (detail::IsMcuAxis<Axes>) {
                if (Axes::value != explicit_m) ok = false;
            }
        }(), ...);
        return ok;
    }

    static_assert(_mcu_consistent(),
        "ComposedTarget: an explicit McuType axis disagrees with another "
        "MCU-contributing axis (e.g. sku::NOTE_ESP + mcu::NOTE_STM32L4 — "
        "NOTE_ESP uses ESP32-S3, not STM32L4). Remove the conflicting axis.");

    // --- Target contract -----------------------------------------------
    /// Endpoint supported iff every radios-contributing axis is in `s`.
    /// Empty pack → true (universal, matches Unconstrained semantically).
    static constexpr bool supports(RadiosSupport s) {
        bool ok = true;
        ([&] {
            if constexpr (detail::axis_has_radios<Axes>) {
                if (!s.supports(detail::radios_of<Axes>())) ok = false;
            }
        }(), ...);
        return ok;
    }

    /// Firmware check: pass if we declared no constraint, or if our
    /// derived max meets the endpoint's minimum.
    static constexpr bool firmware_ok(Firmware min) {
        if (min.as_int() == 0) return true;
        if (firmware_version.as_int() == 0) return true;  // no fw constraint
        return firmware_version >= min;
    }

    static constexpr auto as_strict() {
        return ComposedTargetImpl<true, Axes...>{};
    }
};

template<typename... Axes>
using ComposedTarget = ComposedTargetImpl<false, Axes...>;

#endif // C++20

} // namespace note
