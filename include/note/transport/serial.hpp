#pragma once

#include <note/error.hpp>
#include <note/transport/detail/crc32.hpp>
#include <note/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// note::transport::NotecardSerial
//
// Implements the Notecard serial wire protocol in C++, providing a complete
// alternative to the note-c serial transport. Faithful port of note-c
// n_serial.c + n_request.c (retry, CRC) with a platform-injectable HAL.
//
// Usage — platform subclass:
//
//   class MySerial : public note::transport::SerialHal { ... };
//   MySerial hal;
//   note::transport::NotecardSerial transport(hal);
//   note::Notecard nc(backend, [&transport](note::string_view req, uint32_t t) {
//       return transport(req, t);
//   });
//
// Usage — lambda/callback (tests, host tooling):
//
//   note::transport::SerialCallbackHal hal{tx_fn, rx_fn, millis_fn, delay_fn};
//   note::transport::NotecardSerial transport(hal);

namespace note::transport {

// ---------------------------------------------------------------------------
// Protocol timing constants (matching note-c n_lib.h / n_request.c)
// ---------------------------------------------------------------------------

inline constexpr uint32_t kSerialSegmentMaxLen       = 250;   // bytes per TX segment
inline constexpr uint32_t kSerialSegmentDelayMs      = 250;   // inter-segment delay
inline constexpr uint32_t kIntraTransactionTimeoutMs = 1000;  // timeout after first byte
inline constexpr uint32_t kResetDrainMs              = 500;   // drain window per reset attempt
inline constexpr uint32_t kResetSyncRetries          = 10;    // max reset attempts
inline constexpr uint32_t kMaxRetries                = 5;     // max transaction retries
inline constexpr uint32_t kRetryDelayMs              = 500;   // delay between retries

// ---------------------------------------------------------------------------
// SerialHal — pure virtual platform interface
// ---------------------------------------------------------------------------

class SerialHal {
public:
    virtual ~SerialHal() = default;

    // Send all len bytes. Returns false on hardware error.
    virtual bool     transmit(const uint8_t* data, size_t len) = 0;

    // Non-blocking read. Returns 0..max_len bytes currently available.
    // Must not block waiting for more data.
    virtual size_t   receive(uint8_t* buf, size_t max_len) = 0;

    // Monotonic millisecond counter (wraps after ~49 days — same as Arduino millis()).
    virtual uint32_t millis() = 0;

    // Block for exactly ms milliseconds.
    virtual void     delay(uint32_t ms) = 0;
};

// ---------------------------------------------------------------------------
// SerialCallbackHal — wraps four lambdas/function pointers as a SerialHal
// ---------------------------------------------------------------------------

class SerialCallbackHal : public SerialHal {
public:
    using TransmitFn = std::function<bool(const uint8_t*, size_t)>;
    using ReceiveFn  = std::function<size_t(uint8_t*, size_t)>;
    using MillisFn   = std::function<uint32_t()>;
    using DelayFn    = std::function<void(uint32_t)>;

    SerialCallbackHal(TransmitFn tx, ReceiveFn rx, MillisFn ms, DelayFn dl)
        : tx_(std::move(tx)), rx_(std::move(rx))
        , millis_(std::move(ms)), delay_(std::move(dl)) {}

    bool     transmit(const uint8_t* d, size_t n) override { return tx_(d, n); }
    size_t   receive(uint8_t* b, size_t n)         override { return rx_(b, n); }
    uint32_t millis()                              override { return millis_(); }
    void     delay(uint32_t ms)                    override { delay_(ms); }

private:
    TransmitFn tx_;
    ReceiveFn  rx_;
    MillisFn   millis_;
    DelayFn    delay_;
};

// ---------------------------------------------------------------------------
// NotecardSerial — Notecard serial protocol implementation
// ---------------------------------------------------------------------------

class NotecardSerial {
public:
    explicit NotecardSerial(SerialHal& hal) : hal_(hal) {}

    // Satisfies note::Notecard's RequestFn signature:
    //   std::function<Result<std::string>(string_view, uint32_t)>
    //
    // Auto-resets on first call. Handles CRC (auto-detected), segmented TX,
    // greedy RX, and up to kMaxRetries retries on failure.
    Result<std::string> operator()(string_view request, uint32_t timeout_ms) {
        if (!initialized_) {
            if (!do_reset())
                return make_error(Error::NotReady, "Notecard not ready after reset");
            initialized_ = true;
        }

        // Build wire request once (add CRC if firmware has indicated support).
        // The same seqno is used for all retries of this request — matches note-c,
        // where _crcAdd() is called once before the retry loop and seqNo is only
        // incremented after a successful transaction.
        std::string wire(request);
        if (crc_enabled_) {
            ++crc_seq_;
            wire = detail::crc_add(std::move(wire), crc_seq_);
        }

        for (uint32_t attempt = 0; attempt <= kMaxRetries; ++attempt) {
            if (attempt > 0) hal_.delay(kRetryDelayMs);

            // Segmented transmit.
            if (!send_segmented(wire.data(), wire.size())) {
                do_reset();
                continue;
            }

            // Receive until newline.
            auto result = receive_line(timeout_ms);
            if (!result) continue;

            // CRC validation + auto-detection (mutates *result, sets crc_enabled_).
            if (detail::crc_check_and_strip(*result, crc_seq_, crc_enabled_)) continue;

            return result;
        }

        return make_error(Error::Transport, "Notecard: max retries exceeded");
    }

private:
    SerialHal& hal_;
    bool       initialized_ = false;
    bool       crc_enabled_ = false;
    uint16_t   crc_seq_     = 0;

    // -----------------------------------------------------------------------
    // reset — matches note-c _serialNoteReset()
    // Send \n and drain until only \r/\n received for kResetDrainMs.
    // -----------------------------------------------------------------------
    bool do_reset() {
        hal_.delay(kSerialSegmentDelayMs);  // 250 ms pre-delay

        for (uint32_t retry = 0; retry < kResetSyncRetries; ++retry) {
            const uint8_t nl = '\n';
            hal_.transmit(&nl, 1);

            uint8_t buf[32];
            bool found_something    = false;
            bool found_non_control  = false;
            uint32_t drain_start    = hal_.millis();

            while (hal_.millis() - drain_start < kResetDrainMs) {
                size_t n = hal_.receive(buf, sizeof(buf));
                for (size_t i = 0; i < n; ++i) {
                    found_something = true;
                    if (buf[i] != '\n' && buf[i] != '\r') {
                        found_non_control = true;
                        drain_start = hal_.millis();  // extend window
                    }
                }
                if (n == 0) hal_.delay(1);
            }

            if (found_something && !found_non_control) return true;

            hal_.delay(kResetDrainMs);
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // send_segmented — matches note-c _serialChunkedTransmit()
    // Transmit in kSerialSegmentMaxLen chunks, delay between them.
    // Appends \r\n terminator (matches note-c c_newline).
    // -----------------------------------------------------------------------
    bool send_segmented(const char* data, size_t len) {
        size_t offset = 0;
        size_t rem    = len;
        while (rem > 0) {
            const size_t chunk = std::min(rem, size_t(kSerialSegmentMaxLen));
            if (!hal_.transmit(reinterpret_cast<const uint8_t*>(data + offset), chunk))
                return false;
            offset += chunk;
            rem    -= chunk;
            if (rem > 0) hal_.delay(kSerialSegmentDelayMs);
        }
        const uint8_t crlf[2] = {'\r', '\n'};
        return hal_.transmit(crlf, 2);
    }

    // -----------------------------------------------------------------------
    // receive_line — matches note-c _serialChunkedReceive()
    // Poll until \n received. Initial timeout = timeout_ms; after first byte
    // switches to kIntraTransactionTimeoutMs (1 s).
    // Returns JSON stripped of trailing \r\n.
    // -----------------------------------------------------------------------
    Result<std::string> receive_line(uint32_t timeout_ms) {
        std::string  buf;
        uint8_t      chunk[64];
        bool         first_byte_seen = false;
        uint32_t     start           = hal_.millis();
        uint32_t     intra_start     = 0;

        while (true) {
            const size_t n = hal_.receive(chunk, sizeof(chunk));
            if (n > 0) {
                if (!first_byte_seen) {
                    first_byte_seen = true;
                    intra_start     = hal_.millis();
                }
                buf.append(reinterpret_cast<const char*>(chunk), n);
                if (buf.back() == '\n') {
                    while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r'))
                        buf.pop_back();
                    return buf;
                }
            } else {
                hal_.delay(1);
                const uint32_t now = hal_.millis();
                if (!first_byte_seen) {
                    if (timeout_ms && (now - start >= timeout_ms))
                        return make_error(Error::Timeout, "Notecard: no response");
                } else {
                    if (now - intra_start >= kIntraTransactionTimeoutMs)
                        return make_error(Error::Timeout, "Notecard: response incomplete");
                }
            }
        }
    }
};

}  // namespace note::transport
