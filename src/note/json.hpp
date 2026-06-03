#pragma once

#include "detail/number_format.hpp"
#include "json_sax.hpp"
#include "span.hpp"
#include "types.hpp"

#include <type_traits>

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
    virtual JsonBuilder& add(string_view key, json_int_t value) = 0;
    virtual JsonBuilder& add(string_view key, double value) = 0;
    virtual JsonBuilder& add(string_view key, string_view value) = 0;

    // Prevent const char* from matching the bool overload.
    JsonBuilder& add(string_view key, const char* value) {
        return add(key, string_view(value));
    }

    // Widen narrower integer types to json_int_t to prevent ambiguity
    // with the bool overload (int → bool and int → int64_t are both
    // standard conversions of equal rank without these).
    template<typename T, std::enable_if_t<
        std::is_integral_v<T> && !std::is_same_v<T, bool> &&
        !std::is_same_v<T, json_int_t> && !std::is_same_v<T, char>, int> = 0>
    JsonBuilder& add(string_view key, T value) {
        return add(key, static_cast<json_int_t>(value));
    }

    virtual JsonBuilder& begin_object(string_view key) = 0;
    virtual JsonBuilder& end_object() = 0;

    virtual JsonBuilder& begin_array(string_view key) = 0;
    virtual JsonBuilder& end_array() = 0;

    // Start an object as an element of the current array (no key — must be
    // inside begin_array/end_array). Match with end_object().
    // Default: no-op. Override in backends that support array serialization.
    virtual JsonBuilder& begin_element_object() { return *this; }

    // Add an element to the current array (no key — must be inside begin_array/end_array).
    // Default: no-op. Override in backends that support array serialization.
    virtual JsonBuilder& add_element(bool) { return *this; }
    virtual JsonBuilder& add_element(json_int_t) { return *this; }
    virtual JsonBuilder& add_element(double) { return *this; }
    virtual JsonBuilder& add_element(string_view) { return *this; }
    template<typename T, std::enable_if_t<
        std::is_integral_v<T> && !std::is_same_v<T, bool> &&
        !std::is_same_v<T, json_int_t> && !std::is_same_v<T, char>, int> = 0>
    JsonBuilder& add_element(T value) {
        return add_element(static_cast<json_int_t>(value));
    }
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

    // Wire-format escape hatch for callers that hold a pre-rendered value
    // (already in this builder's wire format) for the given key. Emits the
    // key and any required separator through this builder's own state — i.e.
    // `kItem(key)` for a JSONB stream, or `,"key":` for a JSON-text stream —
    // then returns the underlying JsonWriter so the caller can stream the
    // value bytes directly, bypassing per-field virtual dispatch.
    //
    // Returns nullptr on builders that don't stream to a byte writer
    // (tree-mode backends), where this splice isn't possible.
    //
    // Used by note::experimental::body_template to splice a compile-time
    // pre-rendered object/array into the request body. Other callers should
    // not need this — the normal `add(...)` interface is the wire-format-
    // agnostic path.
    //
    // Default: nullptr. Override only on builders that stream to a writer.
    virtual JsonWriter* begin_raw_value(string_view key) { (void)key; return nullptr; }
};

class JsonReader {
public:
    virtual ~JsonReader() = default;

    virtual bool has(string_view key) const = 0;

    virtual bool get_bool(string_view key, bool def = false) const = 0;
    virtual json_int_t get_int(string_view key, json_int_t def = 0) const = 0;
    virtual double get_double(string_view key, double def = 0.0) const = 0;
    virtual string_view get_string(string_view key, string_view def = {}) const = 0;

    virtual std::unique_ptr<JsonReader> get_object(string_view key) const = 0;

    /// Read a JSON array of strings. Returns the number of elements read.
    /// Default: 0 (backends override to implement).
    virtual size_t get_string_array(string_view key, string_view* out, size_t max) const {
        (void)key; (void)out; (void)max;
        return 0;
    }

    /// Read a JSON array of objects. Populates out[] with readers for each element.
    /// Returns the number of elements read. Default: 0.
    virtual size_t get_object_array(string_view key,
                                     std::unique_ptr<JsonReader>* out, size_t max) const {
        (void)key; (void)out; (void)max;
        return 0;
    }

    virtual bool has_error() const = 0;
    virtual string_view get_error() const = 0;
};

// ---------------------------------------------------------------------------
// SaxToTextSink — JsonSink that re-serializes SAX events into JSON text.
//
// Bridges the SAX-events-in surface (start_response / finish_response on
// JsonBackend) into the text-shaped legacy parse path. Backends that want
// the default JsonBackend impl get tree-from-text indirectly via this
// sink — the response bytes never sit in the caller's buffer as wire
// bytes, they're assembled here as canonical JSON text from the
// transport-layer SAX events.
//
// Used by:
//   - JsonBackend's default start_response / finish_response (below).
//   - Any backend that prefers JSON-text-in over a direct tree-builder
//     sink (StaticJsonBackend, in particular — jsmn wants contiguous text).
//
// The buffer is supplied via rearm() — typically Notecard's rsp_buf().
// ---------------------------------------------------------------------------

namespace detail {

class SaxToTextSink : public JsonSink {
public:
    /// Begin a new response. `work_buf` is the destination for the
    /// serialized JSON text; it must outlive `view()` calls.
    void rearm(span<char> work_buf) {
        writer_ = JsonBufferWriter(work_buf.data(), work_buf.size());
        depth_ = 0;
        need_comma_ = false;
        opened_root_ = false;
    }

    /// JsonSink-level reset (SaxEvent::Reset). Clears the buffer but
    /// keeps the underlying span — callers reusing the sink across
    /// transactions should prefer rearm() with a fresh buffer.
    void reset() override {
        writer_.reset();
        depth_ = 0;
        need_comma_ = false;
        opened_root_ = false;
    }

    string_view view() const { return writer_.view(); }
    bool overflow() const { return writer_.overflow(); }

    void on_null(string_view k) override { emit_kv_prefix(k); writer_.write(string_view("null")); }
    void on_bool(string_view k, bool v) override {
        emit_kv_prefix(k);
        writer_.write(v ? string_view("true") : string_view("false"));
    }
    void on_int(string_view k, json_int_t v) override {
        emit_kv_prefix(k);
        char buf[24];
        size_t n = detail::itoa(buf, sizeof(buf), v);
        writer_.write(buf, n);
    }
    void on_float(string_view k, double v) override {
        emit_kv_prefix(k);
        char buf[32];
        size_t n = detail::dtoa_shortest(buf, sizeof(buf), v);
        writer_.write(buf, n);
    }
    void on_number(string_view k, string_view raw) override {
        // Lossless path used by some lexers when neither on_int nor
        // on_float is called. Pass through verbatim.
        emit_kv_prefix(k);
        writer_.write(raw);
    }
    void on_string(string_view k, string_view v) override {
        emit_kv_prefix(k);
        quoted(v);
    }
    void on_object_begin(string_view k) override {
        if (!opened_root_) {
            opened_root_ = true;
            writer_.write('{');
            push_frame(/*array=*/false);
            return;
        }
        emit_kv_prefix(k);
        writer_.write('{');
        push_frame(false);
    }
    void on_object_end(string_view) override {
        writer_.write('}');
        pop_frame();
    }
    void on_array_begin(string_view k) override {
        emit_kv_prefix(k);
        writer_.write('[');
        push_frame(/*array=*/true);
    }
    void on_array_end(string_view) override {
        writer_.write(']');
        pop_frame();
    }

private:
    JsonBufferWriter writer_{nullptr, 0};

    // Tracks "inside an array?" for each open container so emit_kv_prefix
    // knows whether to suppress the key prefix (array elements have no
    // keys, just values).
    static constexpr size_t kMaxDepth = 8;
    bool in_array_[kMaxDepth] = {};
    size_t depth_ = 0;
    bool need_comma_ = false;
    bool opened_root_ = false;

    bool in_array() const {
        return depth_ > 0 && in_array_[depth_ - 1];
    }

    void emit_kv_prefix(string_view key) {
        if (need_comma_) writer_.write(',');
        need_comma_ = true;
        if (!in_array()) {
            quoted(key);
            writer_.write(':');
        }
    }

    void push_frame(bool array) {
        if (depth_ < kMaxDepth) in_array_[depth_] = array;
        ++depth_;
        need_comma_ = false;
    }

    void pop_frame() {
        if (depth_ > 0) --depth_;
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
};

} // namespace detail

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

    // ── SAX-events-in surface ──────────────────────────────────────────
    //
    // start_response() returns a sink that consumes all SAX events for
    // one transaction. finish_response() finalizes and returns the
    // reader over the assembled tree. The reader is valid until the
    // next start_response() call.
    //
    // The two methods bracket one response. work_buf is scratch space
    // for backends that re-serialize SAX events to JSON text (default
    // impl + StaticJsonBackend). Tree-direct backends (cJSON, nlohmann)
    // ignore it.
    //
    // Default impl: route through SaxToTextSink, then call get_reader()
    // on the assembled JSON text. Backends that want a direct tree path
    // override both methods to skip the text round-trip.

    virtual JsonSink& start_response(span<char> work_buf) {
        rt_sink_.rearm(work_buf);
        return rt_sink_;
    }

    virtual JsonReader& finish_response() {
        return get_reader(rt_sink_.view());
    }

    /// Take ownership of the most recently assembled response tree.
    /// Used by the no-allocator error path: the returned reader keeps
    /// the err-message storage alive when the caller can't intern it
    /// into a StringPool. The backend's internal reader is reset; the
    /// next call to finish_response() will build a new one.
    ///
    /// Default impl re-parses the SaxToTextSink's text buffer (so the
    /// returned tree is independent of the backend's reusable storage).
    /// Direct-tree backends override to transfer the tree.
    virtual std::unique_ptr<JsonReader> release_response() {
        return parse_response(rt_sink_.view());
    }

private:
    std::unique_ptr<JsonBuilder> owned_builder_;
    std::unique_ptr<JsonReader> owned_reader_;
    detail::SaxToTextSink rt_sink_;
};

// ---------------------------------------------------------------------------
// StreamingJsonBuilder — JsonBuilder that writes through a JsonWriter.
//
// Same interface as StaticJsonBuilder, but each add() call writes directly
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
    using JsonBuilder::add;           // inherit integer widening template
    using JsonBuilder::add_element;   // inherit integer widening template

    explicit StreamingJsonBuilder(JsonWriter& w) : writer_(w) {
        writer_.write('{');
    }

    JsonBuilder& add(string_view key, bool value) override {
        kv(key);
        writer_.write(value ? string_view("true") : string_view("false"));
        return *this;
    }

    JsonBuilder& add(string_view key, json_int_t value) override {
        kv(key);
        char tmp[24];
        size_t len = detail::itoa(tmp, sizeof(tmp), value);
        writer_.write(tmp, len);
        return *this;
    }

    JsonBuilder& add(string_view key, double value) override {
        kv(key);
        char tmp[32];
        size_t len = detail::dtoa_shortest(tmp, sizeof(tmp), value);
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

    // Emit `[,]"key":` and hand back the writer so the caller can stream a
    // pre-rendered JSON-text value. kv() leaves need_comma_ = true, so the
    // next field is correctly comma-separated.
    JsonWriter* begin_raw_value(string_view key) override {
        kv(key);
        return &writer_;
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

    JsonBuilder& begin_element_object() override {
        comma();
        writer_.write('{');
        need_comma_ = false;
        return *this;
    }

    JsonBuilder& add_element(bool value) override {
        comma();
        writer_.write(value ? string_view("true") : string_view("false"));
        return *this;
    }

    JsonBuilder& add_element(json_int_t value) override {
        comma();
        char tmp[24];
        size_t len = detail::itoa(tmp, sizeof(tmp), value);
        writer_.write(tmp, len);
        return *this;
    }

    JsonBuilder& add_element(double value) override {
        comma();
        char tmp[32];
        size_t len = detail::dtoa_shortest(tmp, sizeof(tmp), value);
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
