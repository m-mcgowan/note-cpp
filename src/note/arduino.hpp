#pragma once
/// @file arduino.hpp
/// Arduino convenience header — one include for typical Notecard usage.
///
///   #include <note/arduino.hpp>
///
///   note::arduino::Notecard nc;
///
///   void setup() {
///       nc.begin(Serial1, 9600);       // serial
///       // or: nc.begin(Wire);         // I2C (default address)
///       // or: nc.begin(Wire, 0x17);   // I2C with explicit address
///
///       nc.hub.set().product("com.example.app").execute();
///   }

#include <note/allocator.hpp>
#include <note/notecard_api.hpp>
#include <note/streaming_transport.hpp>
#include <note/arduino/serial.hpp>
#include <note/arduino/i2c.hpp>

#include <memory>

namespace note::arduino {

/// Arduino Notecard — typed API with begin() for Arduino hardware.
///
/// Inherits the full typed API from NotecardApi. Adds begin() overloads
/// that accept Arduino Serial or Wire objects directly. Owns the HAL
/// and transport internally.
#if __cplusplus >= 202002L
template<typename TargetT = Unconstrained>
class Notecard : public NotecardApi<TargetT> {
    using Base = NotecardApi<TargetT>;
#else
class Notecard : public NotecardApi {
    using Base = NotecardApi;
#endif
public:
    Notecard() = default;

    /// Begin with serial transport.
    ///   nc.begin(Serial1, 9600);
    template<typename SerialT>
    void begin(SerialT& uart, unsigned long baud = 9600) {
        serial_hal_ = std::make_unique<SerialHal<SerialT>>(uart, baud);
        serial_hal_transport_ = std::make_unique<transport::NotecardSerial<>>(*serial_hal_);
        serial_streaming_ = std::make_unique<StreamingTransport>(*serial_hal_transport_);
        Base::begin(*serial_streaming_);
    }

    /// Begin with serial transport and explicit allocator.
    ///   MonotonicArena arena(buf);
    ///   nc.begin(Serial1, 9600, arena_allocator(arena));
    template<typename SerialT>
    void begin(SerialT& uart, unsigned long baud, Allocator alloc) {
        serial_hal_ = std::make_unique<SerialHal<SerialT>>(uart, baud);
        serial_hal_transport_ = std::make_unique<transport::NotecardSerial<>>(*serial_hal_);
        serial_streaming_ = std::make_unique<StreamingTransport>(*serial_hal_transport_);
        Base::begin(*serial_streaming_, alloc);
    }

    /// Begin with I2C transport (default address).
    ///   nc.begin(Wire);
    void begin(TwoWire& wire) {
        begin_i2c(wire, transport::kI2cDefaultAddress);
    }

    /// Begin with I2C transport and explicit address.
    ///   nc.begin(Wire, 0x17);
    void begin(TwoWire& wire, uint8_t address) {
        begin_i2c(wire, address);
    }

    /// Begin with I2C transport and explicit allocator.
    void begin(TwoWire& wire, Allocator alloc) {
        begin_i2c(wire, transport::kI2cDefaultAddress, alloc);
    }

    /// Begin with I2C transport, address, and explicit allocator.
    void begin(TwoWire& wire, uint8_t address, Allocator alloc) {
        begin_i2c(wire, address, alloc);
    }

private:
    void begin_i2c(TwoWire& wire, uint8_t address, Allocator alloc = {}) {
        i2c_hal_ = std::make_unique<I2CHal>(wire, address);
        i2c_hal_transport_ = std::make_unique<transport::NotecardI2c<>>(*i2c_hal_);
        i2c_streaming_ = std::make_unique<StreamingTransport>(*i2c_hal_transport_);
        Base::begin(*i2c_streaming_, alloc);
    }

    std::unique_ptr<transport::SerialHal> serial_hal_;
    std::unique_ptr<transport::NotecardSerial<>> serial_hal_transport_;
    std::unique_ptr<StreamingTransport> serial_streaming_;
    std::unique_ptr<I2CHal> i2c_hal_;
    std::unique_ptr<transport::NotecardI2c<>> i2c_hal_transport_;
    std::unique_ptr<StreamingTransport> i2c_streaming_;
};

} // namespace note::arduino
