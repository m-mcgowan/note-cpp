#pragma once

#include <note/transport.hpp>
#include <note/transport/protocol_policy.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#ifndef NOTE_NO_STD_STRING
#include <string>
#endif

// note::transport::NotecardI2c
//
// Implements the Notecard I2C wire protocol. Extends AbstractTransport
// with I2C-specific byte I/O (chunked TX/RX, priming query, \n framing).
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
// I2CHal — pure virtual platform interface
// ---------------------------------------------------------------------------

class I2CHal {
public:
    virtual ~I2CHal() = default;

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
// I2cCallbackHal — wraps lambdas/function pointers as an I2CHal
// ---------------------------------------------------------------------------

class I2cCallbackHal : public I2CHal {
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

#if __cplusplus >= 202002L
template <typename PolicyType = StaticI2cPolicy<I2cPolicy{}>>
#else
template <typename PolicyType = I2cPolicy>
#endif
class NotecardI2c : public note::AbstractTransport {
public:
    // policy is public so callers can read or mutate it between requests.
    // For StaticI2cPolicy (the default), [[no_unique_address]] gives it
    // zero bytes — all fields are static constexpr, folded by the compiler.
    [[no_unique_address]] PolicyType policy;

    explicit NotecardI2c(I2CHal& hal, PolicyType pol = {})
        : policy(pol), hal_(hal) {}

protected:
    // ── AbstractTransport building blocks ──────────────────────────────────

    // I2C appends bare \n to the wire buffer (not \r\n like serial).
    void prepare_wire(string_view request) override {
        AbstractTransport::prepare_wire(request);
        wire_data()[wire_len_++] = '\n';  // I2C appends bare \n (not \r\n like serial)
    }

    // Chunked transmit with IO delay before each chunk.
    bool do_transmit(const char* data, size_t len) override {
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

            if (sentInSegment > policy.segment_max_len) {
                sentInSegment = 0;
                hal_.delay(policy.segment_delay_ms);
            }
            if (policy.chunk_delay_ms > 0) hal_.delay(policy.chunk_delay_ms);
        }

        return true;
    }

    // Raw binary write: chunked MTU writes, no segment pacing.
    // Matches note-c _i2cChunkedTransmit with delay=false.
    bool do_write(const uint8_t* data, size_t len) override {
        size_t offset = 0;
        while (offset < len) {
            const size_t chunk = std::min(len - offset, hal_.max_transfer());
            if (!hal_.transmit(data + offset, chunk)) return false;
            offset += chunk;
        }
        return true;
    }

#ifndef NOTE_NO_STD_STRING
    // Priming-query receive: poll until data available, then chunked read.
    Result<void> do_receive(std::string& buf, uint32_t timeout_ms) override {
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
                    hal_.delay(policy.response_poll_ms);
                }
            }
        }

        // Chunked receive loop.
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
                intra_start = hal_.millis();
                eop = eop || (buf.back() == '\n');
            }

            if (available > 0) continue;
            if (eop) break;

            if (hal_.millis() - intra_start >= policy.intra_timeout_ms)
                return make_error(Error::ResponseLost, Cause::TimeoutIntra, "response incomplete");
            hal_.delay(policy.response_poll_ms);
        }

        // Strip trailing \r\n.
        while (!buf.empty() &&
               (buf.back() == '\n' || buf.back() == '\r'))
            buf.pop_back();

        return {};
    }

#endif // NOTE_NO_STD_STRING

    // Read available bytes from the I2C bus (up to max_len).
    // Uses priming query + chunked read. Returns bytes read.
    Result<size_t> do_read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
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
                    hal_.delay(policy.response_poll_ms);
                }
            }
        }

        size_t want = std::min(size_t(available), std::min(max_len, hal_.max_transfer()));
        if (!hal_.receive(buf, want, available))
            return make_error(Error::ResponseLost, Cause::HalError, "I2C receive failed");
        return want;
    }

    // Reset — matches note-c _i2cNoteReset().
    bool do_reset() override {
        hal_.delay(policy.segment_delay_ms);
        if (!hal_.reset()) return false;
        delay_io();

        for (uint32_t retry = 0; retry < policy.reset_sync_retries; ++retry) {
            const uint8_t nl = '\n';
            if (!hal_.transmit(&nl, 1)) {
                hal_.delay(policy.nack_wait_ms);
                continue;
            }

            hal_.delay(policy.segment_delay_ms);

            uint32_t drain_start       = hal_.millis();
            size_t   chunk_len         = 0;
            bool     found_something   = false;
            bool     found_non_control = false;
            uint8_t  buf[64];

            while (hal_.millis() - drain_start < policy.reset_drain_ms) {
                uint32_t avail = 0;
                const size_t req = std::min({chunk_len, sizeof(buf),
                                             hal_.max_transfer()});

                if (!hal_.receive(buf, req, avail)) {
                    hal_.delay(policy.segment_delay_ms);
                    continue;
                }

                if (req > 0) {
                    found_something = true;
                    for (size_t i = 0; i < req; ++i) {
                        if (buf[i] != '\n' && buf[i] != '\r') {
                            found_non_control = true;
                            drain_start = hal_.millis();
                        }
                    }
                }

                chunk_len = std::min({size_t(avail), sizeof(buf),
                                      hal_.max_transfer()});
                hal_.delay(policy.chunk_delay_ms);
            }

            if (found_something && !found_non_control) return true;

            if (!found_something) {
                if (!hal_.reset()) return false;
                delay_io();
            }
        }
        return false;
    }

    // I2C line terminator: bare \n (not \r\n like serial).
    bool write_line_terminator() override {
        const uint8_t nl = '\n';
        return do_write(&nl, 1);
    }

    // Policy access for AbstractTransport.
    uint32_t max_retries() const override { return policy.max_retries; }
    uint32_t retry_delay_ms() const override { return policy.retry_delay_ms; }
    void delay(uint32_t ms) override { hal_.delay(ms); }

private:
    I2CHal& hal_;

    void delay_io() {
        if (policy.io_delay_ms > 0) hal_.delay(policy.io_delay_ms);
    }
};

// Deduction guides — allow construction without explicit template arguments.
#if __cplusplus >= 202002L
NotecardI2c(I2CHal&) -> NotecardI2c<StaticI2cPolicy<I2cPolicy{}>>;
template <I2cPolicy P>
NotecardI2c(I2CHal&, StaticI2cPolicy<P>) -> NotecardI2c<StaticI2cPolicy<P>>;
#else
NotecardI2c(I2CHal&) -> NotecardI2c<I2cPolicy>;
#endif
NotecardI2c(I2CHal&, I2cPolicy) -> NotecardI2c<I2cPolicy>;

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
