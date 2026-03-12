#pragma once
/// @file hal_serial.hpp
/// ESP32 SerialHal implementation using Arduino HardwareSerial.

#include <note/transport/serial.hpp>
#include <HardwareSerial.h>

#ifndef NOTECARD_SERIAL_RX
#error "NOTECARD_SERIAL_RX must be defined (source env.sh before building)"
#endif
#ifndef NOTECARD_SERIAL_TX
#error "NOTECARD_SERIAL_TX must be defined (source env.sh before building)"
#endif

class Esp32SerialHal : public note::transport::SerialHal {
    HardwareSerial& uart_;
public:
    explicit Esp32SerialHal(HardwareSerial& uart,
                            int rx = NOTECARD_SERIAL_RX,
                            int tx = NOTECARD_SERIAL_TX,
                            unsigned long baud = 9600)
        : uart_(uart)
    {
        uart_.begin(baud, SERIAL_8N1, rx, tx);
    }

    bool transmit(const uint8_t* data, size_t len) override {
        size_t written = uart_.write(data, len);
        uart_.flush();
        return written == len;
    }

    size_t receive(uint8_t* buf, size_t max_len) override {
        size_t count = 0;
        while (count < max_len && uart_.available()) {
            buf[count++] = static_cast<uint8_t>(uart_.read());
        }
        return count;
    }

    uint32_t millis() override { return ::millis(); }
    void delay(uint32_t ms) override { ::delay(ms); }
};
