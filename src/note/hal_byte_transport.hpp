#pragma once

/// @file hal_byte_transport.hpp
/// Concrete `IByteTransport` over a `note::Hal`. Two flavors:
///
///   HalByteTransport      — polymorphic, inherits IByteTransport. Used
///                           by the polymorphic Notecard / BareNotecard
///                           paths that take an IByteTransport& or
///                           ITransact& by reference.
///
///   HalByteTransportT<HalT> — templated, no vtable. Used by the static /
///                             AVR path (StaticNotecard) where the HAL
///                             type is known statically and the compiler
///                             can inline through the stack.
///
/// Both implement identical byte-level semantics: init handshake, frame
/// terminator detection at `\n`, lookahead-buffered HAL reads so bytes
/// after the terminator (e.g. binary COBS data arriving in the same
/// chunk) aren't dropped. No wire-format knowledge (CRC, JSON, JSONB)
/// lives here — see `json_transact.hpp` for that layer.

#include <note/bus_lock.hpp>  // unconditional: NullLock is the default Lock template param
#include <note/error.hpp>
#include <note/transport.hpp>
#include <note/transport_hal.hpp>

#include <algorithm>
#include <cstring>

namespace note {

// ---------------------------------------------------------------------------
// HalByteTransport — polymorphic byte transport over a `Hal&`
// ---------------------------------------------------------------------------

class HalByteTransport : public IByteTransport {
public:
    explicit HalByteTransport(Hal& hal) : hal_(hal) {}

    Result<void> begin_transaction(uint32_t /*timeout_ms*/) override {
        if (!ensure_init())
            return make_error(Error::NotReady, NOTE_ERR("not ready"));
        frame_terminated_ = false;
        any_data_received_ = false;
        return {};
    }

    void end_transaction() override {
        if (!frame_terminated_ && any_data_received_)
            drain_frame_boundary();
    }

    Result<void> write(const uint8_t* data, size_t len) override {
        if (!hal_.transmit(data, len))
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("transmit failed"));
        return {};
    }

    Result<void> write_frame_terminator() override {
        if (!hal_.write_line_terminator())
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("line terminator failed"));
        return {};
    }

    Result<size_t> read(uint8_t* buf, size_t max, uint32_t timeout_ms) override {
        if (frame_terminated_)
            return make_error(Error::EndOfFrame, NOTE_ERR("frame complete"));

        size_t n = 0;
        uint32_t start = hal_.millis();
        while (n == 0) {
            auto r = read_hal(buf, max, timeout_ms);
            if (!r) return r;
            n = *r;
            if (n == 0) {
                if (timeout_ms > 0 && hal_.millis() - start >= timeout_ms)
                    return make_error(Error::ResponseLost, Cause::Timeout,
                                      NOTE_ERR("timeout"));
                hal_.delay(1);
            }
        }
        any_data_received_ = true;

        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                frame_terminated_ = true;
                size_t after = n - i - 1;
                if (after > 0 && after <= sizeof(lookahead_)) {
                    std::memcpy(lookahead_, buf + i + 1, after);
                    lookahead_pos_ = 0;
                    lookahead_len_ = after;
                }
                return i;
            }
        }
        return n;
    }

    void reset() override {
        hal_.reset();
        initialized_ = false;
    }
    void abort() override {}
    Hal& hal() override { return hal_; }

private:
    Result<size_t> read_hal(uint8_t* buf, size_t max, uint32_t timeout_ms) {
        if (lookahead_len_ > 0) {
            size_t n = std::min(max, lookahead_len_);
            std::memcpy(buf, lookahead_ + lookahead_pos_, n);
            lookahead_pos_ += n;
            lookahead_len_ -= n;
            return n;
        }
        return hal_.read(buf, max, timeout_ms);
    }

    bool ensure_init() {
        if (initialized_) return true;
        if (!hal_.reset()) return false;
        initialized_ = true;
        return true;
    }

    void drain_frame_boundary() {
        uint8_t byte;
        for (size_t guard = 0; guard < 256; ++guard) {
            auto rv = read_hal(&byte, 1, 250);
            if (!rv || *rv == 0) break;
            if (byte == '\n') break;
        }
    }

    Hal& hal_;
    bool initialized_ = false;
    bool frame_terminated_ = false;
    bool any_data_received_ = false;
    uint8_t lookahead_[64]{};
    size_t lookahead_pos_ = 0;
    size_t lookahead_len_ = 0;
};

// ---------------------------------------------------------------------------
// HalByteTransportT<HalT> — templated, no-vtable byte transport
// ---------------------------------------------------------------------------

/// Templated byte transport over any Hal-shaped type. No virtual methods,
/// so concrete templated HALs (e.g. `SerialFramer<SerialHal<HardwareSerial>>`)
/// can be inlined through both this layer and the wire-format layer above.
///
/// The optional `Lock` template parameter accepts any type with `lock()` and
/// `unlock()` methods (a C++ Lockable). The default is `NullLock`, which has
/// no vtable and no state. It is held as a private empty base, so empty-base
/// optimization gives it zero size overhead — single-threaded and AVR users
/// pay nothing. (EBO is used rather than a `[[no_unique_address]]` member
/// because the AVR toolchain is GCC 7.3, which predates that C++20 attribute.)
///
/// To protect a shared I2C/serial bus from concurrent access, pass a lock
/// type whose `lock()`/`unlock()` invoke your RTOS or platform mutex:
/// @code
///   using MyTransport = note::HalByteTransportT<MyHal, MyMutex>;
/// @endcode
template<typename HalT, typename Lock = NullLock>
class HalByteTransportT : private Lock {
public:
    explicit HalByteTransportT(HalT& hal) : hal_(hal) {}

    Result<void> begin_transaction(uint32_t) {
        Lock::lock();
        if (!ensure_init()) {
            Lock::unlock();
            return make_error(Error::NotReady, NOTE_ERR("not ready"));
        }
        frame_terminated_ = false;
        any_data_received_ = false;
        return {};
    }

    void end_transaction() {
        if (!frame_terminated_ && any_data_received_)
            drain_frame_boundary();
        Lock::unlock();
    }

    Result<void> write(const uint8_t* data, size_t len) {
        if (!hal_.transmit(data, len))
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("transmit failed"));
        return {};
    }

    Result<void> write_frame_terminator() {
        if (!hal_.write_line_terminator())
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("line terminator failed"));
        return {};
    }

    Result<size_t> read(uint8_t* buf, size_t max, uint32_t timeout_ms) {
        if (frame_terminated_)
            return make_error(Error::EndOfFrame, NOTE_ERR("frame complete"));
        size_t n = 0;
        uint32_t start = hal_.millis();
        while (n == 0) {
            auto r = read_hal(buf, max, timeout_ms);
            if (!r) return r;
            n = *r;
            if (n == 0) {
                if (timeout_ms > 0 && hal_.millis() - start >= timeout_ms)
                    return make_error(Error::ResponseLost, Cause::Timeout,
                                      NOTE_ERR("timeout"));
                hal_.delay(1);
            }
        }
        any_data_received_ = true;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                frame_terminated_ = true;
#if !NOTE_NO_POLYMORPHIC
                // Lookahead is only needed when binary I/O can interleave
                // with JSON responses (the polymorphic Notecard supports
                // card.binary; minimal AVR builds don't).
                size_t after = n - i - 1;
                if (after > 0 && after <= sizeof(lookahead_)) {
                    std::memcpy(lookahead_, buf + i + 1, after);
                    lookahead_pos_ = 0;
                    lookahead_len_ = after;
                }
#endif
                return i;
            }
        }
        return n;
    }

    void reset() { hal_.reset(); initialized_ = false; }
    void abort() {}
    HalT& hal() { return hal_; }

private:
    Result<size_t> read_hal(uint8_t* buf, size_t max, uint32_t timeout_ms) {
#if !NOTE_NO_POLYMORPHIC
        if (lookahead_len_ > 0) {
            size_t n = std::min(max, lookahead_len_);
            std::memcpy(buf, lookahead_ + lookahead_pos_, n);
            lookahead_pos_ += n;
            lookahead_len_ -= n;
            return n;
        }
#endif
        return hal_.read(buf, max, timeout_ms);
    }

    bool ensure_init() {
        if (initialized_) return true;
        if (!hal_.reset()) return false;
        initialized_ = true;
        return true;
    }

    void drain_frame_boundary() {
        uint8_t byte;
        for (size_t guard = 0; guard < 256; ++guard) {
            auto rv = read_hal(&byte, 1, 250);
            if (!rv || *rv == 0) break;
            if (byte == '\n') break;
        }
    }

    HalT& hal_;
    bool initialized_ = false;
    bool frame_terminated_ = false;
    bool any_data_received_ = false;
#if !NOTE_NO_POLYMORPHIC
    uint8_t lookahead_[64]{};
    size_t lookahead_pos_ = 0;
    size_t lookahead_len_ = 0;
#endif
};

} // namespace note
