#pragma once

// Scripted IBufferedTransport for tests — returns a fixed string_view response
// from a const-char-pointer buffer the test owns. No std::string, no
// std::function, no allocations on the hot path.
//
// Tests that need allocation-counted exactness (test_alloc_profile,
// test_sax_alloc_profile) can't use note::test::CallbackTransport because that wraps
// std::function, which may allocate depending on stdlib SBO behavior.
//
// Usage:
//     ScriptedTransport t;
//     t.response = R"({"version":"notecard-7.2.1"})";
//     note::Notecard nc(backend, t);

#include <note/transport.hpp>

#include <cstdint>

namespace note::test {

struct ScriptedTransport : note::IBufferedTransport {
    const char* response = "{}";

    Result<string_view> transact(string_view, uint32_t) override {
        return string_view(response);
    }
    Result<void> send(string_view) override { return {}; }
    void reset() override {}
    void abort() override {}

    // Minimal Hal — same shape as note::test::CallbackHal — so Notecard's
    // hal()-mediated timing path has something valid to call into. No
    // hardware behind any of this.
    struct NoopHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        Result<size_t> read(uint8_t*, size_t, uint32_t) override { return Result<size_t>{size_t{0}}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return 0; }
        void delay(uint32_t) override {}
    } hal_;
    note::Hal& hal() override { return hal_; }
};

} // namespace note::test
