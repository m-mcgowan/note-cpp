// Compile check: verify the SKU / Radio / Mcu compile-time type wrappers
// and their factory variables (radio<>, mcu<>, sku<>) expose the expected
// traits. Drives the metadata → codegen → type-system pipeline.

#include <note/sku_info.hpp>
#include <note/target.hpp>
#include <type_traits>

using namespace note;

// ── Radio wrapper ──────────────────────────────────────────────────────

static_assert(RadioType<Radio::Cell>::value == Radio::Cell);
static_assert(std::is_same_v<decltype(radio<Radio::WiFi>), const RadioType<Radio::WiFi>>);

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

// NOTE-ESP: WiFi radio, ESP32-S3 MCU, dedicated CTX/RTX.
static_assert(sku<NotecardSku::Esp>.radio == Radio::WiFi);
static_assert(sku<NotecardSku::Esp>.mcu   == Mcu::Esp32S3);
static_assert(sku<NotecardSku::Esp>.txn   == TxnPinSupport::Dedicated);
static_assert(sku<NotecardSku::Esp>.has_txn_pins == true);

// NOTE-WIFI: WiFi radio, STM32L4, no CTX/RTX pins.
static_assert(sku<NotecardSku::Wifi>.radio == Radio::WiFi);
static_assert(sku<NotecardSku::Wifi>.mcu   == Mcu::Stm32L4);
static_assert(sku<NotecardSku::Wifi>.txn   == TxnPinSupport::None);
static_assert(sku<NotecardSku::Wifi>.has_txn_pins == false);

// NOTE-NBGLN: cellular, STM32U5, muxed CTX/RTX via AUX2/AUX3.
static_assert(sku<NotecardSku::NbGlN>.radio == Radio::Cell);
static_assert(sku<NotecardSku::NbGlN>.txn   == TxnPinSupport::Muxed);
static_assert(sku<NotecardSku::NbGlN>.has_txn_pins == true);

// NOTE-NBGLWX: satellite multi-mode, dedicated pins.
static_assert(sku<NotecardSku::NbGlWX>.radio == Radio::Skylo);
static_assert(sku<NotecardSku::NbGlWX>.has_txn_pins == true);

// Legacy NOTE-NBGL: STM32L4, no pins.
static_assert(sku<NotecardSku::NbGl>.mcu == Mcu::Stm32L4);
static_assert(sku<NotecardSku::NbGl>.has_txn_pins == false);

// ── part_number_of round-trip ──────────────────────────────────────────

constexpr auto esp_part = part_number_of(NotecardSku::Esp);
static_assert(esp_part[0] == 'N' && esp_part[1] == 'O' && esp_part[2] == 'T' && esp_part[3] == 'E');

int main() { return 0; }
