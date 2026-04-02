#pragma once

/// @file owned_buffer.hpp
/// OwnedBuffer — RAII-managed contiguous byte buffer.
///
/// Owns memory obtained from an Allocator. Move-only. Frees on destruction.
/// Can grow via reallocate(). Provides string_view access, iteration,
/// range copy, and Arduino Printable support.
///
/// Usage:
///   auto buf = OwnedBuffer::create(alloc, 1024);
///   buf.append('x');
///   buf.append(data, len);
///   auto sv = buf.view();       // valid for buf's lifetime
///   Serial.println(buf);        // Arduino Printable

#include "allocator.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstring>

#ifndef NOTE_PRINTABLE
#ifdef ARDUINO
#define NOTE_PRINTABLE 1
#else
#define NOTE_PRINTABLE 0
#endif
#endif

namespace note {

class OwnedBuffer
#if defined(ARDUINO) && NOTE_PRINTABLE
    : public Printable
#endif
{
public:
    OwnedBuffer() = default;

    /// Create with initial capacity from the given allocator.
    /// Returns an empty buffer (size 0) with the given capacity.
    static OwnedBuffer create(Allocator alloc, size_t initial_capacity) {
        OwnedBuffer b;
        b.alloc_ = alloc;
        b.capacity_ = initial_capacity;
        b.data_ = static_cast<char*>(alloc.allocate(initial_capacity));
        if (!b.data_) b.capacity_ = 0;
        return b;
    }

    ~OwnedBuffer() { free(); }

    // Move-only
    OwnedBuffer(OwnedBuffer&& o) noexcept
        : data_(o.data_), size_(o.size_), capacity_(o.capacity_), alloc_(o.alloc_) {
        o.data_ = nullptr; o.size_ = 0; o.capacity_ = 0;
    }
    OwnedBuffer& operator=(OwnedBuffer&& o) noexcept {
        if (this != &o) {
            free();
            data_ = o.data_; size_ = o.size_; capacity_ = o.capacity_; alloc_ = o.alloc_;
            o.data_ = nullptr; o.size_ = 0; o.capacity_ = 0;
        }
        return *this;
    }
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;

    // ── Size and capacity ──────────────────────────────────────────────

    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }

    // ── Access ─────────────────────────────────────────────────────────

    /// View of the buffer contents. Valid for the OwnedBuffer's lifetime.
    string_view view() const { return {data_, size_}; }
    operator string_view() const { return view(); }

    const char* data() const { return data_; }
    const char* begin() const { return data_; }
    const char* end() const { return data_ + size_; }

    char operator[](size_t i) const { return data_[i]; }

    // ── Mutation ───────────────────────────────────────────────────────

    /// Append a single byte. Returns false if growth fails.
    bool append(char c) {
        if (!ensure_capacity(size_ + 1)) return false;
        data_[size_++] = c;
        return true;
    }

    /// Append a range of bytes. Returns false if growth fails.
    bool append(const char* src, size_t len) {
        if (!ensure_capacity(size_ + len)) return false;
        std::memcpy(data_ + size_, src, len);
        size_ += len;
        return true;
    }

    /// Null-terminate the buffer (doesn't increase size).
    void null_terminate() {
        if (data_ && size_ < capacity_) data_[size_] = '\0';
    }

    /// Copy contents to an external buffer. Returns bytes copied.
    size_t copy_to(char* dst, size_t max, size_t offset = 0) const {
        if (offset >= size_) return 0;
        size_t n = std::min(max, size_ - offset);
        std::memcpy(dst, data_ + offset, n);
        return n;
    }

    /// Reset size to 0 without freeing memory.
    void clear() { size_ = 0; }

    /// Release ownership and return the raw pointer.
    /// Caller is responsible for freeing via the original allocator.
    char* release() {
        char* p = data_;
        data_ = nullptr; size_ = 0; capacity_ = 0;
        return p;
    }

    /// Check if the buffer has valid backing memory.
    explicit operator bool() const { return data_ != nullptr; }

#if defined(ARDUINO) && NOTE_PRINTABLE
    size_t printTo(Print& p) const override {
        return p.write(reinterpret_cast<const uint8_t*>(data_), size_);
    }
#endif

private:
    void free() {
        if (data_) alloc_.deallocate(data_, capacity_);
        data_ = nullptr; size_ = 0; capacity_ = 0;
    }

    bool ensure_capacity(size_t needed) {
        if (needed < capacity_) return true;
        size_t new_cap = capacity_ ? capacity_ : 256;
        while (new_cap <= needed) new_cap *= 2;
        auto* grown = static_cast<char*>(alloc_.reallocate(data_, capacity_, new_cap));
        if (!grown) return false;
        data_ = grown;
        capacity_ = new_cap;
        return true;
    }

    char* data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
    Allocator alloc_{};
};

} // namespace note
