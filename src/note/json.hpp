#pragma once

#include "json_sax.hpp"
#include "types.hpp"

namespace note {

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

    // Finalize and return the built JSON as a string.
    virtual std::string to_string() = 0;

    // Finalize and return a view into an internal buffer.
    // The view is valid until the next call to reset(), to_string(), or to_view().
    // Default implementation calls to_string() and caches in a member.
    // Backends may override to serialize into a pre-allocated buffer (zero-alloc).
    virtual string_view to_view() {
        view_buf_ = to_string();
        return view_buf_;
    }

    // Reset builder state for reuse (avoid re-allocating the builder object).
    // Default implementation is a no-op; backends override to clear internal state.
    virtual void reset() {}

private:
    std::string view_buf_;
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

} // namespace note
