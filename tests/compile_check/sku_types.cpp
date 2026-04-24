// Compile check: verify the SKU / Radios / Mcu compile-time type wrappers
// expose the expected traits. Drives the metadata → codegen → type-system
// pipeline.

#include <note/sku_info.hpp>
#include <note/target.hpp>
#include <type_traits>

using namespace note;

// ── Radios wrapper ─────────────────────────────────────────────────────

static_assert(RadiosType<Radios::Cell>::value == Radios::Cell);

// ── Mcu wrapper: coarse has_txn_pins filter ────────────────────────────
// STM32L4 is the only MCU known to never have CTX/RTX pins; everything
// else (including Unknown's counterpart STM32U5) defaults to "possibly".

static_assert(McuType<Mcu::Stm32L4>::has_txn_pins  == false);
static_assert(McuType<Mcu::Unknown>::has_txn_pins  == false);
static_assert(McuType<Mcu::Stm32U5>::has_txn_pins  == true);
static_assert(McuType<Mcu::Esp32S3>::has_txn_pins  == true);
static_assert(McuType<Mcu::Stm32Wl>::has_txn_pins  == true);
static_assert(McuType<Mcu::Stm32Wle5>::has_txn_pins == true);

// ── Sku wrapper: per-SKU authoritative data ────────────────────────────

// NOTE-ESP: WiFi radios, ESP32-S3 MCU, dedicated CTX/RTX.
static_assert(SkuType<NotecardSku::NOTE_ESP>::radios == Radios::WiFi);
static_assert(SkuType<NotecardSku::NOTE_ESP>::mcu    == Mcu::Esp32S3);
static_assert(SkuType<NotecardSku::NOTE_ESP>::txn    == TxnPinSupport::Dedicated);
static_assert(SkuType<NotecardSku::NOTE_ESP>::has_txn_pins == true);

// NOTE-WIFI: WiFi radios, STM32L4, no CTX/RTX pins.
static_assert(SkuType<NotecardSku::NOTE_WIFI>::radios == Radios::WiFi);
static_assert(SkuType<NotecardSku::NOTE_WIFI>::mcu    == Mcu::Stm32L4);
static_assert(SkuType<NotecardSku::NOTE_WIFI>::txn    == TxnPinSupport::None);
static_assert(SkuType<NotecardSku::NOTE_WIFI>::has_txn_pins == false);

// NOTE-NBGLN: cellular, STM32U5, muxed CTX/RTX via AUX2/AUX3.
static_assert(SkuType<NotecardSku::NOTE_NBGLN>::radios == Radios::Cell);
static_assert(SkuType<NotecardSku::NOTE_NBGLN>::txn    == TxnPinSupport::Muxed);
static_assert(SkuType<NotecardSku::NOTE_NBGLN>::has_txn_pins == true);

// NOTE-NBGLWX: satellite multi-mode, dedicated pins.
static_assert(SkuType<NotecardSku::NOTE_NBGLWX>::radios == Radios::Skylo);
static_assert(SkuType<NotecardSku::NOTE_NBGLWX>::has_txn_pins == true);

// Legacy NOTE-NBGL: STM32L4, no pins.
static_assert(SkuType<NotecardSku::NOTE_NBGL>::mcu == Mcu::Stm32L4);
static_assert(SkuType<NotecardSku::NOTE_NBGL>::has_txn_pins == false);

// ── part_number_of round-trip ──────────────────────────────────────────

constexpr auto esp_part = part_number_of(NotecardSku::NOTE_ESP);
static_assert(esp_part[0] == 'N' && esp_part[1] == 'O' && esp_part[2] == 'T' && esp_part[3] == 'E');

// ── Axis factory namespaces ────────────────────────────────────────────
// Each axis constant is a SkuType / RadiosType / McuType instance.

static_assert(std::is_same_v<decltype(sku::NOTE_ESP),
                             const SkuType<NotecardSku::NOTE_ESP>>);
static_assert(sku::NOTE_ESP.radios == Radios::WiFi);
static_assert(sku::NOTE_NBGLWX.radios == Radios::Skylo);

static_assert(std::is_same_v<decltype(radios::WIFI),
                             const RadiosType<Radios::WiFi>>);
static_assert(radios::WIFI.value == Radios::WiFi);
static_assert(radios::CELL_WIFI.value == Radios::CellWifi);

static_assert(std::is_same_v<decltype(mcu::STM32L4),
                             const McuType<Mcu::Stm32L4>>);
static_assert(mcu::STM32L4.has_txn_pins == false);
static_assert(mcu::ESP32S3.has_txn_pins == true);

int main() { return 0; }
