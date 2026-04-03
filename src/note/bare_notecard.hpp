#pragma once

/// @file bare_notecard.hpp
/// BareNotecard — raw JSON passthrough over a Notecard transport.
///
/// Sends and receives pre-formatted JSON strings without parsing or
/// typed response handling. Useful for:
/// - Serial passthrough protocols (forwarding external JSON to the Notecard)
/// - Debug consoles
/// - Bridge firmware that relays arbitrary requests
///
/// No allocator, no JSON backend, no typed API. Just validated JSON in/out.
/// Assumes NonIdempotent safety — only retries on SendFailed.
///
/// Usage:
///   note::StreamingTransport transport(hal);
///   note::BareNotecard bare(transport);
///
///   char buf[512];
///   auto rsp = bare.transact(R"({"req":"card.version"})", buf);
///   bare.send(R"({"cmd":"hub.set","product":"com.example"})");

#include "error.hpp"
#include "json_sax.hpp"
#include "owned_buffer.hpp"
#include "retry.hpp"
#include "retry_policy.hpp"
#include "safety.hpp"
#include "span.hpp"
#include "streaming_transport.hpp"
#include "types.hpp"

namespace note {

class BareNotecard {
public:
    explicit BareNotecard(StreamingTransport& transport)
        : transport_(transport) {}

    /// Send pre-formatted JSON request, read response into caller's buffer.
    /// Assumes NonIdempotent — only retries if the request never reached the Notecard.
    Result<string_view> transact(string_view json, span<char> buf) {
        if (!validate(json))
            return make_error(Error::Json, NOTE_ERR("malformed JSON"));

        auto attempt = [&]() -> Result<string_view> {
            return transport_.transact_raw(json, buf.data(), buf.size(), timeout_ms_);
        };
        auto reset = [&]() { transport_.reset(); };

        return retry_transaction<Result<string_view>>(
            transport_, timing_, Safety::NonIdempotent, retry_policy_,
            attempt, reset);
    }

    /// Send pre-formatted JSON request with explicit Safety override.
    /// Use Safety::ReadOnly or Safety::Idempotent if you know the request is safe to retry.
    Result<string_view> transact(string_view json, span<char> buf, Safety safety) {
        if (!validate(json))
            return make_error(Error::Json, NOTE_ERR("malformed JSON"));

        auto attempt = [&]() -> Result<string_view> {
            return transport_.transact_raw(json, buf.data(), buf.size(), timeout_ms_);
        };
        auto reset = [&]() { transport_.reset(); };

        return retry_transaction<Result<string_view>>(
            transport_, timing_, safety, retry_policy_,
            attempt, reset);
    }

    /// Send pre-formatted JSON command (fire-and-forget).
    /// Inter-transaction timing enforced, but no retry.
    Result<void> send(string_view json) {
        if (!validate(json))
            return make_error(Error::Json, NOTE_ERR("malformed JSON"));
        enforce_timing();
        auto result = transport_.send_raw(json);
        record_timing();
        return result;
    }

    void set_timeout(uint32_t ms) { timeout_ms_ = ms; }
    uint32_t timeout() const { return timeout_ms_; }

    void set_retry_policy(RetryPolicy policy) { retry_policy_ = policy; }
    void set_inter_transaction_gap(uint32_t ms) { timing_.min_gap_ms = ms; }

private:
    static bool validate(string_view json) {
        JsonSink null_sink;
        return sax_parse(json, null_sink).empty();
    }

    void enforce_timing() {
        if (!timing_.has_previous) return;
        uint32_t elapsed = transport_.millis() - timing_.last_transaction_end_ms;
        if (elapsed < timing_.min_gap_ms)
            transport_.delay(timing_.min_gap_ms - elapsed);
    }

    void record_timing() {
        timing_.last_transaction_end_ms = transport_.millis();
        timing_.has_previous = true;
    }

    StreamingTransport& transport_;
    uint32_t timeout_ms_ = 10000;
    RetryPolicy retry_policy_{};
    TransactionTiming timing_{};
};

} // namespace note
