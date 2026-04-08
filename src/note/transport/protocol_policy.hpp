#pragma once

#include <note/note_config.hpp>
#include <cstdint>

// note::transport protocol policy types.
//
// A policy governs how the Notecard request/response protocol behaves —
// retry counts, segment pacing, drain windows, and timeouts. It is separate
// from the hardware channel (HAL), which handles physical I/O.
//
// Two flavours are provided for each transport:
//
//   Runtime policy (e.g. SerialPolicy, I2cPolicy)
//     Stored in the transport object (instance fields). Mutable after
//     construction — callers may adjust fields between requests to change
//     behaviour on a per-request basis. Costs sizeof(SerialPolicy) bytes
//     (28 bytes) in the transport object.
//
//   Compile-time policy (e.g. StaticSerialPolicy<Policy>, StaticI2cPolicy<Policy>)
//     An empty struct whose fields are static constexpr. [[no_unique_address]]
//     gives it zero bytes in the transport object. The compiler folds all
//     policy accesses to compile-time constants, eliminating dead branches
//     (e.g. if segment_delay_ms == 0, the hal_.delay() call and its guard
//     are compiled away entirely at -O1 or higher).
//
// Usage — compile-time default (zero overhead, most common):
//
//   NotecardSerial transport(hal);   // deduces StaticSerialPolicy<SerialPolicy{}>
//
// Usage — compile-time custom policy (zero overhead):
//
//   NotecardSerial<StaticSerialPolicy<SerialPolicy::fast()>> transport(hal);
//
// Usage — runtime policy (mutable, 28 bytes overhead):
//
//   NotecardSerial<SerialPolicy> transport(hal);
//   transport.policy.max_retries = 1;   // adjust before a destructive request
//   transport.policy.max_retries = 5;   // restore

namespace note::transport {

// ---------------------------------------------------------------------------
// ProtocolPolicy — fields common to all Notecard wire transports
// ---------------------------------------------------------------------------

#if NOTE_MUTABLE_POLICY
struct ProtocolPolicy {
    uint32_t segment_max_len    = 250;   // max bytes per TX segment before a pacing delay
    uint32_t segment_delay_ms   = 250;   // inter-segment delay (ms)
    uint32_t intra_timeout_ms   = 1000;  // timeout after first response byte (ms)
    uint32_t reset_drain_ms     = 500;   // drain window per reset attempt (ms)
    uint32_t reset_sync_retries = 10;    // max reset attempts before giving up
    uint32_t max_retries        = 5;     // max transaction retries on failure
    uint32_t retry_delay_ms     = 500;   // delay between retries (ms)
};
#else
/// Constant protocol policy — all values are constexpr, zero storage.
/// With [[no_unique_address]], the policy member occupies 0 bytes.
struct ProtocolPolicy {
    static constexpr uint16_t segment_max_len    = 250;
    static constexpr uint16_t segment_delay_ms   = 250;
    static constexpr uint16_t intra_timeout_ms   = 1000;
    static constexpr uint16_t reset_drain_ms     = 500;
    static constexpr uint16_t reset_sync_retries = 10;
    static constexpr uint16_t max_retries        = 5;
    static constexpr uint16_t retry_delay_ms     = 500;
};
#endif

// ---------------------------------------------------------------------------
// SerialPolicy — protocol policy for serial (UART) transport
//
// Extends ProtocolPolicy with no additional fields: the serial protocol is
// fully described by the common layer. Named separately for clarity and to
// allow future serial-specific fields without breaking I2C users.
// ---------------------------------------------------------------------------

struct SerialPolicy : ProtocolPolicy {
#if NOTE_MUTABLE_POLICY
    static constexpr SerialPolicy fast() {
        SerialPolicy p;
        p.segment_delay_ms   = 0;
        p.intra_timeout_ms   = 500;
        p.reset_drain_ms     = 250;
        p.reset_sync_retries = 5;
        p.max_retries        = 3;
        p.retry_delay_ms     = 100;
        return p;
    }
#endif
};

// ---------------------------------------------------------------------------
// I2cPolicy — protocol policy for I2C transport
//
// Extends ProtocolPolicy with I2C-specific timing fields.
// ---------------------------------------------------------------------------

struct I2cPolicy : ProtocolPolicy {
#if NOTE_MUTABLE_POLICY
    uint32_t io_delay_ms      = 6;
    uint32_t chunk_delay_ms   = 20;
    uint32_t nack_wait_ms     = 1000;
    uint32_t response_poll_ms = 50;

    static constexpr I2cPolicy fast() {
        I2cPolicy p;
        p.segment_delay_ms   = 0;
        p.io_delay_ms        = 0;
        p.chunk_delay_ms     = 5;
        p.intra_timeout_ms   = 500;
        p.reset_drain_ms     = 250;
        p.reset_sync_retries = 5;
        p.max_retries        = 3;
        p.retry_delay_ms     = 100;
        return p;
    }
#else
    static constexpr uint16_t io_delay_ms      = 6;
    static constexpr uint16_t chunk_delay_ms   = 20;
    static constexpr uint16_t nack_wait_ms     = 1000;
    static constexpr uint16_t response_poll_ms = 50;
#endif
};

// ---------------------------------------------------------------------------
// StaticSerialPolicy / StaticI2cPolicy — zero-overhead compile-time policies
//
// C++20 only: uses non-type template parameters (NTTPs) of class type.
// On C++17, the default policy is the runtime SerialPolicy/I2cPolicy struct
// (28 bytes overhead — negligible on ESP32 and similar platforms).
// ---------------------------------------------------------------------------

#if __cplusplus >= 202002L

template <SerialPolicy Policy>
struct StaticSerialPolicy {
    static constexpr uint32_t segment_max_len    = Policy.segment_max_len;
    static constexpr uint32_t segment_delay_ms   = Policy.segment_delay_ms;
    static constexpr uint32_t intra_timeout_ms   = Policy.intra_timeout_ms;
    static constexpr uint32_t reset_drain_ms     = Policy.reset_drain_ms;
    static constexpr uint32_t reset_sync_retries = Policy.reset_sync_retries;
    static constexpr uint32_t max_retries        = Policy.max_retries;
    static constexpr uint32_t retry_delay_ms     = Policy.retry_delay_ms;
};

template <I2cPolicy Policy>
struct StaticI2cPolicy {
    static constexpr uint32_t segment_max_len    = Policy.segment_max_len;
    static constexpr uint32_t segment_delay_ms   = Policy.segment_delay_ms;
    static constexpr uint32_t intra_timeout_ms   = Policy.intra_timeout_ms;
    static constexpr uint32_t reset_drain_ms     = Policy.reset_drain_ms;
    static constexpr uint32_t reset_sync_retries = Policy.reset_sync_retries;
    static constexpr uint32_t max_retries        = Policy.max_retries;
    static constexpr uint32_t retry_delay_ms     = Policy.retry_delay_ms;
    static constexpr uint32_t io_delay_ms        = Policy.io_delay_ms;
    static constexpr uint32_t chunk_delay_ms     = Policy.chunk_delay_ms;
    static constexpr uint32_t nack_wait_ms       = Policy.nack_wait_ms;
    static constexpr uint32_t response_poll_ms   = Policy.response_poll_ms;
};

#endif // C++20

}  // namespace note::transport
