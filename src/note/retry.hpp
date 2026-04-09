#pragma once

/// @file retry.hpp
/// Inter-transaction timing and safety-gated retry for Notecard transactions.
///
/// Two APIs:
/// - retry_loop(): non-template, type-erased — used by StaticNotecard to avoid
///   per-endpoint monomorphization on constrained targets.
/// - retry_transaction(): template — used by Notecard (polymorphic, non-AVR)
///   where code size is less critical.

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
// Type-erased transport operations for retry_loop
// ---------------------------------------------------------------------------

/// Minimal transport operations needed by retry_loop.
/// Avoids templating the retry loop on the transport type.
struct RetryTransportOps {
    void* ctx;
    uint32_t (*millis_fn)(void* ctx);
    void (*delay_fn)(void* ctx, uint32_t ms);
    void (*reset_fn)(void* ctx);
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
// retry_loop — non-template retry engine (for StaticNotecard / AVR)
// ---------------------------------------------------------------------------

/// Type-erased attempt function. Called by retry_loop.
/// Returns true on success. On failure, writes the Error code to *out_error.
using RetryAttemptFn = bool (*)(void* ctx, Error* out_error);

/// Execute a transport call with inter-transaction timing and safety-gated retry.
/// Non-template — one copy in the binary regardless of endpoint count.
///
/// The caller provides the first attempt result (success + error code).
/// retry_loop only runs the retry iterations (not the first attempt), so the
/// caller's typed result lives in the caller's stack frame.
inline bool retry_loop(
    bool first_ok,
    Error first_error,
    RetryAttemptFn attempt,
    void* attempt_ctx,
    RetryTransportOps& ops,
    TransactionTiming& timing,
    Safety safety,
    const RetryPolicy& policy)
{
    // Record timing on exit.
    auto record = [&]() {
        timing.last_transaction_end_ms = ops.millis_fn(ops.ctx);
        timing.has_previous = true;
    };

    uint32_t start_ms = ops.millis_fn(ops.ctx);
    bool ok = first_ok;
    Error last_error = first_error;

    for (uint32_t i = 0; i < policy.max_retries; ++i) {
        if (ok) { record(); return true; }
        if (!detail::should_retry(last_error, safety)) { record(); return false; }

        if (policy.timeout_ms > 0) {
            uint32_t elapsed = ops.millis_fn(ops.ctx) - start_ms;
            if (elapsed >= policy.timeout_ms) break;
        }

        ops.delay_fn(ops.ctx, policy.retry_delay_ms);
        ops.reset_fn(ops.ctx);
        ok = attempt(attempt_ctx, &last_error);
    }

    record();
    return ok;
}

// ---------------------------------------------------------------------------
// retry_transaction — template wrapper (for Notecard / non-constrained)
// ---------------------------------------------------------------------------

/// Execute a transport call with inter-transaction timing and safety-gated retry.
///
/// @tparam ResultT    The return type (e.g. ApiResult<Rsp>, Result<string_view>).
/// @tparam Transport  Anything with millis(), delay(uint32_t), reset().
/// @tparam AttemptFn  Callable returning ResultT.
/// @tparam ResetFn    Callable with no args — resets the transport.
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
