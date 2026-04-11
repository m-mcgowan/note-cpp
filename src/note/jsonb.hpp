#pragma once
/// @file jsonb.hpp
/// JSONB binary wire format — opcode constants and streaming builder.
///
/// JSONB is Blues' TLV binary encoding for Notecard communication.
/// Each value is an opcode byte followed by its payload. Strings are
/// null-terminated; integers and floats are little-endian fixed width.
///
/// The builder implements JsonBuilder and writes opcodes through a
/// JsonWriter (which may be backed by a COBS encoder in the streaming
/// transport path).

#include "json.hpp"

#include <cstdint>
#include <cstring>

namespace note {

// ---------------------------------------------------------------------------
// JSONB opcode constants
// ---------------------------------------------------------------------------

namespace jsonb {

constexpr uint8_t kBeginObject = 0x10;
constexpr uint8_t kEndObject   = 0x11;
constexpr uint8_t kBeginArray  = 0x12;
constexpr uint8_t kEndArray    = 0x13;

constexpr uint8_t kNull        = 0x20;
constexpr uint8_t kTrue        = 0x21;
constexpr uint8_t kFalse       = 0x22;

constexpr uint8_t kItem        = 0x30;  // field name (null-terminated)
constexpr uint8_t kString      = 0x40;  // string value (null-terminated)

constexpr uint8_t kInt8        = 0x61;
constexpr uint8_t kInt16       = 0x62;
constexpr uint8_t kInt32       = 0x64;
constexpr uint8_t kInt64       = 0x68;

constexpr uint8_t kUint8       = 0x71;
constexpr uint8_t kUint16      = 0x72;
constexpr uint8_t kUint32      = 0x74;
constexpr uint8_t kUint64      = 0x78;

constexpr uint8_t kFloat       = 0x84;
constexpr uint8_t kDouble      = 0x88;

/// COBS XOR byte for JSONB framing (eliminates newlines from payload).
constexpr uint8_t kCobsXor     = '\n';

}  // namespace jsonb

// ---------------------------------------------------------------------------
// StreamingJsonbBuilder — JsonBuilder that emits JSONB opcodes through a
// JsonWriter.
//
// Same streaming architecture as StreamingJsonBuilder: the constructor
// emits kBeginObject, each add() writes opcodes directly. The transport
// handles framing ({: header, COBS encoding, :}\n trailer) and closing
// the root object (kEndObject).
//
// to_view() is for standalone use only — it closes with kEndObject.
// ---------------------------------------------------------------------------

class StreamingJsonbBuilder : public JsonBuilder {
public:
    explicit StreamingJsonbBuilder(JsonWriter& w) : writer_(w) {
        emit(jsonb::kBeginObject);
    }

    JsonBuilder& add(string_view key, bool value) override {
        emit_item(key);
        emit(value ? jsonb::kTrue : jsonb::kFalse);
        return *this;
    }

    JsonBuilder& add(string_view key, int32_t value) override {
        emit_item(key);
        emit(jsonb::kInt32);
        emit_le32(value);
        return *this;
    }

    JsonBuilder& add(string_view key, double value) override {
        emit_item(key);
        emit(jsonb::kDouble);
        emit_bytes(&value, 8);
        return *this;
    }

    JsonBuilder& add(string_view key, string_view value) override {
        emit_item(key);
        emit(jsonb::kString);
        writer_.write(value.data(), value.size());
        emit('\0');
        return *this;
    }

    JsonBuilder& add_raw(string_view, string_view) override {
        // Raw JSON fragments cannot be embedded in JSONB.
        return *this;
    }

    JsonBuilder& begin_object(string_view key) override {
        emit_item(key);
        emit(jsonb::kBeginObject);
        return *this;
    }

    JsonBuilder& end_object() override {
        emit(jsonb::kEndObject);
        return *this;
    }

    JsonBuilder& begin_array(string_view key) override {
        emit_item(key);
        emit(jsonb::kBeginArray);
        return *this;
    }

    JsonBuilder& end_array() override {
        emit(jsonb::kEndArray);
        return *this;
    }

    // Array elements — no kItem prefix.
    JsonBuilder& add_element(bool value) override {
        emit(value ? jsonb::kTrue : jsonb::kFalse);
        return *this;
    }

    JsonBuilder& add_element(int32_t value) override {
        emit(jsonb::kInt32);
        emit_le32(value);
        return *this;
    }

    JsonBuilder& add_element(double value) override {
        emit(jsonb::kDouble);
        emit_bytes(&value, 8);
        return *this;
    }

    JsonBuilder& add_element(string_view value) override {
        emit(jsonb::kString);
        writer_.write(value.data(), value.size());
        emit('\0');
        return *this;
    }

    string_view to_view() override {
        if (!closed_) {
            emit(jsonb::kEndObject);
            closed_ = true;
        }
        return {};
    }

    void reset() override {
        closed_ = false;
        emit(jsonb::kBeginObject);
    }

private:
    JsonWriter& writer_;
    bool closed_ = false;

    void emit(uint8_t opcode) {
        writer_.write(reinterpret_cast<const char*>(&opcode), 1);
    }

    void emit_bytes(const void* data, size_t len) {
        writer_.write(reinterpret_cast<const char*>(data), len);
    }

    void emit_le32(int32_t value) {
        uint8_t le[4];
        auto uval = static_cast<uint32_t>(value);
        le[0] = static_cast<uint8_t>(uval);
        le[1] = static_cast<uint8_t>(uval >> 8);
        le[2] = static_cast<uint8_t>(uval >> 16);
        le[3] = static_cast<uint8_t>(uval >> 24);
        emit_bytes(le, 4);
    }

    void emit_item(string_view key) {
        emit(jsonb::kItem);
        writer_.write(key.data(), key.size());
        emit('\0');
    }
};

}  // namespace note
