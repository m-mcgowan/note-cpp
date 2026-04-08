#pragma once

#include <note/note_config.hpp>
#include <note/transport_hal.hpp>
#include <note/transport/protocol_policy.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

// NOTE_STATIC_HAL strips virtual inheritance from HAL types.
// Concrete types provide the same methods without vtable overhead.
#if NOTE_STATIC_HAL
#define NOTE_HAL_OVERRIDE
#else
#define NOTE_HAL_OVERRIDE override
#endif

// note::transport::NotecardSerial
//
// Implements the Notecard serial wire protocol as a TransportHal.
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
    virtual bool     transmit(const uint8_t* data, size_t len) = 0;
    virtual size_t   receive(uint8_t* buf, size_t max_len) = 0;
    virtual uint32_t millis() = 0;
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

/// NotecardSerial — TransportHal implementation for serial (UART).
/// Wraps a platform SerialHal (non-blocking receive) into the blocking
/// TransportHal interface used by StreamingTransport.
#if NOTE_STATIC_HAL
template <typename HalT, typename PolicyType = SerialPolicy>
class NotecardSerial {
#elif __cplusplus >= 202002L
template <typename PolicyType = StaticSerialPolicy<SerialPolicy{}>>
class NotecardSerial : public note::TransportHal {
#else
template <typename PolicyType = SerialPolicy>
class NotecardSerial : public note::TransportHal {
#endif
public:
    [[no_unique_address]] PolicyType policy;

#if NOTE_STATIC_HAL
    explicit NotecardSerial(HalT& hal, PolicyType pol = {})
        : policy(pol), hal_(hal) {}
#else
    explicit NotecardSerial(SerialHal& hal, PolicyType pol = {})
        : policy(pol), hal_(hal) {}
#endif

    bool transmit(const uint8_t* data, size_t len) NOTE_HAL_OVERRIDE {
        size_t offset = 0;
        while (offset < len) {
            size_t chunk = std::min(len - offset, size_t(policy.segment_max_len));
            if (!hal_.transmit(data + offset, chunk)) return false;
            offset += chunk;
            if (offset < len && policy.segment_delay_ms > 0)
                hal_.delay(policy.segment_delay_ms);
        }
        return true;
    }

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) NOTE_HAL_OVERRIDE {
        const uint32_t start = hal_.millis();
        while (true) {
            size_t n = hal_.receive(buf, max_len);
            if (n > 0) return n;
            hal_.delay(1);
            if (timeout_ms && (hal_.millis() - start >= timeout_ms))
                return make_error(Error::ResponseLost, Cause::Timeout, "no response");
        }
    }

    bool reset() NOTE_HAL_OVERRIDE {
        hal_.delay(policy.segment_delay_ms);

        for (uint32_t retry = 0; retry < policy.reset_sync_retries; ++retry) {
            const uint8_t nl = '\n';
            hal_.transmit(&nl, 1);

            uint8_t  drain_buf[32];
            bool     found_something   = false;
            bool     found_non_control = false;
            uint32_t drain_start       = hal_.millis();

            while (hal_.millis() - drain_start < policy.reset_drain_ms) {
                size_t n = hal_.receive(drain_buf, sizeof(drain_buf));
                for (size_t i = 0; i < n; ++i) {
                    found_something = true;
                    if (drain_buf[i] != '\n' && drain_buf[i] != '\r') {
                        found_non_control = true;
                        drain_start = hal_.millis();
                    }
                }
                if (n == 0) hal_.delay(1);
            }

            if (found_something && !found_non_control) return true;
            hal_.delay(policy.reset_drain_ms);
        }
        return false;
    }

    bool write_line_terminator() NOTE_HAL_OVERRIDE {
        const uint8_t crlf[] = {'\r', '\n'};
        return hal_.transmit(crlf, 2);
    }

    uint32_t millis() NOTE_HAL_OVERRIDE { return hal_.millis(); }
    void delay(uint32_t ms) NOTE_HAL_OVERRIDE { hal_.delay(ms); }

private:
#if NOTE_STATIC_HAL
    HalT& hal_;
#else
    SerialHal& hal_;
#endif
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
