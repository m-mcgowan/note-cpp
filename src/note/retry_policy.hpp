#pragma once

/// @file retry_policy.hpp
/// RetryPolicy — configurable retry parameters for Notecard transactions.

#include <cstdint>

namespace note {

/// Controls how the Notecard retries failed transactions.
///
/// The retry loop in the Notecard object uses these parameters to decide
/// how many times to retry, how long to wait between attempts, and the
/// total time budget for all attempts.
struct RetryPolicy {
    /// Maximum number of retry attempts after the first failure.
    /// 0 = no retries (single attempt only).
    uint8_t max_retries = 5;

    /// Delay in milliseconds between retry attempts.
    /// Applied after a failed attempt, before the transport is reset.
    uint16_t retry_delay_ms = 500;

    /// Total wall-clock budget for all attempts, in milliseconds.
    /// 0 = no limit. Checked between attempts — if the budget is
    /// exceeded, no further retries are attempted.
    uint32_t timeout_ms = 30000;
};

} // namespace note
