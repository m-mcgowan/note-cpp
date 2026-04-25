/// @file test_retry.cpp
/// Unit tests for retry_transaction() — safety-gated retry and inter-transaction timing.

#include <doctest.h>
#include <string>

#include <note/retry.hpp>
#include <note/error.hpp>
#include <note/safety.hpp>

#include <cstdint>

using namespace note;

// ---------------------------------------------------------------------------
// Mock transport — controllable clock, tracks reset calls
// ---------------------------------------------------------------------------

struct MockClock {
    uint32_t now_ms = 0;
    int reset_count = 0;
    int delay_total_ms = 0;

    uint32_t millis() { return now_ms; }
    void delay(uint32_t ms) { now_ms += ms; delay_total_ms += ms; }
    void reset() { ++reset_count; }
};

// ---------------------------------------------------------------------------
// Helpers to create results
// ---------------------------------------------------------------------------

struct TestResult {
    bool ok;
    ErrorInfo err;

    TestResult() : ok(false), err{} {}
    explicit TestResult(bool success) : ok(success), err{} {}
    TestResult(Error code, Cause cause = Cause::Unspecified)
        : ok(false), err{code, cause, "test error"} {}
    TestResult(ErrorInfo e) : ok(false), err(e) {}

    explicit operator bool() const { return ok; }
    const ErrorInfo& error() const { return err; }
};

static TestResult success() { return TestResult(true); }
static TestResult send_failed() { return TestResult(Error::SendFailed, Cause::HalError); }
static TestResult response_lost() { return TestResult(Error::ResponseLost, Cause::Timeout); }
static TestResult notecard_error() { return TestResult(Error::Notecard); }
static TestResult json_error() { return TestResult(Error::Json); }

// ---------------------------------------------------------------------------
// Safety × Error matrix
// ---------------------------------------------------------------------------

TEST_CASE("retry: SendFailed always retries regardless of Safety") {
    for (auto safety : {Safety::ReadOnly, Safety::Idempotent,
                        Safety::NonIdempotent, Safety::Destructive}) {
        MockClock clock;
        TransactionTiming timing;
        RetryPolicy policy{.max_retries = 2};
        int attempts = 0;

        auto result = retry_transaction<TestResult>(
            clock, timing, safety, policy,
            [&]() -> TestResult {
                ++attempts;
                if (attempts < 3) return send_failed();
                return success();
            },
            [&]() { clock.reset(); });

        CAPTURE(safety);
        CHECK(result);
        CHECK(attempts == 3);
    }
}

TEST_CASE("retry: ResponseLost retries for ReadOnly and Idempotent") {
    for (auto safety : {Safety::ReadOnly, Safety::Idempotent}) {
        MockClock clock;
        TransactionTiming timing;
        RetryPolicy policy{.max_retries = 2};
        int attempts = 0;

        auto result = retry_transaction<TestResult>(
            clock, timing, safety, policy,
            [&]() -> TestResult {
                ++attempts;
                if (attempts < 3) return response_lost();
                return success();
            },
            [&]() { clock.reset(); });

        CAPTURE(safety);
        CHECK(result);
        CHECK(attempts == 3);
    }
}

TEST_CASE("retry: ResponseLost does NOT retry for NonIdempotent") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5};
    int attempts = 0;

    auto result = retry_transaction<TestResult>(
        clock, timing, Safety::NonIdempotent, policy,
        [&]() -> TestResult { ++attempts; return response_lost(); },
        [&]() { clock.reset(); });

    CHECK(!result);
    CHECK(attempts == 1);  // no retry
    CHECK(result.error().code == Error::ResponseLost);
}

TEST_CASE("retry: ResponseLost does NOT retry for Destructive") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5};
    int attempts = 0;

    auto result = retry_transaction<TestResult>(
        clock, timing, Safety::Destructive, policy,
        [&]() -> TestResult { ++attempts; return response_lost(); },
        [&]() { clock.reset(); });

    CHECK(!result);
    CHECK(attempts == 1);
    CHECK(result.error().code == Error::ResponseLost);
}

TEST_CASE("retry: Notecard error never retries") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5};
    int attempts = 0;

    auto result = retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { ++attempts; return notecard_error(); },
        [&]() { clock.reset(); });

    CHECK(!result);
    CHECK(attempts == 1);
}

TEST_CASE("retry: Json error never retries") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5};
    int attempts = 0;

    auto result = retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { ++attempts; return json_error(); },
        [&]() { clock.reset(); });

    CHECK(!result);
    CHECK(attempts == 1);
}

// ---------------------------------------------------------------------------
// Retry count and reset
// ---------------------------------------------------------------------------

TEST_CASE("retry: respects max_retries") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 3};
    int attempts = 0;

    auto result = retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { ++attempts; return send_failed(); },
        [&]() { clock.reset(); });

    CHECK(!result);
    CHECK(attempts == 4);  // 1 initial + 3 retries
}

TEST_CASE("retry: zero max_retries means single attempt") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 0};
    int attempts = 0;

    auto result = retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { ++attempts; return send_failed(); },
        [&]() { clock.reset(); });

    CHECK(!result);
    CHECK(attempts == 1);
}

TEST_CASE("retry: resets transport between retries") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 3};

    retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { return send_failed(); },
        [&]() { clock.reset(); });

    CHECK(clock.reset_count == 3);  // reset before each retry, not before first attempt
}

TEST_CASE("retry: delays between retries") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 2, .retry_delay_ms = 100};

    retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { return send_failed(); },
        [&]() { clock.reset(); });

    CHECK(clock.delay_total_ms == 200);  // 100ms × 2 retries
}

// ---------------------------------------------------------------------------
// Timeout budget
// ---------------------------------------------------------------------------

TEST_CASE("retry: stops when timeout budget exceeded") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 100, .retry_delay_ms = 100, .timeout_ms = 250};

    int attempts = 0;
    retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { ++attempts; return send_failed(); },
        [&]() { clock.reset(); });

    // Budget: 250ms. Budget checked before delay on each retry.
    // i=0: attempt at t=0 → fail
    // i=1: check 0<250, delay→t=100, attempt → fail
    // i=2: check 100<250, delay→t=200, attempt → fail
    // i=3: check 200<250, delay→t=300, attempt → fail
    // i=4: check 300>=250 → break
    CHECK(attempts == 4);
}

TEST_CASE("retry: timeout_ms=0 means no limit") {
    MockClock clock;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5, .retry_delay_ms = 1, .timeout_ms = 0};
    int attempts = 0;

    retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { ++attempts; return send_failed(); },
        [&]() { clock.reset(); });

    CHECK(attempts == 6);  // all 5 retries used
}

// ---------------------------------------------------------------------------
// Inter-transaction timing
// ---------------------------------------------------------------------------

TEST_CASE("retry: enforces inter-transaction gap") {
    MockClock clock;
    clock.now_ms = 1000;
    TransactionTiming timing{.last_transaction_end_ms = 999, .min_gap_ms = 50, .has_previous = true};
    RetryPolicy policy{.max_retries = 0};

    uint32_t attempt_time = 0;
    retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { attempt_time = clock.now_ms; return success(); },
        [&]() { clock.reset(); });

    // Elapsed since last: 1000 - 999 = 1ms. Gap is 50ms. Should wait 49ms.
    CHECK(attempt_time == 1049);
}

TEST_CASE("retry: no wait when gap already satisfied") {
    MockClock clock;
    clock.now_ms = 1000;
    TransactionTiming timing{.last_transaction_end_ms = 900, .min_gap_ms = 50, .has_previous = true};
    RetryPolicy policy{.max_retries = 0};

    uint32_t attempt_time = 0;
    retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { attempt_time = clock.now_ms; return success(); },
        [&]() { clock.reset(); });

    // Elapsed: 100ms > 50ms gap. No wait.
    CHECK(attempt_time == 1000);
}

TEST_CASE("retry: no wait on first transaction") {
    MockClock clock;
    clock.now_ms = 500;
    TransactionTiming timing;  // has_previous = false
    RetryPolicy policy{.max_retries = 0};

    uint32_t attempt_time = 0;
    retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { attempt_time = clock.now_ms; return success(); },
        [&]() { clock.reset(); });

    CHECK(attempt_time == 500);  // no delay
}

TEST_CASE("retry: records end time on success") {
    MockClock clock;
    clock.now_ms = 100;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 0};

    retry_transaction<TestResult>(
        clock, timing, Safety::ReadOnly, policy,
        [&]() -> TestResult { clock.now_ms = 200; return success(); },
        [&]() { clock.reset(); });

    CHECK(timing.has_previous);
    CHECK(timing.last_transaction_end_ms == 200);
}

TEST_CASE("retry: records end time on failure") {
    MockClock clock;
    clock.now_ms = 100;
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 0};

    retry_transaction<TestResult>(
        clock, timing, Safety::NonIdempotent, policy,
        [&]() -> TestResult { clock.now_ms = 200; return response_lost(); },
        [&]() { clock.reset(); });

    CHECK(timing.has_previous);
    CHECK(timing.last_transaction_end_ms == 200);
}

// ---------------------------------------------------------------------------
// should_retry unit tests
// ---------------------------------------------------------------------------

TEST_CASE("should_retry: SendFailed is always retryable") {
    CHECK(detail::should_retry(Error::SendFailed, Safety::ReadOnly));
    CHECK(detail::should_retry(Error::SendFailed, Safety::Idempotent));
    CHECK(detail::should_retry(Error::SendFailed, Safety::NonIdempotent));
    CHECK(detail::should_retry(Error::SendFailed, Safety::Destructive));
}

TEST_CASE("should_retry: ResponseLost depends on Safety") {
    CHECK(detail::should_retry(Error::ResponseLost, Safety::ReadOnly));
    CHECK(detail::should_retry(Error::ResponseLost, Safety::Idempotent));
    CHECK_FALSE(detail::should_retry(Error::ResponseLost, Safety::NonIdempotent));
    CHECK_FALSE(detail::should_retry(Error::ResponseLost, Safety::Destructive));
}

TEST_CASE("should_retry: other errors never retryable") {
    for (auto err : {Error::Notecard, Error::Json, Error::NotReady,
                     Error::Overflow, Error::InvalidArg}) {
        CHECK_FALSE(detail::should_retry(err, Safety::ReadOnly));
    }
}

// ---------------------------------------------------------------------------
// retry_loop — type-erased retry engine
// ---------------------------------------------------------------------------

struct LoopTestOps {
    uint32_t now_ms = 0;
    int reset_count = 0;
    int delay_total_ms = 0;

    static uint32_t millis(void* ctx) { return static_cast<LoopTestOps*>(ctx)->now_ms; }
    static void delay(void* ctx, uint32_t ms) {
        auto* self = static_cast<LoopTestOps*>(ctx);
        self->now_ms += ms;
        self->delay_total_ms += static_cast<int>(ms);
    }
    static void reset(void* ctx) { static_cast<LoopTestOps*>(ctx)->reset_count++; }

    RetryTransportOps ops() { return {this, millis, delay, reset}; }
};

struct AttemptTracker {
    int calls = 0;
    int succeed_after = 999;
    Error error = Error::SendFailed;

    static bool attempt(void* ctx, Error* out) {
        auto* self = static_cast<AttemptTracker*>(ctx);
        self->calls++;
        if (self->calls >= self->succeed_after) return true;
        *out = self->error;
        return false;
    }
};

TEST_CASE("retry_loop: first attempt success returns immediately") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5};
    AttemptTracker tracker;

    bool ok = retry_loop(true, Error{}, AttemptTracker::attempt, &tracker,
                         t, timing, Safety::ReadOnly, policy);
    CHECK(ok);
    CHECK(tracker.calls == 0);
}

TEST_CASE("retry_loop: retries on SendFailed and succeeds") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 3};
    AttemptTracker tracker{.succeed_after = 2};

    bool ok = retry_loop(false, Error::SendFailed, AttemptTracker::attempt, &tracker,
                         t, timing, Safety::ReadOnly, policy);
    CHECK(ok);
    CHECK(tracker.calls == 2);
}

TEST_CASE("retry_loop: respects max_retries") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 3};
    AttemptTracker tracker;

    bool ok = retry_loop(false, Error::SendFailed, AttemptTracker::attempt, &tracker,
                         t, timing, Safety::ReadOnly, policy);
    CHECK_FALSE(ok);
    CHECK(tracker.calls == 3);
}

TEST_CASE("retry_loop: non-retryable error stops immediately") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5};
    AttemptTracker tracker;

    bool ok = retry_loop(false, Error::Notecard, AttemptTracker::attempt, &tracker,
                         t, timing, Safety::ReadOnly, policy);
    CHECK_FALSE(ok);
    CHECK(tracker.calls == 0);
}

TEST_CASE("retry_loop: ResponseLost retries for ReadOnly") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 2};
    AttemptTracker tracker{.succeed_after = 1, .error = Error::ResponseLost};

    bool ok = retry_loop(false, Error::ResponseLost, AttemptTracker::attempt, &tracker,
                         t, timing, Safety::ReadOnly, policy);
    CHECK(ok);
    CHECK(tracker.calls == 1);
}

TEST_CASE("retry_loop: ResponseLost does not retry for NonIdempotent") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5};
    AttemptTracker tracker;

    bool ok = retry_loop(false, Error::ResponseLost, AttemptTracker::attempt, &tracker,
                         t, timing, Safety::NonIdempotent, policy);
    CHECK_FALSE(ok);
    CHECK(tracker.calls == 0);
}

TEST_CASE("retry_loop: delays and resets between retries") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 3, .retry_delay_ms = 100};
    AttemptTracker tracker;

    retry_loop(false, Error::SendFailed, AttemptTracker::attempt, &tracker,
               t, timing, Safety::ReadOnly, policy);
    CHECK(ops.delay_total_ms == 300);
    CHECK(ops.reset_count == 3);
}

TEST_CASE("retry_loop: timeout budget stops retries") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 100, .retry_delay_ms = 100, .timeout_ms = 250};
    AttemptTracker tracker;

    retry_loop(false, Error::SendFailed, AttemptTracker::attempt, &tracker,
               t, timing, Safety::ReadOnly, policy);
    // After 2 delays (200ms), 3rd check: 200 < 250 → delay → 300ms → attempt
    // After 3 delays (300ms), 4th check: 300 >= 250 → break
    CHECK(tracker.calls == 3);
}

TEST_CASE("retry_loop: timeout_ms=0 means no limit") {
    LoopTestOps ops;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 5, .retry_delay_ms = 1, .timeout_ms = 0};
    AttemptTracker tracker;

    retry_loop(false, Error::SendFailed, AttemptTracker::attempt, &tracker,
               t, timing, Safety::ReadOnly, policy);
    CHECK(tracker.calls == 5);
}

TEST_CASE("retry_loop: records timing on success") {
    LoopTestOps ops;
    ops.now_ms = 100;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 0};

    retry_loop(true, Error{}, AttemptTracker::attempt, nullptr,
               t, timing, Safety::ReadOnly, policy);
    CHECK(timing.has_previous);
    CHECK(timing.last_transaction_end_ms == 100);
}

TEST_CASE("retry_loop: records timing on failure") {
    LoopTestOps ops;
    ops.now_ms = 200;
    auto t = ops.ops();
    TransactionTiming timing;
    RetryPolicy policy{.max_retries = 0};

    retry_loop(false, Error::Notecard, AttemptTracker::attempt, nullptr,
               t, timing, Safety::ReadOnly, policy);
    CHECK(timing.has_previous);
    CHECK(timing.last_transaction_end_ms == 200);
}

// ---------------------------------------------------------------------------
// Error to_string coverage
// ---------------------------------------------------------------------------

TEST_CASE("to_string(Error): all error codes") {
    CHECK(to_string(Error::NoError) == "no_error");
    CHECK(to_string(Error::SendFailed) == "send_failed");
    CHECK(to_string(Error::ResponseLost) == "response_lost");
    CHECK(to_string(Error::Notecard) == "notecard");
    CHECK(to_string(Error::Json) == "json");
    CHECK(to_string(Error::NotReady) == "not_ready");
    CHECK(to_string(Error::Overflow) == "overflow");
    CHECK(to_string(Error::InvalidArg) == "invalid_argument");
}

TEST_CASE("to_string(Cause): all cause codes") {
    CHECK(to_string(Cause::Unspecified) == "unspecified");
    CHECK(to_string(Cause::Timeout) == "timeout");
    CHECK(to_string(Cause::TimeoutIntra) == "timeout_intra");
    CHECK(to_string(Cause::HalError) == "hal_error");
    CHECK(to_string(Cause::CrcMismatch) == "crc_mismatch");
}

TEST_CASE("to_string(ErrorInfo): formatted with cause") {
    ErrorInfo ei{Error::SendFailed, Cause::Timeout, "connection lost"};
    auto s = to_string(ei);
    CHECK(std::string_view(s) == "send_failed[timeout]: connection lost");
}

TEST_CASE("to_string(ErrorInfo): formatted without cause") {
    ErrorInfo ei{Error::Json, Cause::Unspecified, "parse error"};
    auto s = to_string(ei);
    CHECK(std::string_view(s) == "json: parse error");
}
