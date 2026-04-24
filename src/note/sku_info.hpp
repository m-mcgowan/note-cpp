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
// Enum values match the Blues part numbers with '-' replaced by '_'
// (C++ identifier rules). Example: NOTE-MBGLN → NOTE_MBGLN. Revision
// differences within the same part number are disambiguated with a
// _V{major}_{minor} suffix (e.g. NOTE_LWEU_V1_4). The human-readable
// part number is available via `part_number_of()`.
// ---------------------------------------------------------------------------

enum class NotecardSku : uint16_t {
    Unknown = 0,
    NOTE_ESP,  // NOTE-ESP (2.1)
    NOTE_WIFI,  // NOTE-WIFI (1.2)
    NOTE_LWEU_V1_4,  // NOTE-LWEU (1.4)
    NOTE_LWUS_V1_4,  // NOTE-LWUS (1.4)
    NOTE_LWEU,  // NOTE-LWEU (2.1)
    NOTE_LWUS,  // NOTE-LWUS (2.1)
    NOTE_MBGLN,  // NOTE-MBGLN (3.2)
    NOTE_MBNAN,  // NOTE-MBNAN (3.2)
    NOTE_NBGLN,  // NOTE-NBGLN (3.2)
    NOTE_NBNAN,  // NOTE-NBNAN (3.2)
    NOTE_WBEXN,  // NOTE-WBEXN (3.2)
    NOTE_WBNAN,  // NOTE-WBNAN (3.2)
    NOTE_MBGLW,  // NOTE-MBGLW (2.1)
    NOTE_MBNAW,  // NOTE-MBNAW (2.1)
    NOTE_NBGLW,  // NOTE-NBGLW (2.1)
    NOTE_NBNAW,  // NOTE-NBNAW (2.1)
    NOTE_WBEXW,  // NOTE-WBEXW (2.1)
    NOTE_WBGLW,  // NOTE-WBGLW (2.1)
    NOTE_WBNAW,  // NOTE-WBNAW (2.1)
    NOTE_WBGLWT,  // NOTE-WBGLWT (1.x) — Legacy Telit modem variant; 1.x hardware generation.
    NOTE_NBGLWX,  // NOTE-NBGLWX (2.x) — Cell+WiFi+NTN satellite multi-mode.
    NOTE_NBGL,  // NOTE-NBGL (1.5) — Legacy cellular.
    NOTE_NBNA,  // NOTE-NBNA (1.5) — Legacy cellular.
    NOTE_WBEX,  // NOTE-WBEX (1.5) — Legacy cellular.
    NOTE_WBNA,  // NOTE-WBNA (1.5) — Legacy cellular.
};

// ---------------------------------------------------------------------------
// info_for — per-SKU capability lookup
// ---------------------------------------------------------------------------

constexpr SkuInfo info_for(NotecardSku s) {
    switch (s) {
    case NotecardSku::NOTE_ESP: return {Radios::WiFi, Mcu::Esp32S3, TxnPinSupport::Dedicated};
    case NotecardSku::NOTE_WIFI: return {Radios::WiFi, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::NOTE_LWEU_V1_4: return {Radios::LoRa, Mcu::Stm32Wl, TxnPinSupport::Dedicated};
    case NotecardSku::NOTE_LWUS_V1_4: return {Radios::LoRa, Mcu::Stm32Wl, TxnPinSupport::Dedicated};
    case NotecardSku::NOTE_LWEU: return {Radios::LoRa, Mcu::Stm32Wle5, TxnPinSupport::Dedicated};
    case NotecardSku::NOTE_LWUS: return {Radios::LoRa, Mcu::Stm32Wle5, TxnPinSupport::Dedicated};
    case NotecardSku::NOTE_MBGLN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Muxed};
    case NotecardSku::NOTE_MBNAN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NOTE_NBGLN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Muxed};
    case NotecardSku::NOTE_NBNAN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NOTE_WBEXN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Muxed};
    case NotecardSku::NOTE_WBNAN: return {Radios::Cell, Mcu::Stm32U5, TxnPinSupport::Muxed};
    case NotecardSku::NOTE_MBGLW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NOTE_MBNAW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NOTE_NBGLW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NOTE_NBNAW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::None};
    case NotecardSku::NOTE_WBEXW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NOTE_WBGLW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NOTE_WBNAW: return {Radios::CellWifi, Mcu::Stm32U5, TxnPinSupport::Unknown};
    case NotecardSku::NOTE_WBGLWT: return {Radios::CellWifi, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::NOTE_NBGLWX: return {Radios::Skylo, Mcu::Stm32U5, TxnPinSupport::Dedicated};
    case NotecardSku::NOTE_NBGL: return {Radios::Cell, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::NOTE_NBNA: return {Radios::Cell, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::NOTE_WBEX: return {Radios::Cell, Mcu::Stm32L4, TxnPinSupport::None};
    case NotecardSku::NOTE_WBNA: return {Radios::Cell, Mcu::Stm32L4, TxnPinSupport::None};
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
    case NotecardSku::NOTE_ESP: return "NOTE-ESP";
    case NotecardSku::NOTE_WIFI: return "NOTE-WIFI";
    case NotecardSku::NOTE_LWEU_V1_4: return "NOTE-LWEU";
    case NotecardSku::NOTE_LWUS_V1_4: return "NOTE-LWUS";
    case NotecardSku::NOTE_LWEU: return "NOTE-LWEU";
    case NotecardSku::NOTE_LWUS: return "NOTE-LWUS";
    case NotecardSku::NOTE_MBGLN: return "NOTE-MBGLN";
    case NotecardSku::NOTE_MBNAN: return "NOTE-MBNAN";
    case NotecardSku::NOTE_NBGLN: return "NOTE-NBGLN";
    case NotecardSku::NOTE_NBNAN: return "NOTE-NBNAN";
    case NotecardSku::NOTE_WBEXN: return "NOTE-WBEXN";
    case NotecardSku::NOTE_WBNAN: return "NOTE-WBNAN";
    case NotecardSku::NOTE_MBGLW: return "NOTE-MBGLW";
    case NotecardSku::NOTE_MBNAW: return "NOTE-MBNAW";
    case NotecardSku::NOTE_NBGLW: return "NOTE-NBGLW";
    case NotecardSku::NOTE_NBNAW: return "NOTE-NBNAW";
    case NotecardSku::NOTE_WBEXW: return "NOTE-WBEXW";
    case NotecardSku::NOTE_WBGLW: return "NOTE-WBGLW";
    case NotecardSku::NOTE_WBNAW: return "NOTE-WBNAW";
    case NotecardSku::NOTE_WBGLWT: return "NOTE-WBGLWT";
    case NotecardSku::NOTE_NBGLWX: return "NOTE-NBGLWX";
    case NotecardSku::NOTE_NBGL: return "NOTE-NBGL";
    case NotecardSku::NOTE_NBNA: return "NOTE-NBNA";
    case NotecardSku::NOTE_WBEX: return "NOTE-WBEX";
    case NotecardSku::NOTE_WBNA: return "NOTE-WBNA";
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

// ---------------------------------------------------------------------------
// note::sku — factory namespace (one constant per SKU, Blues part-number named)
//
// Use at construction sites:
//   note::arduino::Notecard nc(note::sku::NOTE_ESP);
// ---------------------------------------------------------------------------

namespace sku {
inline constexpr auto NOTE_ESP = SkuType<NotecardSku::NOTE_ESP>{};
inline constexpr auto NOTE_WIFI = SkuType<NotecardSku::NOTE_WIFI>{};
inline constexpr auto NOTE_LWEU_V1_4 = SkuType<NotecardSku::NOTE_LWEU_V1_4>{};
inline constexpr auto NOTE_LWUS_V1_4 = SkuType<NotecardSku::NOTE_LWUS_V1_4>{};
inline constexpr auto NOTE_LWEU = SkuType<NotecardSku::NOTE_LWEU>{};
inline constexpr auto NOTE_LWUS = SkuType<NotecardSku::NOTE_LWUS>{};
inline constexpr auto NOTE_MBGLN = SkuType<NotecardSku::NOTE_MBGLN>{};
inline constexpr auto NOTE_MBNAN = SkuType<NotecardSku::NOTE_MBNAN>{};
inline constexpr auto NOTE_NBGLN = SkuType<NotecardSku::NOTE_NBGLN>{};
inline constexpr auto NOTE_NBNAN = SkuType<NotecardSku::NOTE_NBNAN>{};
inline constexpr auto NOTE_WBEXN = SkuType<NotecardSku::NOTE_WBEXN>{};
inline constexpr auto NOTE_WBNAN = SkuType<NotecardSku::NOTE_WBNAN>{};
inline constexpr auto NOTE_MBGLW = SkuType<NotecardSku::NOTE_MBGLW>{};
inline constexpr auto NOTE_MBNAW = SkuType<NotecardSku::NOTE_MBNAW>{};
inline constexpr auto NOTE_NBGLW = SkuType<NotecardSku::NOTE_NBGLW>{};
inline constexpr auto NOTE_NBNAW = SkuType<NotecardSku::NOTE_NBNAW>{};
inline constexpr auto NOTE_WBEXW = SkuType<NotecardSku::NOTE_WBEXW>{};
inline constexpr auto NOTE_WBGLW = SkuType<NotecardSku::NOTE_WBGLW>{};
inline constexpr auto NOTE_WBNAW = SkuType<NotecardSku::NOTE_WBNAW>{};
inline constexpr auto NOTE_WBGLWT = SkuType<NotecardSku::NOTE_WBGLWT>{};
inline constexpr auto NOTE_NBGLWX = SkuType<NotecardSku::NOTE_NBGLWX>{};
inline constexpr auto NOTE_NBGL = SkuType<NotecardSku::NOTE_NBGL>{};
inline constexpr auto NOTE_NBNA = SkuType<NotecardSku::NOTE_NBNA>{};
inline constexpr auto NOTE_WBEX = SkuType<NotecardSku::NOTE_WBEX>{};
inline constexpr auto NOTE_WBNA = SkuType<NotecardSku::NOTE_WBNA>{};
} // namespace sku

} // namespace note
