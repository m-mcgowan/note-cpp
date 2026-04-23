// Streaming-transport mock for stdcpp examples.
//
// Mirrors tests/test_endpoint_streaming.cpp's MockHal. Provides a
// TransportHal subclass that buffers transmitted bytes until the
// streaming transport's write_line_terminator() fires, then prints the
// captured JSON request and lets the example pick a canned response.
//
// Use with note::StreamingTransport + note::Notecard. The example
// overrides choose_response(const std::string& req) to pick the right
// canned payload based on what the library sent.
//
// In production, replace this with a real streaming transport — see
// examples/stdcpp/posix-hardware.cpp for the POSIX serial + I2C path.

#pragma once

#include <note/transport_hal.hpp>
#include <note/error.hpp>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>

/// Base streaming-transport mock. Subclass and override
/// `choose_response(req)` to seed the canned reply for each request.
class StreamingMockHal : public note::TransportHal {
public:
    std::string          tx_buf;
    std::deque<uint8_t>  rx;

    /// Seed the next response. Called from `choose_response` overrides.
    void prime(const std::string& response) {
        rx.clear();
        for (char c : response) rx.push_back(static_cast<uint8_t>(c));
        rx.push_back('\n');
    }

    bool transmit(const uint8_t* data, size_t len) override {
        tx_buf.append(reinterpret_cast<const char*>(data), len);
        return true;
    }

    // StreamingTransport calls this after the JSON body to finish the
    // line. We treat it as the request-complete signal.
    bool write_line_terminator() override {
        if (!tx_buf.empty()) {
            std::printf("  >> %s\n", tx_buf.c_str());
            if (tx_buf.find("\"req\":") != std::string::npos ||
                tx_buf.find("\"cmd\":") != std::string::npos) {
                choose_response(tx_buf);
            }
            tx_buf.clear();
        }
        return true;
    }

    note::Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t) override {
        if (rx.empty()) {
            return note::make_error(note::Error::ResponseLost,
                                    note::Cause::Timeout, "no data");
        }
        const size_t n = std::min(max_len, rx.size());
        for (size_t i = 0; i < n; ++i) {
            buf[i] = rx.front();
            rx.pop_front();
        }
        return n;
    }

    bool    reset()                 override { return true; }
    void    delay(uint32_t)         override {}
    uint32_t millis()               override { return 0; }

protected:
    /// Override to pick the canned response for `req` (the full JSON
    /// request string just transmitted). Default: empty object.
    virtual void choose_response(const std::string& /*req*/) {
        prime("{}");
    }
};
