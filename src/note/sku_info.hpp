// AUTO-GENERATED from tools/codegen/metadata/skus.json by tools/codegen/generate.py
// DO NOT EDIT directly — edit the JSON table and regenerate.
//
// Declares `note::NotecardSku` (enum of known Notecard product codes) and
// `note::info_for(NotecardSku)` (consteval lookup returning SkuInfo).
// Relies on Radios / Mcu / TxnPinSupport / SkuInfo defined in note/target.hpp.

#pragma once

#include <note/target.hpp>

namespace note {

// ---------------------------------------------------------------------------
// NotecardSku — specific Notecard product codes
//
// Enum values are C++-friendly renderings of the part numbers (e.g.
// NOTE-MBGLN → MbGlN). Revision differences within the same part number
// are disambiguated with a _v{major}_{minor} suffix (e.g. LwEu_v1_4).
// The human-readable part number is available via `part_number_of()`.
// ---------------------------------------------------------------------------

enum class NotecardSku : uint16_t {
    Unknown = 0,
    Esp,  // NOTE-ESP (2.1)
    Wifi,  // NOTE-WIFI (1.2)
    LwEu_v1_4,  // NOTE-LWEU (1.4)
    LwUs_v1_4,  // NOTE-LWUS (1.4)
    LwEu,  // NOTE-LWEU (2.1)
    LwUs,  // NOTE-LWUS (2.1)
    MbGlN,  // NOTE-MBGLN (3.2)
    MbNaN,  // NOTE-MBNAN (3.2)
    NbGlN,  // NOTE-NBGLN (3.2)
    NbNaN,  // NOTE-NBNAN (3.2)
    WbExN,  // NOTE-WBEXN (3.2)
    WbNaN,  // NOTE-WBNAN (3.2)
    MbGlW,  // NOTE-MBGLW (2.1)
    MbNaW,  // NOTE-MBNAW (2.1)
    NbGlW,  // NOTE-NBGLW (2.1)
    NbNaW,  // NOTE-NBNAW (2.1)
    WbExW,  // NOTE-WBEXW (2.1)
    WbGlW,  // NOTE-WBGLW (2.1)
    WbNaW,  // NOTE-WBNAW (2.1)
    WbGlWT,  // NOTE-WBGLWT (1.x) — Legacy Telit modem variant; 1.x hardware generation.
    NbGlWX,  // NOTE-NBGLWX (2.x) — Cell+WiFi+NTN satellite multi-mode.
    NbGl,  // NOTE-NBGL (1.5) — Legacy cellular.
    NbNa,  // NOTE-NBNA (1.5) — Legacy cellular.
    WbEx,  // NOTE-WBEX (1.5) — Legacy cellular.
    WbNa,  // NOTE-WBNA (1.5) — Legacy cellular.
};

// ---------------------------------------------------------------------------
// info_for — per-SKU capability lookup
// ---------------------------------------------------------------------------

constexpr SkuInfo info_for(NotecardSku s) {
    switch (s) {
    case NotecardSku::Esp: return {Radios::WiFi, Mcu::Esp32S3, TxnPinSupport::Dedicated};
    case NotecardSku::Wifi: return {Radios::WiFi, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::LwEu_v1_4: return {Radios::LoRa, Mcu::Stm32Wl, TxnPinSupport::Dedicated};
    case NotecardSku::LwUs_v1_4: return {Radios::LoRa, Mcu::Stm32Wl, TxnPinSupport::Dedicated};
    case NotecardSku::LwEu: return {Radios::LoRa, Mcu::Stm32Wle5, TxnPinSupport::Dedicated};
    case NotecardSku::LwUs: return {Radios::LoRa, Mcu::Stm32Wle5, TxnPinSupport::Dedicated};
    case NotecardSku::MbGlN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Muxed};
    case NotecardSku::MbNaN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NbGlN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Muxed};
    case NotecardSku::NbNaN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::WbExN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Muxed};
    case NotecardSku::WbNaN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Muxed};
    case NotecardSku::MbGlW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::MbNaW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NbGlW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NbNaW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::None};
    case NotecardSku::WbExW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::WbGlW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::WbNaW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::WbGlWT: return {Radios::CellWifi, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::NbGlWX: return {Radios::Skylo, Mcu::Stm32U5, TxnPinSupport::Dedicated};
    case NotecardSku::NbGl: return {Radios::Cell, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::NbNa: return {Radios::Cell, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::WbEx: return {Radios::Cell, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::WbNa: return {Radios::Cell, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::Unknown:
    default:
        return {Radios{}, Mcu::Unknown, TxnPinSupport::Unknown};
    }
}

// ---------------------------------------------------------------------------
// part_number_of — human-readable part number for a SKU value
// ---------------------------------------------------------------------------

constexpr const char* part_number_of(NotecardSku s) {
    switch (s) {
    case NotecardSku::Esp: return "NOTE-ESP";
    case NotecardSku::Wifi: return "NOTE-WIFI";
    case NotecardSku::LwEu_v1_4: return "NOTE-LWEU";
    case NotecardSku::LwUs_v1_4: return "NOTE-LWUS";
    case NotecardSku::LwEu: return "NOTE-LWEU";
    case NotecardSku::LwUs: return "NOTE-LWUS";
    case NotecardSku::MbGlN: return "NOTE-MBGLN";
    case NotecardSku::MbNaN: return "NOTE-MBNAN";
    case NotecardSku::NbGlN: return "NOTE-NBGLN";
    case NotecardSku::NbNaN: return "NOTE-NBNAN";
    case NotecardSku::WbExN: return "NOTE-WBEXN";
    case NotecardSku::WbNaN: return "NOTE-WBNAN";
    case NotecardSku::MbGlW: return "NOTE-MBGLW";
    case NotecardSku::MbNaW: return "NOTE-MBNAW";
    case NotecardSku::NbGlW: return "NOTE-NBGLW";
    case NotecardSku::NbNaW: return "NOTE-NBNAW";
    case NotecardSku::WbExW: return "NOTE-WBEXW";
    case NotecardSku::WbGlW: return "NOTE-WBGLW";
    case NotecardSku::WbNaW: return "NOTE-WBNAW";
    case NotecardSku::WbGlWT: return "NOTE-WBGLWT";
    case NotecardSku::NbGlWX: return "NOTE-NBGLWX";
    case NotecardSku::NbGl: return "NOTE-NBGL";
    case NotecardSku::NbNa: return "NOTE-NBNA";
    case NotecardSku::WbEx: return "NOTE-WBEX";
    case NotecardSku::WbNa: return "NOTE-WBNA";
    case NotecardSku::Unknown:
    default:
        return "unknown";
    }
}

// ---------------------------------------------------------------------------
// SkuType<S> — compile-time SKU wrapper
//
// Exposes the SkuInfo fields as static constexpr members so they're usable
// in `if constexpr` and template deduction contexts (e.g. CTAD on Notecard).
// ---------------------------------------------------------------------------

template<NotecardSku S>
struct SkuType {
    static constexpr NotecardSku  value  = S;
    static constexpr SkuInfo      info   = info_for(S);
    static constexpr Radios       radios = info.radios;
    static constexpr Mcu          mcu    = info.mcu;
    static constexpr TxnPinSupport txn   = info.txn;

    /// True iff the SKU is known to expose CTX/RTX pins (dedicated or muxed).
    /// Unknown SKUs default to false (conservative — caller can still register
    /// a TxnHandshake manually if they know better).
    static constexpr bool has_txn_pins =
        (info.txn == TxnPinSupport::Dedicated) ||
        (info.txn == TxnPinSupport::Muxed);
};

/// SKU factory variable: `sku<NotecardSku::Esp>` → `SkuType<NotecardSku::Esp>`.
template<NotecardSku S>
inline constexpr SkuType<S> sku{};

} // namespace note
