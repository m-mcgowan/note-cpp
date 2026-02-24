#pragma once

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

    // Release ownership of the built JSON object.
    // The returned handle is owned by the caller and must be passed
    // to NotecardIO (which takes ownership) or freed via JsonBackend.
    virtual json_handle release() = 0;
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
    virtual std::unique_ptr<JsonReader> wrap_response(json_handle raw) = 0;
    virtual void free_response(json_handle raw) = 0;
};

} // namespace note
