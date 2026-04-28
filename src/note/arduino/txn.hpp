/// @file arduino/txn.hpp
/// Reference TxnHandshake implementation for Arduino platforms that
/// drives CTX/RTX GPIO lines during each Notecard transaction. Mirrors
/// the behaviour of note-arduino's `NoteTxn_Arduino.cpp`.
///
/// Typical usage:
/// @code
///     note::arduino::GpioTxnHandshake handshake(CTX_PIN, RTX_PIN);
///     nc.begin(Serial1, 9600);
///     nc.set_handshake(handshake);   // brackets every transaction
/// @endcode
///
/// Protocol:
///   - idle:  both CTX and RTX floating (INPUT / INPUT_ANALOG on STM32)
///            to conserve Notecard power.
///   - start: drive RTX high (request to transact); wait for CTX high
///            (clear to transact) with INPUT_PULLUP on CTX. Timeout →
///            return false; Protocol surfaces Error::NotReady.
///   - stop:  float RTX (release) so the Notecard is free to sleep.

#pragma once

#include <note/note_config.hpp>

#if NOTE_TXN_HANDSHAKE

#include <note/txn_handshake.hpp>

#ifdef ARDUINO
#include <Arduino.h>
#else
// Non-Arduino host builds: provide just enough to compile-check the
// interface. Real behaviour lives under Arduino.
#endif

#include <stdint.h>

namespace note::arduino {

class GpioTxnHandshake final : public ::note::TxnHandshake {
public:
    GpioTxnHandshake(uint8_t ctx_pin, uint8_t rtx_pin)
        : ctx_pin_(ctx_pin), rtx_pin_(rtx_pin)
    {
#ifdef ARDUINO
        // Float both pins at rest to conserve Notecard power.
    #ifdef ARDUINO_ARCH_STM32
        ::pinMode(ctx_pin_, INPUT_ANALOG);
        ::pinMode(rtx_pin_, INPUT_ANALOG);
    #else
        ::pinMode(ctx_pin_, INPUT);
        ::pinMode(rtx_pin_, INPUT);
    #endif
#endif
    }

    bool start(uint32_t timeout_ms) override {
#ifdef ARDUINO
        // Request To Transact: drive RTX high.
        ::pinMode(rtx_pin_, OUTPUT);
        ::digitalWrite(rtx_pin_, HIGH);

        // Wait for Clear To Transact on CTX.
        ::pinMode(ctx_pin_, INPUT_PULLUP);

        bool ok = false;
        const uint32_t deadline = millis() + timeout_ms;
        while (millis() < deadline) {
            if (::digitalRead(ctx_pin_) == HIGH) { ok = true; break; }
            ::delay(1);
        }

        // Float CTX again.
    #ifdef ARDUINO_ARCH_STM32
        ::pinMode(ctx_pin_, INPUT_ANALOG);
    #else
        ::pinMode(ctx_pin_, INPUT);
    #endif

        if (!ok) stop();
        return ok;
#else
        (void)timeout_ms;
        return true;
#endif
    }

    void stop() override {
#ifdef ARDUINO
        // Float RTX — signals "done, you may sleep".
    #ifdef ARDUINO_ARCH_STM32
        ::pinMode(rtx_pin_, INPUT_ANALOG);
    #else
        ::pinMode(rtx_pin_, INPUT);
    #endif
#endif
    }

private:
    uint8_t ctx_pin_;
    uint8_t rtx_pin_;
};

} // namespace note::arduino

#endif // NOTE_TXN_HANDSHAKE
