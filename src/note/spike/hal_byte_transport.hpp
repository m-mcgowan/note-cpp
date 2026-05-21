#pragma once

/// @file spike/hal_byte_transport.hpp
/// SPIKE: concrete IByteTransport over a note::Hal. Mirrors the byte-level
/// pieces of the existing Protocol class (HAL I/O, init handshake, frame
/// terminator detection, lookahead buffer) but without any wire-format or
/// CRC knowledge.

#include <note/transport.hpp>

#include <algorithm>
#include <cstring>

namespace note { namespace spike {

class HalByteTransport : public IByteTransport {
public:
    explicit HalByteTransport(Hal& hal) : hal_(hal) {}

    Result<void> begin_transaction(uint32_t /*timeout_ms*/) override {
        if (!ensure_init())
            return make_error(Error::NotReady, NOTE_ERR("not ready"));
        frame_terminated_ = false;
        any_data_received_ = false;
        return {};
    }

    void end_transaction() override {
        if (!frame_terminated_ && any_data_received_)
            drain_frame_boundary();
    }

    Result<void> write(const uint8_t* data, size_t len) override {
        if (!hal_.transmit(data, len))
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("transmit failed"));
        return {};
    }

    Result<void> write_frame_terminator() override {
        if (!hal_.write_line_terminator())
            return make_error(Error::SendFailed, Cause::HalError,
                              NOTE_ERR("line terminator failed"));
        return {};
    }

    Result<size_t> read(uint8_t* buf, size_t max, uint32_t timeout_ms) override {
        if (frame_terminated_)
            return make_error(Error::EndOfFrame, NOTE_ERR("frame complete"));

        size_t n = 0;
        uint32_t start = hal_.millis();
        while (n == 0) {
            auto r = read_hal(buf, max, timeout_ms);
            if (!r) return r;
            n = *r;
            if (n == 0) {
                if (timeout_ms > 0 && hal_.millis() - start >= timeout_ms)
                    return make_error(Error::ResponseLost, Cause::Timeout,
                                      NOTE_ERR("timeout"));
                hal_.delay(1);
            }
        }
        any_data_received_ = true;

        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                frame_terminated_ = true;
                size_t after = n - i - 1;
                if (after > 0 && after <= sizeof(lookahead_)) {
                    std::memcpy(lookahead_, buf + i + 1, after);
                    lookahead_pos_ = 0;
                    lookahead_len_ = after;
                }
                return i;
            }
        }
        return n;
    }

    void reset() override {
        hal_.reset();
        initialized_ = false;
    }
    void abort() override {}
    Hal& hal() override { return hal_; }

private:
    Result<size_t> read_hal(uint8_t* buf, size_t max, uint32_t timeout_ms) {
        if (lookahead_len_ > 0) {
            size_t n = std::min(max, lookahead_len_);
            std::memcpy(buf, lookahead_ + lookahead_pos_, n);
            lookahead_pos_ += n;
            lookahead_len_ -= n;
            return n;
        }
        return hal_.read(buf, max, timeout_ms);
    }

    bool ensure_init() {
        if (initialized_) return true;
        if (!hal_.reset()) return false;
        initialized_ = true;
        return true;
    }

    void drain_frame_boundary() {
        uint8_t byte;
        for (size_t guard = 0; guard < 256; ++guard) {
            auto rv = read_hal(&byte, 1, 250);
            if (!rv || *rv == 0) break;
            if (byte == '\n') break;
        }
    }

    Hal& hal_;
    bool initialized_ = false;
    bool frame_terminated_ = false;
    bool any_data_received_ = false;
    uint8_t lookahead_[64]{};
    size_t lookahead_pos_ = 0;
    size_t lookahead_len_ = 0;
};

}} // namespace note::spike
