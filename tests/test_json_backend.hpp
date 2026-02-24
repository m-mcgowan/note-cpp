// Test JSON backend: builds JSON strings for wire format verification.
// No external dependencies — just std::string manipulation.
#pragma once

#include <note/json.hpp>
#include <note/io.hpp>
#include <note/types.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace note::test {

// ---------------------------------------------------------------------------
// TestJsonBuilder: builds a JSON string in insertion order, no whitespace.
// json_handle is a heap-allocated std::string*.
// ---------------------------------------------------------------------------
class TestJsonBuilder : public JsonBuilder {
public:
    TestJsonBuilder() { buf_ = "{"; needs_comma_.push_back(false); }

    TestJsonBuilder& add(string_view k, bool v) override {
        key(k); buf_ += v ? "true" : "false"; return *this;
    }
    TestJsonBuilder& add(string_view k, int32_t v) override {
        key(k); buf_ += std::to_string(v); return *this;
    }
    TestJsonBuilder& add(string_view k, double v) override {
        key(k);
        // Use shortest representation that round-trips correctly
        char tmp[32];
        for (int prec = 1; prec <= 17; ++prec) {
            auto n = std::snprintf(tmp, sizeof(tmp), "%.*g", prec, v);
            double check;
            std::sscanf(tmp, "%lf", &check);
            if (check == v) {
                buf_.append(tmp, static_cast<size_t>(n));
                return *this;
            }
        }
        auto n = std::snprintf(tmp, sizeof(tmp), "%.17g", v);
        buf_.append(tmp, static_cast<size_t>(n));
        return *this;
    }
    TestJsonBuilder& add(string_view k, string_view v) override {
        key(k); buf_ += '"'; escape_string(v); buf_ += '"'; return *this;
    }
    TestJsonBuilder& begin_object(string_view k) override {
        key(k); buf_ += '{'; needs_comma_.push_back(false); return *this;
    }
    TestJsonBuilder& end_object() override {
        needs_comma_.pop_back(); buf_ += '}'; return *this;
    }
    TestJsonBuilder& begin_array(string_view k) override {
        key(k); buf_ += '['; needs_comma_.push_back(false); return *this;
    }
    TestJsonBuilder& end_array() override {
        needs_comma_.pop_back(); buf_ += ']'; return *this;
    }
    json_handle release() override {
        buf_ += '}';
        return new std::string(std::move(buf_));
    }

private:
    std::string buf_;
    std::vector<bool> needs_comma_;

    void comma() { if (needs_comma_.back()) buf_ += ','; needs_comma_.back() = true; }
    void key(string_view k) { comma(); buf_ += '"'; buf_ += k; buf_ += "\":"; }
    void escape_string(string_view s) {
        for (char c : s) {
            switch (c) {
                case '"':  buf_ += "\\\""; break;
                case '\\': buf_ += "\\\\"; break;
                case '\n': buf_ += "\\n"; break;
                case '\r': buf_ += "\\r"; break;
                case '\t': buf_ += "\\t"; break;
                default:   buf_ += c; break;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// TestJsonReader: wraps a stored std::string (not a real parser).
// For response tests, we pre-populate field values.
// ---------------------------------------------------------------------------
class TestJsonReader : public JsonReader {
public:
    bool has(string_view) const override { return false; }
    bool get_bool(string_view, bool def) const override { return def; }
    int32_t get_int(string_view, int32_t def) const override { return def; }
    double get_double(string_view, double def) const override { return def; }
    string_view get_string(string_view, string_view def) const override { return def; }
    std::unique_ptr<JsonReader> get_object(string_view) const override { return nullptr; }
    bool has_error() const override { return false; }
    string_view get_error() const override { return {}; }
};

// ---------------------------------------------------------------------------
// TestJsonBackend: ties builder and reader together.
// ---------------------------------------------------------------------------
class TestJsonBackend : public JsonBackend {
public:
    std::unique_ptr<JsonBuilder> create_builder() override {
        return std::make_unique<TestJsonBuilder>();
    }
    std::unique_ptr<JsonReader> wrap_response(json_handle h) override {
        // We don't parse — just return a default reader.
        // The captured JSON string is checked via CapturingIO.
        auto* s = static_cast<std::string*>(h);
        delete s;
        return std::make_unique<TestJsonReader>();
    }
    void free_response(json_handle h) override {
        delete static_cast<std::string*>(h);
    }
};

// ---------------------------------------------------------------------------
// CapturingIO: records the JSON string from each request for assertion.
// Returns a canned empty-object response.
// ---------------------------------------------------------------------------
class CapturingIO : public NotecardIO {
public:
    std::string last_request;

    Result<json_handle> request_response(json_handle req, uint32_t) override {
        auto* s = static_cast<std::string*>(req);
        last_request = *s;
        delete s;
        // Return an empty JSON object as response
        return static_cast<json_handle>(new std::string("{}"));
    }
    Result<void> send(json_handle req) override {
        auto* s = static_cast<std::string*>(req);
        last_request = *s;
        delete s;
        return {};
    }
    Result<void> binary_transmit(const uint8_t*, uint32_t, uint32_t) override {
        return {};
    }
    Result<uint32_t> binary_receive(uint8_t*, uint32_t) override {
        return 0u;
    }
    Result<void> binary_reset() override {
        return {};
    }
};

} // namespace note::test
