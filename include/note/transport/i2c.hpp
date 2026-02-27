#pragma once

#include <note/error.hpp>
#include <note/transport/detail/crc32.hpp>
#include <note/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// note::transport::NotecardI2c
//
// Implements the Notecard I2C wire protocol in C++, providing a complete
// alternative to the note-c I2C transport. Faithful port of note-c
// n_i2c.c (reset, chunked TX/RX with segment pacing, CRC) with a
// platform-injectable HAL.
//
// Usage — platform subclass:
//
//   class MyI2c : public note::transport::I2cHal { ... };
//   MyI2c hal;
//   note::transport::NotecardI2c transport(hal);
//   note::Notecard nc(backend, [&transport](note::string_view req, uint32_t t) {
//       return transport(req, t);
//   });
//
// Usage — lambda/callback (tests, host tooling):
//
//   note::transport::I2cCallbackHal hal{reset_fn, tx_fn, rx_fn, millis_fn, delay_fn};
//   note::transport::NotecardI2c transport(hal);

namespace note::transport {

// ---------------------------------------------------------------------------
// Protocol timing constants (matching note-c n_lib.h)
// ---------------------------------------------------------------------------

inline constexpr uint16_t kI2cDefaultAddress  = 0x17;  // NOTE_I2C_ADDR_DEFAULT
inline constexpr size_t   kI2cDefaultMtu       = 30;    // NOTE_I2C_MTU_DEFAULT (safe for all Arduino)
inline constexpr size_t   kI2cMaxMtu           = 253;   // NOTE_I2C_MTU_MAX (UCHAR_MAX - header size)
inline constexpr uint32_t kI2cIoDelayMs        = 6;     // empirical stability delay (_delayIO)
inline constexpr uint32_t kI2cSegmentMaxLen    = 250;   // CARD_REQUEST_I2C_SEGMENT_MAX_LEN
inline constexpr uint32_t kI2cSegmentDelayMs   = 250;   // CARD_REQUEST_I2C_SEGMENT_DELAY_MS
inline constexpr uint32_t kI2cChunkDelayMs     = 20;    // CARD_REQUEST_I2C_CHUNK_DELAY_MS
inline constexpr uint32_t kI2cNackWaitMs       = 1000;  // CARD_REQUEST_I2C_NACK_WAIT_MS
inline constexpr uint32_t kI2cResetDrainMs     = 500;   // CARD_RESET_DRAIN_MS
inline constexpr uint32_t kI2cResetSyncRetries = 10;    // CARD_RESET_SYNC_RETRIES
inline constexpr uint32_t kI2cResponsePollMs   = 50;    // polling interval waiting for data
inline constexpr uint32_t kI2cIntraTimeoutMs   = 1000;  // CARD_INTRA_TRANSACTION_TIMEOUT_SEC * 1000
inline constexpr uint32_t kI2cMaxRetries       = 5;     // max transaction retries
inline constexpr uint32_t kI2cRetryDelayMs     = 500;   // delay between retries

// ---------------------------------------------------------------------------
// I2cHal — pure virtual platform interface
// ---------------------------------------------------------------------------

class I2cHal {
public:
    virtual ~I2cHal() = default;

    // Hardware-level I2C reset. Returns false on failure.
    virtual bool     reset() = 0;

    // Transmit len bytes to the Notecard. Returns false on error (e.g. NACK).
    virtual bool     transmit(const uint8_t* data, size_t len) = 0;

    // Receive from the Notecard.
    //   len == 0: priming query — no bytes consumed; available is set to
    //             the number of bytes the Notecard has pending.
    //   len  > 0: read exactly len bytes into buf; available is set to the
    //             number of bytes still pending after this read.
    // Returns false on error.
    virtual bool     receive(uint8_t* buf, size_t len, uint32_t& available) = 0;

    // Monotonic millisecond counter (wraps after ~49 days).
    virtual uint32_t millis() = 0;

    // Block for exactly ms milliseconds.
    virtual void     delay(uint32_t ms) = 0;

    // Maximum bytes per I2C chunk. Defaults to kI2cDefaultMtu (30 bytes),
    // which is safe for all Arduino Wire implementations. Override for
    // platforms with a larger I2C buffer (e.g. STM32: 253 bytes).
    virtual size_t   max_transfer() { return kI2cDefaultMtu; }
};

// ---------------------------------------------------------------------------
// I2cCallbackHal — wraps lambdas/function pointers as an I2cHal
// ---------------------------------------------------------------------------

class I2cCallbackHal : public I2cHal {
public:
    using ResetFn    = std::function<bool()>;
    using TransmitFn = std::function<bool(const uint8_t*, size_t)>;
    using ReceiveFn  = std::function<bool(uint8_t*, size_t, uint32_t&)>;
    using MillisFn   = std::function<uint32_t()>;
    using DelayFn    = std::function<void(uint32_t)>;

    I2cCallbackHal(ResetFn rst, TransmitFn tx, ReceiveFn rx,
                   MillisFn ms, DelayFn dl,
                   size_t mtu = kI2cDefaultMtu)
        : rst_(std::move(rst)), tx_(std::move(tx)), rx_(std::move(rx))
        , millis_(std::move(ms)), delay_(std::move(dl)), mtu_(mtu) {}

    bool     reset()                                          override { return rst_(); }
    bool     transmit(const uint8_t* d, size_t n)            override { return tx_(d, n); }
    bool     receive(uint8_t* b, size_t n, uint32_t& avail)  override { return rx_(b, n, avail); }
    uint32_t millis()                                        override { return millis_(); }
    void     delay(uint32_t ms)                              override { delay_(ms); }
    size_t   max_transfer()                                  override { return mtu_; }

private:
    ResetFn    rst_;
    TransmitFn tx_;
    ReceiveFn  rx_;
    MillisFn   millis_;
    DelayFn    delay_;
    size_t     mtu_;
};

// ---------------------------------------------------------------------------
// NotecardI2c — Notecard I2C protocol implementation
// ---------------------------------------------------------------------------

class NotecardI2c {
public:
    explicit NotecardI2c(I2cHal& hal) : hal_(hal) {}

    // Satisfies note::Notecard's RequestFn signature:
    //   std::function<Result<std::string>(string_view, uint32_t)>
    //
    // Auto-resets on first call. Handles CRC (auto-detected), chunked TX
    // with IO + segment pacing, priming-query RX, and up to kI2cMaxRetries
    // retries on failure.
    Result<std::string> operator()(string_view request, uint32_t timeout_ms) {
        if (!initialized_) {
            if (!do_reset())
                return make_error(Error::NotReady, "Notecard not ready after reset");
            initialized_ = true;
        }

        // Build wire request once (CRC same seq for all retries — matches
        // note-c, where _crcAdd() is called once before the retry loop and
        // seqNo is only incremented after a successful transaction).
        std::string wire(request);
        if (crc_enabled_) {
            ++crc_seq_;
            wire = detail::crc_add(std::move(wire), crc_seq_);
        }
        wire += '\n';  // I2C uses bare \n terminator (not \r\n like serial)

        for (uint32_t attempt = 0; attempt <= kI2cMaxRetries; ++attempt) {
            if (attempt > 0) hal_.delay(kI2cRetryDelayMs);

            if (!send_chunked(wire.data(), wire.size())) {
                do_reset();
                continue;
            }

            auto result = receive_response(timeout_ms);
            if (!result) continue;

            if (detail::crc_check_and_strip(*result, crc_seq_, crc_enabled_)) continue;

            return result;
        }

        return make_error(Error::Transport, "Notecard: max retries exceeded");
    }

private:
    I2cHal&  hal_;
    bool     initialized_ = false;
    bool     crc_enabled_ = false;
    uint16_t crc_seq_     = 0;

    // -----------------------------------------------------------------------
    // delay_io — matches note-c _delayIO()
    // Empirical 6 ms stability delay before each I2C operation.
    // -----------------------------------------------------------------------
    void delay_io() { hal_.delay(kI2cIoDelayMs); }

    // -----------------------------------------------------------------------
    // do_reset — matches note-c _i2cNoteReset()
    // Hardware reset + sync: send \n and drain until only \r/\n received
    // for kI2cResetDrainMs, then declare ready.
    // -----------------------------------------------------------------------
    bool do_reset() {
        hal_.delay(kI2cSegmentDelayMs);  // 250 ms pre-delay
        if (!hal_.reset()) return false;
        delay_io();  // 6 ms after hardware reset

        for (uint32_t retry = 0; retry < kI2cResetSyncRetries; ++retry) {
            // Send \n to sync. On NACK (transmit failure): wait and retry.
            const uint8_t nl = '\n';
            if (!hal_.transmit(&nl, 1)) {
                hal_.delay(kI2cNackWaitMs);  // 1000 ms after NACK
                continue;
            }

            hal_.delay(kI2cSegmentDelayMs);  // 250 ms after transmit

            // Drain loop for kI2cResetDrainMs (500 ms).
            // chunk_len starts at 0 (priming query) and is updated to the
            // number of bytes the Notecard has pending after each receive.
            uint32_t drain_start     = hal_.millis();
            size_t   chunk_len       = 0;
            bool     found_something   = false;
            bool     found_non_control = false;
            uint8_t  buf[64];

            while (hal_.millis() - drain_start < kI2cResetDrainMs) {
                uint32_t available = 0;

                // Clamp requested bytes to buffer and HAL max_transfer.
                const size_t req = std::min({chunk_len, sizeof(buf),
                                             hal_.max_transfer()});

                if (!hal_.receive(buf, req, available)) {
                    // I2C bus error — delay and keep draining.
                    hal_.delay(kI2cSegmentDelayMs);
                    continue;
                }

                if (req > 0) {
                    found_something = true;
                    for (size_t i = 0; i < req; ++i) {
                        if (buf[i] != '\n' && buf[i] != '\r') {
                            found_non_control = true;
                            drain_start = hal_.millis();  // extend window
                        }
                    }
                }

                // Update chunk_len from available count (clamped).
                chunk_len = std::min({size_t(available), sizeof(buf),
                                      hal_.max_transfer()});
                hal_.delay(kI2cChunkDelayMs);  // 20 ms between drain polls
            }

            if (found_something && !found_non_control) return true;

            // Drain was not clean. If nothing received at all, the Notecard
            // may be unresponsive — attempt a hardware reset before retrying.
            if (!found_something) {
                if (!hal_.reset()) return false;
                delay_io();
            }
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // send_chunked — matches note-c _i2cChunkedTransmit()
    // Transmit in max_transfer() chunks with IO delay before each chunk,
    // 20 ms inter-chunk delay, and 250 ms segment delay after every
    // kI2cSegmentMaxLen (250) bytes.
    // -----------------------------------------------------------------------
    bool send_chunked(const char* data, size_t len) {
        size_t offset        = 0;
        size_t sentInSegment = 0;

        while (offset < len) {
            const size_t chunk = std::min(len - offset, hal_.max_transfer());

            delay_io();  // 6 ms before each chunk (matches note-c _delayIO)
            if (!hal_.transmit(reinterpret_cast<const uint8_t*>(data + offset), chunk)) {
                hal_.reset();
                return false;
            }

            offset        += chunk;
            sentInSegment += chunk;

            // Segment pacing: after more than kI2cSegmentMaxLen consecutive
            // bytes, pause 250 ms to avoid overwhelming the Notecard.
            if (sentInSegment > kI2cSegmentMaxLen) {
                sentInSegment = 0;
                hal_.delay(kI2cSegmentDelayMs);  // 250 ms inter-segment
            }
            hal_.delay(kI2cChunkDelayMs);  // 20 ms inter-chunk
        }

        return true;
    }

    // -----------------------------------------------------------------------
    // receive_response — matches note-c _i2cNoteQueryLength + _i2cChunkedReceive()
    // 1. Priming query: poll until Notecard signals data available.
    // 2. Chunked receive: read until \n is received AND available == 0.
    // Returns JSON stripped of trailing \r\n.
    // -----------------------------------------------------------------------
    Result<std::string> receive_response(uint32_t timeout_ms) {
        delay_io();  // 6 ms after final transmit chunk

        // Priming query loop: wait until Notecard has data available.
        uint32_t available = 0;
        {
            uint8_t dummy = 0;
            const uint32_t start = hal_.millis();
            while (available == 0) {
                if (!hal_.receive(&dummy, 0, available))
                    return make_error(Error::Transport, "Notecard: I2C receive failed");
                if (available == 0) {
                    if (timeout_ms && hal_.millis() - start >= timeout_ms)
                        return make_error(Error::Timeout, "Notecard: no response");
                    hal_.delay(kI2cResponsePollMs);  // 50 ms poll interval
                }
            }
        }

        // Chunked receive loop: read until end-of-packet (\n) with no more
        // bytes pending. When available drops to 0 without EOP, a subsequent
        // receive(len=0) acts as a priming query to check for more data —
        // matching note-c _i2cChunkedReceive() behavior.
        std::string  buf;
        uint8_t      chunk[64];
        uint32_t     intra_start = hal_.millis();
        bool         eop = false;

        while (true) {
            const size_t req = std::min({size_t(available),
                                         hal_.max_transfer(),
                                         sizeof(chunk)});

            if (!hal_.receive(chunk, req, available))
                return make_error(Error::Transport, "Notecard: I2C receive failed");

            if (req > 0) {
                buf.append(reinterpret_cast<const char*>(chunk), req);
                intra_start = hal_.millis();  // reset intra-timeout on data
                eop = eop || (buf.back() == '\n');
            }

            if (available > 0) continue;  // more pending (drain even if EOP)
            if (eop) break;               // EOP with no more data — done

            // Nothing available yet and no EOP — wait for more.
            if (hal_.millis() - intra_start >= kI2cIntraTimeoutMs)
                return make_error(Error::Timeout, "Notecard: response incomplete");
            hal_.delay(kI2cResponsePollMs);  // 50 ms poll
        }

        // Strip trailing \r\n.
        while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r'))
            buf.pop_back();

        return buf;
    }
};

}  // namespace note::transport
