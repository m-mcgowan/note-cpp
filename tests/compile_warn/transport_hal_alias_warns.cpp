// `note::TransportHal` is a [[deprecated]] alias for `note::Hal`. Direct
// use of the old name must trigger -Werror=deprecated-declarations so that
// downstream code is steered to the new name during the v0.3 → v0.4 window.
#include <note/transport_hal.hpp>

struct UseDeprecated : note::TransportHal {
    bool transmit(const uint8_t*, size_t) override { return true; }
    note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return note::Result<size_t>{0}; }
    bool reset() override { return true; }
    bool write_line_terminator() override { return true; }
    uint32_t millis() override { return 0; }
    void delay(uint32_t) override {}
};
