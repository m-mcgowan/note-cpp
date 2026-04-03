#pragma once

/// @file retry.hpp
/// Inter-transaction timing and safety-gated retry for Notecard transactions.
///
/// The retry_transaction() template is called by Notecard, StaticNotecard,
/// and BareNotecard to wrap every transport call with:
///   1. Inter-transaction gap enforcement (wall-clock based)
///   2. Single transport attempt
///   3. Safety-gated retry decision
///   4. Transport reset between retries
///   5. Timeout budget tracking

#include <note/error.hpp>
#include <note/retry_policy.hpp>
#include <note/safety.hpp>

#include <cstdint>

namespace note {

// ---------------------------------------------------------------------------
// TransactionTiming — tracks inter-transaction gap
// ---------------------------------------------------------------------------

/// Per-Notecard state for enforcing a minimum gap between transactions.
/// Wall-clock based: only waits the remaining delta if the next transaction
/// arrives before min_gap_ms has elapsed since the previous one ended.
struct TransactionTiming {
    uint32_t last_transaction_end_ms = 0;
    uint32_t min_gap_ms = 2;  ///< Default minimum gap (ms). Configurable.
    bool has_previous = false;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace detail {

/// Returns true if the error is retryable given the request's Safety.
///
/// - SendFailed: always retryable (request never reached the Notecard).
/// - ResponseLost: retryable only for ReadOnly/Idempotent requests.
/// - All other errors (Notecard, Json, NotReady, etc.): never retryable.
inline bool should_retry(Error error, Safety safety) {
    if (error == Error::SendFailed) return true;
    if (error == Error::ResponseLost) return is_safe_to_retry(safety);
    return false;
}

} // namespace detail

// ---------------------------------------------------------------------------
// retry_transaction — the unified retry wrapper
// ---------------------------------------------------------------------------

/// Execute a transport call with inter-transaction timing and safety-gated retry.
///
/// @tparam ResultT    The return type (e.g. ApiResult<Rsp>, Result<string_view>).
///                    Must support: `if (result)` for success test, and
///                    `result.error().code` for Error extraction on failure.
/// @tparam Transport  Anything with millis(), delay(uint32_t), reset().
/// @tparam AttemptFn  Callable returning ResultT — performs one transport attempt.
/// @tparam ResetFn    Callable with no args — resets the transport.
///
/// The wrapper:
///   1. Enforces inter-transaction gap (waits remaining delta)
///   2. Attempts the transport call
///   3. On success or non-retryable error: records end time, returns
///   4. On retryable error: checks timeout budget, delays, resets, retries
///
template<typename ResultT, typename Transport, typename AttemptFn, typename ResetFn>
ResultT retry_transaction(
    Transport& transport,
    TransactionTiming& timing,
    Safety safety,
    const RetryPolicy& policy,
    AttemptFn&& attempt,
    ResetFn&& reset_transport)
{
    // 1. Enforce inter-transaction gap
    if (timing.has_previous) {
        uint32_t now = transport.millis();
        uint32_t elapsed = now - timing.last_transaction_end_ms;
        if (elapsed < timing.min_gap_ms) {
            transport.delay(timing.min_gap_ms - elapsed);
        }
    }

    uint32_t start_ms = transport.millis();

    // First attempt (always runs).
    auto last_result = attempt();
    auto record_and_return = [&](auto&& r) -> ResultT {
        timing.last_transaction_end_ms = transport.millis();
        timing.has_previous = true;
        return std::forward<decltype(r)>(r);
    };

    for (uint32_t i = 0; i < policy.max_retries; ++i) {
        // Success — done
        if (last_result) return record_and_return(std::move(last_result));

        // Non-retryable error — done
        if (!detail::should_retry(last_result.error().code, safety))
            return record_and_return(std::move(last_result));

        // Check timeout budget before retrying
        if (policy.timeout_ms > 0) {
            uint32_t elapsed = transport.millis() - start_ms;
            if (elapsed >= policy.timeout_ms) break;
        }

        // Delay + reset + retry
        transport.delay(policy.retry_delay_ms);
        reset_transport();
        last_result = attempt();
    }

    return record_and_return(std::move(last_result));
}

} // namespace note
