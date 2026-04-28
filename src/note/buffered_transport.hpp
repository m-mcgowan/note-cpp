#pragma once

/// @file buffered_transport.hpp
/// BufferedStreamingTransport — adapter exposing IBufferedTransport
/// over a StreamingTransport.
///
/// The buffered Notecard ctor (Notecard(JsonBackend&,
/// IBufferedTransport&)) needs an IBufferedTransport. Note-cpp's
/// built-in I2C and serial HALs feed StreamingTransport (the streaming
/// path), not IBufferedTransport, so the buffered Notecard ctor was
/// historically only constructible against the host-side
/// CallbackTransport.
///
/// This adapter wraps any StreamingTransport plus a caller-supplied
/// response buffer and presents it as an IBufferedTransport. With it
/// you can run the buffered Notecard ctor over Arduino I2C / serial,
/// POSIX serial, or any other StreamingTransport-backed HAL — and
/// `Response::body()` returns a JsonReader* on hardware.
///
/// Example (Arduino I2C, ESP32, with a JsonBackend such as
/// note::backends::CjsonBackend):
///
///     note::arduino::I2CHal hal(Wire);
///     note::transport::NotecardI2c<> i2c{hal};
///     note::StreamingTransport streaming{i2c};
///
///     char rsp_buf[1024];
///     note::BufferedStreamingTransport buffered{streaming, rsp_buf};
///
///     note::backends::CjsonBackend backend;
///     note::Notecard nc(backend, buffered);
///     note::Api<> api(nc);
///
///     auto r = api.env.get().execute();
///     if (auto* body = r.body()) {
///         // walk body via JsonReader…
///     }
///
/// The adapter doesn't allocate — it points into the caller's buffer.
/// Size that buffer to the largest expected response.

#include <note/transport.hpp>
#include <note/streaming_transport.hpp>
#include <note/span.hpp>
#include <note/types.hpp>

// The buffered Notecard ctor is gated by NOTE_NO_BUFFERED — the buffered
// execute path is compiled out under NOTE_MINIMAL. The adapter exists
// only to feed that ctor, so gate it the same way.
//
// IBufferedTransport-derived classes need full virtuals, so this also
// only makes sense when the polymorphic streaming transport is in play.
#if !NOTE_NO_BUFFERED && !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL

namespace note {

/// Wraps a StreamingTransport + caller-supplied response buffer as an
/// IBufferedTransport. The buffer must outlive the adapter.
class BufferedStreamingTransport : public IBufferedTransport {
public:
    BufferedStreamingTransport(StreamingTransport& streaming, span<char> buf)
        : streaming_(streaming), buf_(buf) {}

    BufferedStreamingTransport(StreamingTransport& streaming, char* buf, size_t n)
        : streaming_(streaming), buf_(buf, n) {}

    Result<string_view> transact(string_view request, uint32_t timeout_ms) override {
        return streaming_.transact_raw(request, buf_.data(), buf_.size(), timeout_ms);
    }

    Result<void> send(string_view request) override {
        return streaming_.send_raw(request);
    }

    void reset() override { streaming_.reset(); }
    void abort() override { streaming_.abort(); }
    Hal& hal() override { return streaming_.hal(); }

    Result<void> write(const uint8_t* data, size_t len) override {
        return streaming_.write(data, len);
    }

    Result<size_t> read(uint8_t* buf, size_t max_len, uint32_t timeout_ms) override {
        return streaming_.read(buf, max_len, timeout_ms);
    }

    /// Mutable access to the underlying response buffer (e.g. to swap
    /// in a larger buffer between transactions).
    span<char> buffer() const { return buf_; }
    void set_buffer(span<char> buf) { buf_ = buf; }

private:
    StreamingTransport& streaming_;
    span<char> buf_;
};

} // namespace note

#endif // !NOTE_NO_BUFFERED && !NOTE_NO_POLYMORPHIC && !NOTE_STATIC_HAL
