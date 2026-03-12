#pragma once

#include <note/error.hpp>
#include <note/transport/detail/crc32.hpp>
#include <note/transport/protocol_policy.hpp>
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
// The PolicyType template parameter controls retry counts, segment pacing,
// and timeouts. See protocol_policy.hpp for details.
//
// Usage — compile-time default policy (zero overhead, most common):
//
//   NotecardI2c transport(hal);
//
// Usage — compile-time custom policy (zero overhead):
//
//   NotecardI2c<StaticI2cPolicy<I2cPolicy::fast()>> transport(hal);
//
// Usage — runtime mutable policy:
//
//   NotecardI2c<I2cPolicy> transport(hal);
//   transport.policy.max_retries = 1;  // adjust before a destructive request

namespace note::transport {

// Notecard default I2C address (NOTE_I2C_ADDR_DEFAULT).
inline constexpr uint16_t kI2cDefaultAddress = 0x17;

// I2C MTU constants — hardware transfer size limits, not protocol policy.
inline constexpr size_t kI2cDefaultMtu = 30;   // safe for all Arduino Wire implementations
inline constexpr size_t kI2cMaxMtu     = 253;  // UCHAR_MAX - 2 byte header

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
    //   len == 0: priming query — no bytes consumed; available is set to the
    //             number of bytes the Notecard has pending.
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

template <typename PolicyType = StaticI2cPolicy<I2cPolicy{}>>
class NotecardI2c {
public:
    // policy is public so callers can read or mutate it between requests.
    // For StaticI2cPolicy (the default), [[no_unique_address]] gives it
    // zero bytes — all fields are static constexpr, folded by the compiler.
    [[no_unique_address]] PolicyType policy;

    explicit NotecardI2c(I2cHal& hal, PolicyType pol = {})
        : policy(pol), hal_(hal) {}

    // Satisfies note::Notecard's RequestFn signature:
    //   std::function<Result<std::string>(string_view, uint32_t)>
    //
    // Auto-resets on first call. Handles CRC (auto-detected), chunked TX
    // with IO + segment pacing, priming-query RX, and up to policy.max_retries
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

        ErrorInfo last_error{Error::SendFailed, Cause::HalError, "I2C transmit failed"};

        for (uint32_t attempt = 0; attempt <= policy.max_retries; ++attempt) {
            if (attempt > 0) hal_.delay(policy.retry_delay_ms);

            if (!send_chunked(wire.data(), wire.size())) {
                last_error = {Error::SendFailed, Cause::HalError, "I2C transmit failed"};
                do_reset();
                continue;
            }

            auto result = receive_response(timeout_ms);
            if (!result) {
                last_error = result.error();
                continue;
            }

            if (detail::crc_check_and_strip(*result, crc_seq_, crc_enabled_)) {
                last_error = {Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch"};
                continue;
            }

            return result;
        }

        return Unexpected(last_error);
    }

private:
    I2cHal&  hal_;
    bool     initialized_ = false;
    bool     crc_enabled_ = false;
    uint16_t crc_seq_     = 0;

    // -----------------------------------------------------------------------
    // delay_io — matches note-c _delayIO()
    // Empirical stability delay before each I2C operation.
    // -----------------------------------------------------------------------
    void delay_io() {
        if (policy.io_delay_ms > 0) hal_.delay(policy.io_delay_ms);
    }

    // -----------------------------------------------------------------------
    // do_reset — matches note-c _i2cNoteReset()
    // Hardware reset + sync: send \n and drain until only \r/\n received
    // for policy.reset_drain_ms (clean window), then declare ready.
    // -----------------------------------------------------------------------
    bool do_reset() {
        hal_.delay(policy.segment_delay_ms);  // 250 ms pre-delay
        if (!hal_.reset()) return false;
        delay_io();  // stability delay after hardware reset

        for (uint32_t retry = 0; retry < policy.reset_sync_retries; ++retry) {
            // Send \n to sync. On NACK (transmit failure): wait and retry.
            const uint8_t nl = '\n';
            if (!hal_.transmit(&nl, 1)) {
                hal_.delay(policy.nack_wait_ms);  // 1000 ms after NACK
                continue;
            }

            hal_.delay(policy.segment_delay_ms);  // 250 ms after transmit

            // Drain loop for policy.reset_drain_ms (500 ms).
            // chunk_len starts at 0 (priming query) and is updated to the
            // number of bytes the Notecard has pending after each receive.
            uint32_t drain_start       = hal_.millis();
            size_t   chunk_len         = 0;
            bool     found_something   = false;
            bool     found_non_control = false;
            uint8_t  buf[64];

            while (hal_.millis() - drain_start < policy.reset_drain_ms) {
                uint32_t available = 0;

                // Clamp requested bytes to buffer and HAL max_transfer.
                const size_t req = std::min({chunk_len, sizeof(buf),
                                             hal_.max_transfer()});

                if (!hal_.receive(buf, req, available)) {
                    // I2C bus error — delay and keep draining.
                    hal_.delay(policy.segment_delay_ms);
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
                hal_.delay(policy.chunk_delay_ms);  // 20 ms between drain polls
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
    // policy.chunk_delay_ms inter-chunk delay, and policy.segment_delay_ms
    // after every policy.segment_max_len (250) bytes.
    // -----------------------------------------------------------------------
    bool send_chunked(const char* data, size_t len) {
        size_t offset        = 0;
        size_t sentInSegment = 0;

        while (offset < len) {
            const size_t chunk = std::min(len - offset, hal_.max_transfer());

            delay_io();  // stability delay before each chunk
            if (!hal_.transmit(reinterpret_cast<const uint8_t*>(data + offset), chunk)) {
                hal_.reset();
                return false;
            }

            offset        += chunk;
            sentInSegment += chunk;

            // Segment pacing: after more than policy.segment_max_len consecutive
            // bytes, pause to avoid overwhelming the Notecard.
            if (sentInSegment > policy.segment_max_len) {
                sentInSegment = 0;
                hal_.delay(policy.segment_delay_ms);
            }
            if (policy.chunk_delay_ms > 0) hal_.delay(policy.chunk_delay_ms);
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
        delay_io();  // stability delay after final transmit chunk

        // Priming query loop: wait until Notecard has data available.
        uint32_t available = 0;
        {
            uint8_t dummy = 0;
            const uint32_t start = hal_.millis();
            while (available == 0) {
                if (!hal_.receive(&dummy, 0, available))
                    return make_error(Error::ResponseLost, Cause::HalError, "I2C receive failed");
                if (available == 0) {
                    if (timeout_ms && hal_.millis() - start >= timeout_ms)
                        return make_error(Error::ResponseLost, Cause::Timeout, "no response");
                    hal_.delay(policy.response_poll_ms);  // 50 ms poll interval
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
                return make_error(Error::ResponseLost, Cause::HalError, "I2C receive failed");

            if (req > 0) {
                buf.append(reinterpret_cast<const char*>(chunk), req);
                intra_start = hal_.millis();  // reset intra-timeout on data
                eop = eop || (buf.back() == '\n');
            }

            if (available > 0) continue;  // more pending (drain even if EOP)
            if (eop) break;               // EOP with no more data — done

            // Nothing available yet and no EOP — wait for more.
            if (hal_.millis() - intra_start >= policy.intra_timeout_ms)
                return make_error(Error::ResponseLost, Cause::TimeoutIntra, "response incomplete");
            hal_.delay(policy.response_poll_ms);  // 50 ms poll
        }

        // Strip trailing \r\n.
        while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r'))
            buf.pop_back();

        return buf;
    }
};

// Deduction guides — allow construction without explicit template arguments.
//
//   NotecardI2c transport(hal)          → StaticI2cPolicy<I2cPolicy{}>
//                                         (zero overhead, compile-time defaults)
//   NotecardI2c transport(hal, policy)  → I2cPolicy
//                                         (runtime mutable)
NotecardI2c(I2cHal&) -> NotecardI2c<StaticI2cPolicy<I2cPolicy{}>>;
NotecardI2c(I2cHal&, I2cPolicy) -> NotecardI2c<I2cPolicy>;

template <I2cPolicy P>
NotecardI2c(I2cHal&, StaticI2cPolicy<P>) -> NotecardI2c<StaticI2cPolicy<P>>;

// ---------------------------------------------------------------------------
// Backward-compatible constants (derived from default policy values).
// ---------------------------------------------------------------------------

inline constexpr uint32_t kI2cIoDelayMs        = I2cPolicy{}.io_delay_ms;
inline constexpr uint32_t kI2cSegmentMaxLen    = I2cPolicy{}.segment_max_len;
inline constexpr uint32_t kI2cSegmentDelayMs   = I2cPolicy{}.segment_delay_ms;
inline constexpr uint32_t kI2cChunkDelayMs     = I2cPolicy{}.chunk_delay_ms;
inline constexpr uint32_t kI2cNackWaitMs       = I2cPolicy{}.nack_wait_ms;
inline constexpr uint32_t kI2cResetDrainMs     = I2cPolicy{}.reset_drain_ms;
inline constexpr uint32_t kI2cResetSyncRetries = I2cPolicy{}.reset_sync_retries;
inline constexpr uint32_t kI2cResponsePollMs   = I2cPolicy{}.response_poll_ms;
inline constexpr uint32_t kI2cIntraTimeoutMs   = I2cPolicy{}.intra_timeout_ms;
inline constexpr uint32_t kI2cMaxRetries       = I2cPolicy{}.max_retries;
inline constexpr uint32_t kI2cRetryDelayMs     = I2cPolicy{}.retry_delay_ms;

}  // namespace note::transport
