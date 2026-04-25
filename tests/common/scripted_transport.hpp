#pragma once

// Scripted IBufferedTransport for tests — returns a fixed string_view response
// from a const-char-pointer buffer the test owns. No std::string, no
// std::function, no allocations on the hot path.
//
// Tests that need allocation-counted exactness (test_alloc_profile,
// test_sax_alloc_profile) can't use note::CallbackTransport because that wraps
// std::function, which may allocate depending on stdlib SBO behavior.
//
// Usage:
//     ScriptedTransport t;
//     t.response = R"({"version":"notecard-7.2.1"})";
//     note::Notecard nc(backend, t);

#include <note/transport.hpp>

#include <cstdint>

namespace note::test {

struct ScriptedTransport : note::ITransport {
    const char* response = "{}";

    Result<string_view> transact(string_view, uint32_t) override {
        return string_view(response);
    }
    Result<void> send(string_view) override { return {}; }
    void reset() override {}
    void abort() override {}
    uint32_t millis() override { return 0; }
    void delay(uint32_t) override {}
};

} // namespace note::test
