// Explicit RadiosType disagrees with the SKU's implied radios family.
// NOTE-ESP implies WiFi, but radios::NOTE_CELL explicitly asks for Cell.
// ComposedTarget's _radios_consistent static_assert must fire.
#include <note/sku_info.hpp>
#include <note/target.hpp>
using Bad = note::ComposedTarget<
    decltype(note::sku::NOTE_ESP),
    decltype(note::radios::NOTE_CELL)>;
void test() { (void)Bad::radios; }
