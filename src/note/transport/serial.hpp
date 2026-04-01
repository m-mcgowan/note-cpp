#pragma once

#include <note/transport.hpp>
#include <note/transport/protocol_policy.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#ifndef NOTE_NO_STD_STRING
#include <string>
#endif

// note::transport::NotecardSerial
//
// Implements the Notecard serial wire protocol. Extends AbstractTransport
// with serial-specific byte I/O (segmented TX, greedy RX, \r\n framing).
//
// The PolicyType template parameter controls retry counts, segment pacing,
// and timeouts. See protocol_policy.hpp for details.
//
// Usage — compile-time default policy (zero overhead, most common):
//
//   NotecardSerial transport(hal);
//
// Usage — compile-time custom policy (zero overhead):
//
//   NotecardSerial<StaticSerialPolicy<SerialPolicy::fast()>> transport(hal);
//
// Usage — runtime mutable policy (28 bytes overhead):
//
//   NotecardSerial<SerialPolicy> transport(hal);
//   transport.policy.max_retries = 1;  // adjust before a destructive request

namespace note::transport {

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

#if __cplusplus >= 202002L
template <typename PolicyType = StaticSerialPolicy<SerialPolicy{}>>
#else
template <typename PolicyType = SerialPolicy>
#endif
class NotecardSerial : public note::AbstractTransport {
public:
    // policy is public so callers can read or mutate it between requests.
    // For StaticSerialPolicy (the default), [[no_unique_address]] gives it
    // zero bytes — all fields are static constexpr, folded by the compiler.
    [[no_unique_address]] PolicyType policy;

    explicit NotecardSerial(SerialHal& hal, PolicyType pol = {})
        : policy(pol), hal_(hal) {}

protected:
    // ── AbstractTransport building blocks ──────────────────────────────────

    // Segmented transmit with \r\n terminator.
    bool do_transmit(const char* data, size_t len) override {
        size_t offset = 0;
        size_t rem    = len;
        while (rem > 0) {
            const size_t chunk = std::min(rem, size_t(policy.segment_max_len));
            if (!hal_.transmit(reinterpret_cast<const uint8_t*>(data + offset), chunk))
                return false;
            offset += chunk;
            rem    -= chunk;
            if (rem > 0 && policy.segment_delay_ms > 0)
                hal_.delay(policy.segment_delay_ms);
        }
        const uint8_t crlf[2] = {'\r', '\n'};
        return hal_.transmit(crlf, 2);
    }

#ifndef NOTE_NO_STD_STRING
    // Receive until \n. Stops precisely at \n — does not over-read into
    // subsequent data (important when binary follows JSON on serial).
    Result<void> do_receive(std::string& buf, uint32_t timeout_ms) override {
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
                // Scan for \n — append up to it (not past it).
                // Bytes after \n stay in the UART for subsequent reads.
                // The \n itself is consumed but not appended.
                for (size_t i = 0; i < n; ++i) {
                    if (chunk[i] == '\n') {
                        buf.append(reinterpret_cast<const char*>(chunk), i);
                        // Push back any bytes after \n so they're not lost.
                        // hal_.receive already consumed them — we must
                        // stash them for the next do_read() call.
                        size_t leftover = n - (i + 1);
                        if (leftover > 0) {
                            overflow_len_ = leftover;
                            memcpy(overflow_, chunk + i + 1, leftover);
                        }
                        while (!buf.empty() && buf.back() == '\r')
                            buf.pop_back();
                        return {};
                    }
                }
                buf.append(reinterpret_cast<const char*>(chunk), n);
            } else {
                hal_.delay(1);
                const uint32_t now = hal_.millis();
                if (!first_byte_seen) {
                    if (timeout_ms && (now - start >= timeout_ms))
                        return make_error(Error::ResponseLost, Cause::Timeout, "no response");
                } else {
                    if (now - intra_start >= policy.intra_timeout_ms)
                        return make_error(Error::ResponseLost, Cause::TimeoutIntra, "response incomplete");
                }
            }
        }
    }

#endif // NOTE_NO_STD_STRING

    // Raw binary write: plain HAL write, no \r\n terminator.
    bool do_write(const uint8_t* data, size_t len) override {
        return hal_.transmit(data, len);
    }

    // Read available bytes from serial (up to max_len).
    // Drains any overflow from do_receive() first, then reads from HAL.
    Result<size_t> do_read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
        // Drain overflow from do_receive() greedy read
        if (overflow_len_ > 0) {
            size_t n = (overflow_len_ < max_len) ? overflow_len_ : max_len;
            memcpy(buf, overflow_ + overflow_pos_, n);
            overflow_pos_ += n;
            overflow_len_ -= n;
            return n;
        }
        const uint32_t start = hal_.millis();
        while (true) {
            size_t n = hal_.receive(buf, max_len);
            if (n > 0) return n;
            hal_.delay(1);
            if (timeout_ms && (hal_.millis() - start >= timeout_ms))
                return make_error(Error::ResponseLost, Cause::Timeout, "no response");
        }
    }

    // Reset — matches note-c _serialNoteReset().
    // Send \n and drain until only \r/\n received for policy.reset_drain_ms.
    bool do_reset() override {
        hal_.delay(policy.segment_delay_ms);  // 250 ms pre-delay

        for (uint32_t retry = 0; retry < policy.reset_sync_retries; ++retry) {
            const uint8_t nl = '\n';
            hal_.transmit(&nl, 1);

            uint8_t  buf[32];
            bool     found_something   = false;
            bool     found_non_control = false;
            uint32_t drain_start       = hal_.millis();

            while (hal_.millis() - drain_start < policy.reset_drain_ms) {
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

            hal_.delay(policy.reset_drain_ms);
        }
        return false;
    }

    // Policy access for AbstractTransport.
    uint32_t max_retries() const override { return policy.max_retries; }
    uint32_t retry_delay_ms() const override { return policy.retry_delay_ms; }
    void delay(uint32_t ms) override { hal_.delay(ms); }

private:
    SerialHal& hal_;

    // Overflow buffer: bytes read past \n by do_receive() are stashed here
    // so do_read() can return them before polling the HAL.
    uint8_t overflow_[64]{};
    size_t overflow_len_ = 0;
    size_t overflow_pos_ = 0;
};

// Deduction guides — allow construction without explicit template arguments.
#if __cplusplus >= 202002L
//   NotecardSerial transport(hal)  → StaticSerialPolicy (zero overhead)
NotecardSerial(SerialHal&) -> NotecardSerial<StaticSerialPolicy<SerialPolicy{}>>;
template <SerialPolicy P>
NotecardSerial(SerialHal&, StaticSerialPolicy<P>) -> NotecardSerial<StaticSerialPolicy<P>>;
#else
//   NotecardSerial transport(hal)  → SerialPolicy (runtime, 28 bytes)
NotecardSerial(SerialHal&) -> NotecardSerial<SerialPolicy>;
#endif
NotecardSerial(SerialHal&, SerialPolicy) -> NotecardSerial<SerialPolicy>;

// ---------------------------------------------------------------------------
// Backward-compatible constants (derived from default policy values).
// ---------------------------------------------------------------------------

inline constexpr uint32_t kSerialSegmentMaxLen       = SerialPolicy{}.segment_max_len;
inline constexpr uint32_t kSerialSegmentDelayMs      = SerialPolicy{}.segment_delay_ms;
inline constexpr uint32_t kIntraTransactionTimeoutMs = SerialPolicy{}.intra_timeout_ms;
inline constexpr uint32_t kResetDrainMs              = SerialPolicy{}.reset_drain_ms;
inline constexpr uint32_t kResetSyncRetries          = SerialPolicy{}.reset_sync_retries;
inline constexpr uint32_t kMaxRetries                = SerialPolicy{}.max_retries;
inline constexpr uint32_t kRetryDelayMs              = SerialPolicy{}.retry_delay_ms;

}  // namespace note::transport
