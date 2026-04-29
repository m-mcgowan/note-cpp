#pragma once

/// @file begin.hpp
/// Arduino transport stacks for use with NotecardApi::begin().
///
/// Usage (serial):
///   #include <note/arduino/begin.hpp>
///
///   note::arduino::SerialTransportStack serial(Serial1, 9600);
///   note::NotecardApi nc;
///   nc.begin(serial.transport, note::arena_allocator(arena));

#include <note/arduino/serial.hpp>
#include <note/protocol.hpp>
#include <note/transport/serial.hpp>

namespace note::arduino {

/// Owns the full serial transport stack: SerialHal → NotecardSerial → Protocol.
template<typename SerialT = HardwareSerial>
struct SerialTransportStack {
    using Hal = SerialHal<SerialT>;
#if NOTE_STATIC_HAL
    using NcSerial = transport::NotecardSerial<Hal>;
    using Transport = Protocol<NcSerial>;
#else
    using NcSerial = transport::NotecardSerial<>;
    using Transport = Protocol;
#endif

    Hal hal;
    NcSerial notecard_hal;
    Transport transport;

    SerialTransportStack(SerialT& uart, unsigned long baud)
        : hal(uart, baud)
        , notecard_hal(hal)
        , transport(notecard_hal) {}
};

} // namespace note::arduino

// I2C stack is in a separate header to avoid pulling in Wire.h:
//   #include <note/arduino/begin_i2c.hpp>
