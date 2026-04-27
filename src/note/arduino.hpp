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
#include <note/units.hpp>
#include <note/streaming_transport.hpp>
#include <note/arduino/debug.hpp>
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

    /// Variadic axis pack — constrains TargetT via CTAD (see deduction guide
    /// below). Axes are compile-time tags only; runtime state matches the
    /// default constructor.
    ///
    ///   note::arduino::Notecard nc(note::sku::NOTE_ESP, note::fw::v7_5_1);
#if __cplusplus >= 202002L
    template<typename... Axes>
        requires (sizeof...(Axes) > 0
                  && (note::detail::HasAxisCategory<Axes> && ...))
    Notecard(Axes...) {}
#endif

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

    /// Begin with I2C transport (default address). HAL calls Wire.begin()
    /// with no pin args. Use this on devkits where Wire's default pins
    /// are already correct.
    ///   nc.begin(Wire);
    void begin(TwoWire& wire) {
        begin_i2c(wire, transport::kI2cDefaultAddress);
    }

    /// Begin with I2C transport and explicit address.
    ///   nc.begin(Wire, 0x17);
    void begin(TwoWire& wire, uint8_t address) {
        begin_i2c(wire, address);
    }

    /// Begin with I2C transport on custom pins. HAL calls Wire.begin(sda,
    /// scl); on reset it cycles Wire.end() / Wire.begin(sda, scl).
    /// Use this for non-devkit boards where the Notecard sits on
    /// non-default I2C pins.
    ///   nc.begin(Wire, /*sda=*/14, /*scl=*/21);
    void begin(TwoWire& wire, int sda, int scl) {
        begin_i2c_pins(wire, sda, scl, transport::kI2cDefaultAddress);
    }

    /// Begin with I2C transport on custom pins and explicit address.
    void begin(TwoWire& wire, int sda, int scl, uint8_t address) {
        begin_i2c_pins(wire, sda, scl, address);
    }

    /// Begin on a Wire bus the app already initialised. The HAL never
    /// calls Wire.begin() / Wire.end(). Use this on shared buses where
    /// other drivers / tasks would be disrupted by an internal reset.
    ///   Wire.begin(14, 21);
    ///   nc.begin(Wire, note::arduino::external_bus);
    void begin(TwoWire& wire, ExternalBus tag) {
        begin_i2c_external(wire, tag, transport::kI2cDefaultAddress);
    }

    /// External-bus begin with explicit address.
    void begin(TwoWire& wire, ExternalBus tag, uint8_t address) {
        begin_i2c_external(wire, tag, address);
    }

    /// Begin with I2C transport and explicit allocator.
    void begin(TwoWire& wire, Allocator alloc) {
        begin_i2c(wire, transport::kI2cDefaultAddress, alloc);
    }

    /// Begin with I2C transport, address, and explicit allocator.
    void begin(TwoWire& wire, uint8_t address, Allocator alloc) {
        begin_i2c(wire, address, alloc);
    }

    /// Begin with I2C transport on custom pins and explicit allocator.
    void begin(TwoWire& wire, int sda, int scl, Allocator alloc) {
        begin_i2c_pins(wire, sda, scl, transport::kI2cDefaultAddress, alloc);
    }

    /// Begin with I2C transport on custom pins, explicit address and allocator.
    void begin(TwoWire& wire, int sda, int scl, uint8_t address, Allocator alloc) {
        begin_i2c_pins(wire, sda, scl, address, alloc);
    }

    /// External-bus begin with explicit allocator.
    void begin(TwoWire& wire, ExternalBus tag, Allocator alloc) {
        begin_i2c_external(wire, tag, transport::kI2cDefaultAddress, alloc);
    }

    /// External-bus begin with explicit address and allocator.
    void begin(TwoWire& wire, ExternalBus tag, uint8_t address, Allocator alloc) {
        begin_i2c_external(wire, tag, address, alloc);
    }

    /// Enable debug output to an Arduino Print (e.g. Serial).
    /// Default: wire data only. Pass flags for more categories:
    ///   nc.setDebugOutput(Serial, note::DebugWire | note::DebugTiming);
    void setDebugOutput(Print& out, uint8_t flags = DebugWire) {
        Base::notecard().set_debug(arduino::debug(out, flags));
    }

    /// Disable debug output.
    void clearDebugOutput() {
        Base::notecard().clear_debug();
    }

private:
    void begin_i2c(TwoWire& wire, uint8_t address, Allocator alloc = {}) {
        i2c_hal_ = std::make_unique<I2CHal>(wire, address);
        begin_i2c_finish(alloc);
    }

    void begin_i2c_pins(TwoWire& wire, int sda, int scl,
                        uint8_t address, Allocator alloc = {}) {
        i2c_hal_ = std::make_unique<I2CHal>(wire, sda, scl, address);
        begin_i2c_finish(alloc);
    }

    void begin_i2c_external(TwoWire& wire, ExternalBus tag,
                            uint8_t address, Allocator alloc = {}) {
        i2c_hal_ = std::make_unique<I2CHal>(wire, tag, address);
        begin_i2c_finish(alloc);
    }

    void begin_i2c_finish(Allocator alloc) {
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

#if __cplusplus >= 202002L
/// CTAD: map axis values at the call site to `Notecard<ComposedTarget<Axes...>>`.
template<typename... Axes>
    requires (sizeof...(Axes) > 0
              && (note::detail::HasAxisCategory<Axes> && ...))
Notecard(Axes...) -> Notecard<note::ComposedTarget<Axes...>>;
#endif

} // namespace note::arduino
