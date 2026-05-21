#pragma once

/// @file begin_spike_layered.hpp
/// SPIKE: drop-in equivalent of `SerialTransportStack` that uses the
/// layered `HalByteTransportT` + `JsonTransactT` stack instead of the
/// monolithic `Protocol`. Used only for AVR flash measurement during
/// the spike validation — production version will replace
/// SerialTransportStack outright.

#include <note/arduino/serial.hpp>
#include <note/link/serial.hpp>
#include <note/spike/templated.hpp>

namespace note::arduino {

template<typename SerialT = HardwareSerial>
struct SpikeLayeredSerialTransportStack {
    using Hal = SerialHal<SerialT>;
#if NOTE_STATIC_HAL
    using NcSerial = link::SerialFramer<Hal>;
#else
    using NcSerial = link::SerialFramer<>;
#endif
    using ByteTransport = note::spike::HalByteTransportT<NcSerial>;
    using Transport = note::spike::JsonTransactT<ByteTransport>;

    Hal hal;
    NcSerial notecard_hal;
    ByteTransport byte_tx;
    Transport transport;

    SpikeLayeredSerialTransportStack(SerialT& uart, unsigned long baud)
        : hal(uart, baud)
        , notecard_hal(hal)
        , byte_tx(notecard_hal)
        , transport(byte_tx) {}
};

} // namespace note::arduino
