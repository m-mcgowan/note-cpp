#pragma once
/// @file posix.hpp
/// POSIX convenience header — one include for typical Notecard usage on
/// Linux / macOS / BSD hosts.
///
///   #include <note/posix.hpp>
///
///   int main() {
///       note::posix::Notecard nc;
///       nc.begin("/dev/ttyUSB0", 9600);          // serial (any POSIX)
///       // or: nc.begin(note::posix::serial{"/dev/ttyUSB0"});
///       // or: nc.begin_i2c("/dev/i2c-1");        // Linux-only
///
///       nc.hub.set().product("com.example.app").execute();
///   }

#include <note/allocator.hpp>
#include <note/notecard_api.hpp>
#include <note/streaming_transport.hpp>
#include <note/units.hpp>

#include <note/posix/serial.hpp>
#ifdef __linux__
#include <note/posix/i2c.hpp>
#endif

#include <cstdint>
#include <memory>

namespace note::posix {

// ---------------------------------------------------------------------------
// Tag types — for begin(note::posix::serial{...}) / begin(i2c{...}) callers
// that prefer explicit dispatch over overload resolution.
// ---------------------------------------------------------------------------

struct serial {
    const char* port;
    int         baud;
    constexpr serial(const char* p, int b = 9600) : port(p), baud(b) {}
};

#ifdef __linux__
struct i2c {
    const char* device;
    uint8_t     address;
    constexpr i2c(const char* d,
                  uint8_t a = transport::kI2cDefaultAddress)
        : device(d), address(a) {}
};
#endif

// ---------------------------------------------------------------------------
// Notecard — typed API + begin() overloads for POSIX hosts.
// ---------------------------------------------------------------------------
//
// Three equivalent ways to open a serial link:
//   nc.begin("/dev/ttyUSB0", 9600);             // default overload
//   nc.begin_serial("/dev/ttyUSB0", 9600);      // explicit method
//   nc.begin(note::posix::serial{"/dev/ttyUSB0", 9600});  // tag type
//
// And for I2C on Linux (the kernel i2c-dev interface is Linux-specific):
//   nc.begin_i2c("/dev/i2c-1");                  // default addr 0x17
//   nc.begin_i2c("/dev/i2c-1", 0x17);
//   nc.begin(note::posix::i2c{"/dev/i2c-1", 0x17});

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
    ///   note::posix::Notecard nc(note::sku::NOTE_ESP, note::fw::v7_5_1);
#if __cplusplus >= 202002L
    template<typename... Axes>
        requires (sizeof...(Axes) > 0
                  && (note::detail::HasAxisCategory<Axes> && ...))
    Notecard(Axes...) {}
#endif

    // ── Method A: explicit method names ────────────────────────────────────

    /// Open a serial link to the Notecard.
    void begin_serial(const char* port, int baud = 9600) {
        serial_hal_ = std::make_unique<PosixSerialHal>(port, baud);
        serial_transport_ = std::make_unique<transport::NotecardSerial<>>(*serial_hal_);
        serial_streaming_ = std::make_unique<StreamingTransport>(*serial_transport_);
        Base::begin(*serial_streaming_);
    }

    void begin_serial(const char* port, int baud, Allocator alloc) {
        serial_hal_ = std::make_unique<PosixSerialHal>(port, baud);
        serial_transport_ = std::make_unique<transport::NotecardSerial<>>(*serial_hal_);
        serial_streaming_ = std::make_unique<StreamingTransport>(*serial_transport_);
        Base::begin(*serial_streaming_, alloc);
    }

#ifdef __linux__
    /// Open an I2C link to the Notecard (Linux /dev/i2c-N).
    void begin_i2c(const char* device,
                   uint8_t address = transport::kI2cDefaultAddress) {
        i2c_hal_ = std::make_unique<LinuxI2cHal>(device, address);
        i2c_transport_ = std::make_unique<transport::NotecardI2c<>>(*i2c_hal_);
        i2c_streaming_ = std::make_unique<StreamingTransport>(*i2c_transport_);
        Base::begin(*i2c_streaming_);
    }

    void begin_i2c(const char* device, uint8_t address, Allocator alloc) {
        i2c_hal_ = std::make_unique<LinuxI2cHal>(device, address);
        i2c_transport_ = std::make_unique<transport::NotecardI2c<>>(*i2c_hal_);
        i2c_streaming_ = std::make_unique<StreamingTransport>(*i2c_transport_);
        Base::begin(*i2c_streaming_, alloc);
    }
#endif

    // ── Method B: begin() defaults to serial ───────────────────────────────

    /// Open a serial link — shorthand for begin_serial().
    void begin(const char* port, int baud = 9600) {
        begin_serial(port, baud);
    }

    void begin(const char* port, int baud, Allocator alloc) {
        begin_serial(port, baud, alloc);
    }

    // ── Method C: tag-type dispatch ────────────────────────────────────────

    void begin(serial cfg)                   { begin_serial(cfg.port, cfg.baud); }
    void begin(serial cfg, Allocator alloc)  { begin_serial(cfg.port, cfg.baud, alloc); }

#ifdef __linux__
    void begin(i2c cfg)                      { begin_i2c(cfg.device, cfg.address); }
    void begin(i2c cfg, Allocator alloc)     { begin_i2c(cfg.device, cfg.address, alloc); }
#endif

private:
    std::unique_ptr<PosixSerialHal>              serial_hal_;
    std::unique_ptr<transport::NotecardSerial<>> serial_transport_;
    std::unique_ptr<StreamingTransport>          serial_streaming_;
#ifdef __linux__
    std::unique_ptr<LinuxI2cHal>                 i2c_hal_;
    std::unique_ptr<transport::NotecardI2c<>>    i2c_transport_;
    std::unique_ptr<StreamingTransport>          i2c_streaming_;
#endif
};

#if __cplusplus >= 202002L
/// CTAD: map axis values at the call site to `Notecard<ComposedTarget<Axes...>>`.
template<typename... Axes>
    requires (sizeof...(Axes) > 0
              && (note::detail::HasAxisCategory<Axes> && ...))
Notecard(Axes...) -> Notecard<note::ComposedTarget<Axes...>>;
#endif

}  // namespace note::posix
