#pragma once

#include "detail/number_format.hpp"
#include "json_sax.hpp"
#include "types.hpp"

namespace note {

// ---------------------------------------------------------------------------
// JsonWriter — abstract byte-level write sink for streaming JSON generation.
// Implementations include BufferWriter (writes to char[]) and CrcWriter
// (accumulates CRC and forwards to an inner writer).
// ---------------------------------------------------------------------------

class JsonWriter {
public:
    virtual ~JsonWriter() = default;
    virtual bool write(const char* data, size_t len) = 0;

    bool write(char c) { return write(&c, 1); }
    bool write(string_view sv) { return write(sv.data(), sv.size()); }
};

// ---------------------------------------------------------------------------
// JsonBufferWriter — writes to a fixed char buffer through the JsonWriter
// interface. Tracks overflow; never writes past capacity.
// ---------------------------------------------------------------------------

class JsonBufferWriter : public JsonWriter {
public:
    using JsonWriter::write;  // un-hide base class write(char) and write(string_view)

    JsonBufferWriter(char* buf, size_t capacity)
        : buf_(buf), capacity_(capacity) {}

    bool write(const char* data, size_t len) override {
        for (size_t i = 0; i < len; ++i) {
            if (pos_ < capacity_) buf_[pos_] = data[i];
            ++pos_;
        }
        return pos_ <= capacity_;
    }

    char* data() const { return buf_; }
    size_t pos() const { return pos_; }
    size_t capacity() const { return capacity_; }
    bool overflow() const { return pos_ > capacity_; }

    // Return a view of the written bytes (clamped to capacity).
    string_view view() const {
        return {buf_, pos_ < capacity_ ? pos_ : capacity_};
    }

    void reset() { pos_ = 0; }

private:
    char* buf_;
    size_t capacity_;
    size_t pos_ = 0;
};

class JsonBuilder {
public:
    virtual ~JsonBuilder() = default;

    virtual JsonBuilder& add(string_view key, bool value) = 0;
    virtual JsonBuilder& add(string_view key, int32_t value) = 0;
    virtual JsonBuilder& add(string_view key, double value) = 0;
    virtual JsonBuilder& add(string_view key, string_view value) = 0;

    // Prevent const char* from matching the bool overload.
    JsonBuilder& add(string_view key, const char* value) {
        return add(key, string_view(value));
    }

    virtual JsonBuilder& begin_object(string_view key) = 0;
    virtual JsonBuilder& end_object() = 0;

    virtual JsonBuilder& begin_array(string_view key) = 0;
    virtual JsonBuilder& end_array() = 0;

    // Add an element to the current array (no key — must be inside begin_array/end_array).
    // Default: no-op. Override in backends that support array serialization.
    virtual JsonBuilder& add_element(bool) { return *this; }
    virtual JsonBuilder& add_element(int32_t) { return *this; }
    virtual JsonBuilder& add_element(double) { return *this; }
    virtual JsonBuilder& add_element(string_view) { return *this; }
    JsonBuilder& add_element(const char* value) {
        return add_element(string_view(value));
    }

    // Embed a raw JSON fragment as a value (no quoting).
    // Used for body content that's already valid JSON.
    virtual JsonBuilder& add_raw(string_view key, string_view json_fragment) = 0;


    // Finalize and return a view into the builder's internal buffer.
    // The view is valid until the next call to reset() or to_view().
    // All backends must implement this — it's the primary serialization method.
    virtual string_view to_view() = 0;

    // Reset builder state for reuse (avoid re-allocating the builder object).
    // Default implementation is a no-op; backends override to clear internal state.
    virtual void reset() {}
};

class JsonReader {
public:
    virtual ~JsonReader() = default;

    virtual bool has(string_view key) const = 0;

    virtual bool get_bool(string_view key, bool def = false) const = 0;
    virtual int32_t get_int(string_view key, int32_t def = 0) const = 0;
    virtual double get_double(string_view key, double def = 0.0) const = 0;
    virtual string_view get_string(string_view key, string_view def = {}) const = 0;

    virtual std::unique_ptr<JsonReader> get_object(string_view key) const = 0;

    /// Read a JSON array of strings. Returns the number of elements read.
    /// Default: 0 (backends override to implement).
    virtual size_t get_string_array(string_view key, string_view* out, size_t max) const {
        (void)key; (void)out; (void)max;
        return 0;
    }

    virtual bool has_error() const = 0;
    virtual string_view get_error() const = 0;
};

class JsonBackend {
public:
    virtual ~JsonBackend() = default;

    virtual std::unique_ptr<JsonBuilder> create_builder() = 0;

    // Parse a JSON response string and return a reader.
    virtual std::unique_ptr<JsonReader> parse_response(string_view json) = 0;

    // Return a reference to a reusable builder owned by the backend.
    // Avoids the unique_ptr allocation of create_builder() in steady state.
    // Default implementation wraps create_builder() for backward compatibility.
    virtual JsonBuilder& get_builder() {
        owned_builder_ = create_builder();
        return *owned_builder_;
    }

    // Return a reference to a reusable reader owned by the backend.
    // The reader is valid until the next get_reader() call.
    // Default implementation wraps parse_response() for backward compatibility.
    virtual JsonReader& get_reader(string_view json) {
        owned_reader_ = parse_response(json);
        return *owned_reader_;
    }

    // SAX-parse a JSON string and deliver events to a sink.
    // Returns empty string_view on success, error message on failure.
    // Default implementation uses the built-in SAX parser. Tree-based backends
    // (cJSON, nlohmann) can override to walk their tree instead.
    virtual string_view parse_into(string_view json, JsonSink& sink) {
        return sax_parse(json, sink);
    }

private:
    std::unique_ptr<JsonBuilder> owned_builder_;
    std::unique_ptr<JsonReader> owned_reader_;
};

// ---------------------------------------------------------------------------
// StreamingJsonBuilder — JsonBuilder that writes through a JsonWriter.
//
// Same interface as BufferJsonBuilder, but each add() call writes directly
// to the underlying JsonWriter instead of appending to an internal buffer.
// This is the inverse of the SAX parser: SAX is streaming parse (events in
// from bytes), this is streaming build (events out to bytes).
//
// The constructor writes '{'. The caller must NOT call to_view() in the
// streaming path — the transport handles closing '}', CRC, and framing.
// to_view() is provided for standalone use (returns empty view after closing).
// ---------------------------------------------------------------------------

class StreamingJsonBuilder : public JsonBuilder {
public:
    explicit StreamingJsonBuilder(JsonWriter& w) : writer_(w) {
        writer_.write('{');
    }

    JsonBuilder& add(string_view key, bool value) override {
        kv(key);
        writer_.write(value ? string_view("true") : string_view("false"));
        return *this;
    }

    JsonBuilder& add(string_view key, int32_t value) override {
        kv(key);
        char tmp[12];
        size_t len = detail::itoa(tmp, sizeof(tmp), value);
        writer_.write(tmp, len);
        return *this;
    }

    JsonBuilder& add(string_view key, double value) override {
        kv(key);
        char tmp[32];
        size_t len = detail::dtoa(tmp, sizeof(tmp), value);
        writer_.write(tmp, len);
        return *this;
    }

    JsonBuilder& add(string_view key, string_view value) override {
        kv(key);
        quoted(value);
        return *this;
    }

    JsonBuilder& add_raw(string_view key, string_view json_fragment) override {
        kv(key);
        writer_.write(json_fragment);
        return *this;
    }


    JsonBuilder& begin_object(string_view key) override {
        kv(key);
        writer_.write('{');
        need_comma_ = false;
        return *this;
    }

    JsonBuilder& end_object() override {
        writer_.write('}');
        need_comma_ = true;
        return *this;
    }

    JsonBuilder& begin_array(string_view key) override {
        kv(key);
        writer_.write('[');
        need_comma_ = false;
        return *this;
    }

    JsonBuilder& end_array() override {
        writer_.write(']');
        need_comma_ = true;
        return *this;
    }

    JsonBuilder& add_element(bool value) override {
        comma();
        writer_.write(value ? string_view("true") : string_view("false"));
        return *this;
    }

    JsonBuilder& add_element(int32_t value) override {
        comma();
        char tmp[12];
        size_t len = detail::itoa(tmp, sizeof(tmp), value);
        writer_.write(tmp, len);
        return *this;
    }

    JsonBuilder& add_element(double value) override {
        comma();
        char tmp[32];
        size_t len = detail::dtoa(tmp, sizeof(tmp), value);
        writer_.write(tmp, len);
        return *this;
    }

    JsonBuilder& add_element(string_view value) override {
        comma();
        quoted(value);
        return *this;
    }

    // Standalone use: closes the JSON object and returns empty view.
    // In the streaming transport path, this is NOT called — the transport
    // handles closing '}', CRC field, and line terminator.
    string_view to_view() override {
        if (!closed_) {
            writer_.write('}');
            closed_ = true;
        }
        return {};
    }

    void reset() override {
        need_comma_ = false;
        closed_ = false;
        writer_.write('{');
    }

private:
    JsonWriter& writer_;
    bool need_comma_ = false;
    bool closed_ = false;

    void comma() {
        if (need_comma_) writer_.write(',');
        need_comma_ = true;
    }

    void quoted(string_view s) {
        writer_.write('"');
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            switch (c) {
            case '"':  writer_.write('\\'); writer_.write('"'); break;
            case '\\': writer_.write('\\'); writer_.write('\\'); break;
            case '\n': writer_.write('\\'); writer_.write('n'); break;
            case '\r': writer_.write('\\'); writer_.write('r'); break;
            case '\t': writer_.write('\\'); writer_.write('t'); break;
            default:   writer_.write(c); break;
            }
        }
        writer_.write('"');
    }

    void kv(string_view key) {
        comma();
        quoted(key);
        writer_.write(':');
    }
};

} // namespace note
