#pragma once
/// @file cobs.hpp
/// Streaming COBS (Consistent Overhead Byte Stuffing) encoder and decoder.
///
/// note-c's COBS variant uses XOR: the core algorithm eliminates 0x00 bytes
/// (classic COBS), then all output bytes are XOR'd with `eop`. The Notecard
/// uses newline (0x0A) as the delimiter.
///
/// The encoder reads from a const source buffer and flushes encoded blocks
/// to a callback — no duplicate buffer, no in-place mutation. Each block is
/// at most 255 bytes (1 code byte + up to 254 data bytes), matching COBS's
/// natural block size.
///
/// The decoder is stateful: feed() accepts encoded bytes in any chunk size
/// and emits decoded output to a callback as blocks complete.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <note/span.hpp>

/// Size of the internal working block used by CobsEncoder and CobsDecoder.
/// Override before including this header for stack-constrained targets.
/// Must be at least 2.
#ifndef NOTE_COBS_BLOCK_SIZE
#define NOTE_COBS_BLOCK_SIZE 255
#endif

namespace note {

/// The COBS delimiter byte. The Notecard uses newline (0x0A), not zero.
inline constexpr uint8_t cobs_eop = '\n';

/// Maximum encoded size for a given input length (excluding the EOP byte).
inline constexpr size_t cobs_encoded_size(size_t len) {
    return len + (len / 254) + 1;
}

/// Streaming COBS encoder. Reads from a const source buffer and flushes
/// encoded blocks to a callable.
///
/// By default, uses a NOTE_COBS_BLOCK_SIZE-byte stack buffer inside encode().
/// Pass a byte_span to encode() to use caller-provided storage instead.
///
/// @code
/// note::CobsEncoder encoder;
/// encoder.encode(data, len, [&](const uint8_t* block, size_t n) {
///     transport.transmit(block, n);
/// });
/// @endcode
class CobsEncoder {
public:
    explicit CobsEncoder(uint8_t eop = cobs_eop) : eop_(eop) {}

    /// Encode `src` using a NOTE_COBS_BLOCK_SIZE stack buffer.
    template<typename Flush>
    void encode(const uint8_t* src, size_t len, Flush&& flush) const {
        uint8_t block[NOTE_COBS_BLOCK_SIZE];
        encode_impl(src, len, block, NOTE_COBS_BLOCK_SIZE, std::forward<Flush>(flush));
    }

    /// Encode `src` using a caller-provided scratch buffer.
    template<typename Flush>
    void encode(const uint8_t* src, size_t len, byte_span scratch, Flush&& flush) const {
        encode_impl(src, len, scratch.data(), scratch.size(), std::forward<Flush>(flush));
    }

private:
    template<typename Flush>
    void encode_impl(const uint8_t* src, size_t len,
                     uint8_t* block, size_t /*block_cap*/, Flush&& flush) const {
        size_t block_len = 1;   // slot 0 reserved for code byte
        uint8_t code = 1;
        size_t remaining = len;

        while (remaining > 0) {
            size_t max_data = 0xFFu - code;
            size_t search_len = (remaining < max_data) ? remaining : max_data;

            const uint8_t* zero = static_cast<const uint8_t*>(
                memchr(src, 0, search_len));
            size_t chunk = zero ? static_cast<size_t>(zero - src) : search_len;

            for (size_t i = 0; i < chunk; i++) {
                block[block_len++] = src[i] ^ eop_;
            }
            src += chunk;
            remaining -= chunk;
            code += static_cast<uint8_t>(chunk);

            if (zero && remaining > 0) {
                block[0] = code ^ eop_;
                flush(block, block_len);
                code = 1;
                block_len = 1;
                src++;
                remaining--;
            } else if (code == 0xFF) {
                block[0] = code ^ eop_;
                flush(block, block_len);
                code = 1;
                block_len = 1;
            }
        }

        block[0] = code ^ eop_;
        flush(block, block_len);
    }

    uint8_t eop_;
};

namespace detail {

/// Core COBS decoder logic over an external buffer pointer.
/// State only — no owned storage. buf must outlive the decoder.
class CobsDecoderCore {
public:
    CobsDecoderCore(uint8_t* buf, size_t cap, uint8_t eop)
        : eop_(eop), buf_(buf), buf_cap_(cap) { reset(); }

    void reset() {
        remaining_ = 0;
        first_   = true;
        no_zero_ = false;
        buf_len_ = 0;
    }

    template<typename Out>
    bool feed(const uint8_t* data, size_t len, Out&& out) {
        for (size_t i = 0; i < len; i++) {
            if (remaining_ == 0) {
                uint8_t code = data[i] ^ eop_;
                if (code == 0) {
                    flush(out);
                    return false;
                }
                if (!first_ && !no_zero_) {
                    buf_[buf_len_++] = 0;
                    maybe_flush(out);
                }
                first_     = false;
                remaining_ = code - 1;
                no_zero_   = (code == 0xFF);
            } else {
                buf_[buf_len_++] = data[i] ^ eop_;
                remaining_--;
                maybe_flush(out);
            }
        }
        return true;
    }

    template<typename Out>
    void flush(Out&& out) {
        if (buf_len_ > 0) {
            out(buf_, buf_len_);
            buf_len_ = 0;
        }
    }

private:
    template<typename Out>
    void maybe_flush(Out& out) {
        if (buf_len_ >= buf_cap_ / 2) {
            out(buf_, buf_len_);
            buf_len_ = 0;
        }
    }

    uint8_t  eop_;
    uint8_t  remaining_ = 0;
    bool     first_   = true;
    bool     no_zero_ = false;
    uint8_t* buf_;
    size_t   buf_cap_;
    size_t   buf_len_ = 0;
};

} // namespace detail

/// Streaming COBS decoder with owned working buffer (NOTE_COBS_BLOCK_SIZE+1 bytes).
/// Maintains state between feed() calls; encoded data can arrive in any chunk size.
///
/// @code
/// note::CobsDecoder decoder;
/// decoder.reset();
/// decoder.feed(chunk, chunk_len, [&](const uint8_t* data, size_t n) {
///     memcpy(dest + offset, data, n);
///     offset += n;
/// });
/// @endcode
class CobsDecoder : public detail::CobsDecoderCore {
    uint8_t storage_[NOTE_COBS_BLOCK_SIZE + 1];
public:
    explicit CobsDecoder(uint8_t eop = cobs_eop)
        : CobsDecoderCore(storage_, sizeof(storage_), eop) {}
};

/// COBS decoder that uses a caller-provided buffer.
/// The object itself is small (state only, ~20 bytes); buf must outlive the decoder.
/// Use when NOTE_COBS_BLOCK_SIZE bytes of decoder storage must not be on the call stack.
///
/// @code
/// static uint8_t dec_buf[NOTE_COBS_BLOCK_SIZE + 1];
/// note::CobsDecoderExt decoder(dec_buf);
/// @endcode
class CobsDecoderExt : public detail::CobsDecoderCore {
public:
    explicit CobsDecoderExt(byte_span buf, uint8_t eop = cobs_eop)
        : CobsDecoderCore(buf.data(), buf.size(), eop) {}
};

} // namespace note
