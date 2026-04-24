// ComposedTarget: cross-axis MCU conflict — NOTE_ESP is an ESP32-S3 SKU,
// but the user asserted mcu::NOTE_STM32L4. The _mcu_consistent() static
// assertion must fire.
#include <note/target.hpp>
#include <note/sku_info.hpp>

void test() {
    (void) note::ComposedTarget<
        note::SkuType<note::NotecardSku::NOTE_ESP>,
        note::McuType<note::Mcu::Stm32L4>>{};
}
