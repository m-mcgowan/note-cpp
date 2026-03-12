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

// note::transport::NotecardSerial
//
// Implements the Notecard serial wire protocol in C++, providing a complete
// alternative to the note-c serial transport. Faithful port of note-c
// n_serial.c + n_request.c (retry, CRC) with a platform-injectable HAL.
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

template <typename PolicyType = StaticSerialPolicy<SerialPolicy{}>>
class NotecardSerial {
public:
    // policy is public so callers can read or mutate it between requests.
    // For StaticSerialPolicy (the default), [[no_unique_address]] gives it
    // zero bytes — all fields are static constexpr, folded by the compiler.
    [[no_unique_address]] PolicyType policy;

    explicit NotecardSerial(SerialHal& hal, PolicyType pol = {})
        : policy(pol), hal_(hal) {}

    // Satisfies note::Notecard's RequestFn signature:
    //   std::function<Result<std::string>(string_view, uint32_t)>
    //
    // Auto-resets on first call. Handles CRC (auto-detected), segmented TX,
    // greedy RX, and up to policy.max_retries retries on failure.
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

        ErrorInfo last_error{Error::SendFailed, Cause::HalError, "transmit failed"};

        for (uint32_t attempt = 0; attempt <= policy.max_retries; ++attempt) {
            if (attempt > 0) hal_.delay(policy.retry_delay_ms);

            // Segmented transmit.
            if (!send_segmented(wire.data(), wire.size())) {
                last_error = {Error::SendFailed, Cause::HalError, "transmit failed"};
                do_reset();
                continue;
            }

            // Receive until newline.
            auto result = receive_line(timeout_ms);
            if (!result) {
                last_error = result.error();
                continue;
            }

            // CRC validation + auto-detection (mutates *result, sets crc_enabled_).
            if (detail::crc_check_and_strip(*result, crc_seq_, crc_enabled_)) {
                last_error = {Error::ResponseLost, Cause::CrcMismatch, "CRC mismatch"};
                continue;
            }

            return result;
        }

        return Unexpected(last_error);
    }

private:
    SerialHal& hal_;
    bool       initialized_ = false;
    bool       crc_enabled_ = false;
    uint16_t   crc_seq_     = 0;

    // -----------------------------------------------------------------------
    // reset — matches note-c _serialNoteReset()
    // Send \n and drain until only \r/\n received for policy.reset_drain_ms.
    // A clean window (only control characters) means the Notecard is ready.
    // -----------------------------------------------------------------------
    bool do_reset() {
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
                        drain_start = hal_.millis();  // extend window on non-control char
                    }
                }
                if (n == 0) hal_.delay(1);
            }

            if (found_something && !found_non_control) return true;

            hal_.delay(policy.reset_drain_ms);
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // send_segmented — matches note-c _serialChunkedTransmit()
    // Transmit in policy.segment_max_len chunks with an inter-segment delay.
    // Appends \r\n terminator (matches note-c c_newline).
    // -----------------------------------------------------------------------
    bool send_segmented(const char* data, size_t len) {
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

    // -----------------------------------------------------------------------
    // receive_line — matches note-c _serialChunkedReceive()
    // Poll until \n received. Initial timeout = timeout_ms; after first byte
    // switches to policy.intra_timeout_ms (1 s intra-transaction timeout).
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
                        return make_error(Error::ResponseLost, Cause::Timeout, "no response");
                } else {
                    if (now - intra_start >= policy.intra_timeout_ms)
                        return make_error(Error::ResponseLost, Cause::TimeoutIntra, "response incomplete");
                }
            }
        }
    }
};

// Deduction guides — allow construction without explicit template arguments.
//
//   NotecardSerial transport(hal)          → StaticSerialPolicy<SerialPolicy{}>
//                                            (zero overhead, compile-time defaults)
//   NotecardSerial transport(hal, policy)  → SerialPolicy
//                                            (runtime mutable, 28 bytes)
NotecardSerial(SerialHal&) -> NotecardSerial<StaticSerialPolicy<SerialPolicy{}>>;
NotecardSerial(SerialHal&, SerialPolicy) -> NotecardSerial<SerialPolicy>;

template <SerialPolicy P>
NotecardSerial(SerialHal&, StaticSerialPolicy<P>) -> NotecardSerial<StaticSerialPolicy<P>>;

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
