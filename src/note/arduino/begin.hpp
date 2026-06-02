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
#include <note/hal_byte_transport.hpp>
#include <note/json_request_transport.hpp>
#include <note/link/serial.hpp>

namespace note::arduino {

/// Owns the full serial transport stack: SerialHal → SerialFramer →
/// HalByteTransport → JsonRequestTransport. Static/templated builds collapse the
/// two transport layers through templates; polymorphic builds keep the
/// virtual interfaces.
template<typename SerialT = HardwareSerial>
struct SerialTransportStack {
    using Hal = SerialHal<SerialT>;
#if NOTE_STATIC_HAL
    using NcSerial = link::SerialFramer<Hal>;
    using ByteTransport = note::HalByteTransportT<NcSerial>;
    using Transport = note::JsonRequestTransportT<ByteTransport>;
#else
    using NcSerial = link::SerialFramer<>;
    using ByteTransport = note::HalByteTransport;
    using Transport = note::JsonRequestTransport;
#endif

    Hal hal;
    NcSerial notecard_hal;
    ByteTransport byte_tx;
    Transport transport;

    SerialTransportStack(SerialT& uart, unsigned long baud)
        : hal(uart, baud)
        , notecard_hal(hal)
        , byte_tx(notecard_hal)
        , transport(byte_tx) {}
};

} // namespace note::arduino

// I2C stack is in a separate header to avoid pulling in Wire.h:
//   #include <note/arduino/begin_i2c.hpp>
