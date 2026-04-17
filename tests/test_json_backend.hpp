// Test JSON backend: builds JSON strings for wire format verification.
// No external dependencies — just std::string manipulation.
#pragma once

#include <note/json.hpp>
#include <note/types.hpp>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace note::test {

// ---------------------------------------------------------------------------
// TestJsonBuilder: builds a JSON string in insertion order, no whitespace.
// ---------------------------------------------------------------------------
class TestJsonBuilder : public JsonBuilder {
public:
    using JsonBuilder::add;
    using JsonBuilder::add_element;

    TestJsonBuilder() { buf_ = "{"; needs_comma_.push_back(false); }

    TestJsonBuilder& add(string_view k, bool v) override {
        key(k); buf_ += v ? "true" : "false"; return *this;
    }
    TestJsonBuilder& add(string_view k, json_int_t v) override {
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
    TestJsonBuilder& add_raw(string_view k, string_view json) override {
        key(k); buf_.append(json.data(), json.size()); return *this;
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
    TestJsonBuilder& add_element(bool v) override {
        comma(); buf_ += v ? "true" : "false"; return *this;
    }
    TestJsonBuilder& add_element(json_int_t v) override {
        comma(); buf_ += std::to_string(v); return *this;
    }
    TestJsonBuilder& add_element(double v) override {
        comma(); buf_ += std::to_string(v); return *this;
    }
    TestJsonBuilder& add_element(string_view v) override {
        comma(); buf_ += '"'; escape_string(v); buf_ += '"'; return *this;
    }
    string_view to_view() override {
        if (!closed_) { buf_ += '}'; closed_ = true; }
        return buf_;
    }

    void reset() override {
        buf_.clear();
        buf_ = "{";
        needs_comma_.clear();
        needs_comma_.push_back(false);
        closed_ = false;
    }

private:
    std::string buf_;
    std::vector<bool> needs_comma_;
    bool closed_ = false;

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
    json_int_t get_int(string_view, json_int_t def) const override { return def; }
    double get_double(string_view, double def) const override { return def; }
    string_view get_string(string_view, string_view def) const override { return def; }
    std::unique_ptr<JsonReader> get_object(string_view) const override { return nullptr; }
    bool has_error() const override { return false; }
    string_view get_error() const override { return {}; }
};

// ---------------------------------------------------------------------------
// PopulatedJsonReader: a reader pre-populated with values for testing
// response parsing, including nested body objects.
// ---------------------------------------------------------------------------
class PopulatedJsonReader : public JsonReader {
public:
    using Value = std::variant<bool, note::json_int_t, double, std::string>;

    void set(const std::string& key, bool v) { values_[key] = v; }
    void set(const std::string& key, note::json_int_t v) { values_[key] = v; }
    // Widen narrower integer types to json_int_t (prevents int32_t → bool ambiguity).
    template<typename T, std::enable_if_t<
        std::is_integral_v<T> && !std::is_same_v<T, bool> &&
        !std::is_same_v<T, note::json_int_t>, int> = 0>
    void set(const std::string& key, T v) { values_[key] = static_cast<note::json_int_t>(v); }
    void set(const std::string& key, double v) { values_[key] = v; }
    void set(const std::string& key, const std::string& v) { values_[key] = v; }

    void set_object(const std::string& key, std::unique_ptr<PopulatedJsonReader> obj) {
        objects_[key] = std::move(obj);
    }

    void set_array(const std::string& key, std::vector<std::string> arr) {
        arrays_[key] = std::move(arr);
    }

    bool has(string_view k) const override {
        auto key = std::string(k);
        return values_.count(key) || objects_.count(key) || arrays_.count(key);
    }
    bool get_bool(string_view k, bool def) const override {
        auto it = values_.find(std::string(k));
        if (it != values_.end() && std::holds_alternative<bool>(it->second))
            return std::get<bool>(it->second);
        return def;
    }
    json_int_t get_int(string_view k, json_int_t def) const override {
        auto it = values_.find(std::string(k));
        if (it != values_.end() && std::holds_alternative<note::json_int_t>(it->second))
            return std::get<note::json_int_t>(it->second);
        return def;
    }
    double get_double(string_view k, double def) const override {
        auto it = values_.find(std::string(k));
        if (it != values_.end() && std::holds_alternative<double>(it->second))
            return std::get<double>(it->second);
        return def;
    }
    string_view get_string(string_view k, string_view def) const override {
        auto it = values_.find(std::string(k));
        if (it != values_.end() && std::holds_alternative<std::string>(it->second))
            return string_view(std::get<std::string>(it->second));
        return def;
    }
    std::unique_ptr<JsonReader> get_object(string_view k) const override {
        auto it = objects_.find(std::string(k));
        if (it != objects_.end() && it->second) {
            // Clone the populated reader for the caller
            auto clone = std::make_unique<PopulatedJsonReader>();
            clone->values_ = it->second->values_;
            // Deep-clone nested objects
            for (auto& [key, obj] : it->second->objects_) {
                if (obj) {
                    auto sub = std::make_unique<PopulatedJsonReader>();
                    sub->values_ = obj->values_;
                    clone->objects_[key] = std::move(sub);
                }
            }
            return clone;
        }
        return nullptr;
    }
    size_t get_string_array(string_view k, string_view* out, size_t max) const override {
        auto it = arrays_.find(std::string(k));
        if (it == arrays_.end()) return 0;
        size_t n = std::min(max, it->second.size());
        for (size_t i = 0; i < n; ++i)
            out[i] = string_view(it->second[i]);
        return n;
    }

    bool has_error() const override { return false; }
    string_view get_error() const override { return {}; }

private:
    std::map<std::string, Value> values_;
    std::map<std::string, std::unique_ptr<PopulatedJsonReader>> objects_;
    std::map<std::string, std::vector<std::string>> arrays_;
};

// ---------------------------------------------------------------------------
// TestJsonBackend: ties builder and reader together.
// ---------------------------------------------------------------------------
class TestJsonBackend : public JsonBackend {
public:
    std::unique_ptr<JsonBuilder> create_builder() override {
        return std::make_unique<TestJsonBuilder>();
    }
    std::unique_ptr<JsonReader> parse_response(string_view) override {
        return std::make_unique<TestJsonReader>();
    }
};

// ---------------------------------------------------------------------------
// ErrorJsonReader: a reader that reports a Notecard error ("err" field).
// Used to exercise the ApiResult<Rsp>(ErrorInfo) error constructors.
// ---------------------------------------------------------------------------
class ErrorJsonReader : public JsonReader {
public:
    explicit ErrorJsonReader(std::string err) : err_(std::move(err)) {}

    bool has(string_view) const override { return false; }
    bool get_bool(string_view, bool def) const override { return def; }
    json_int_t get_int(string_view, json_int_t def) const override { return def; }
    double get_double(string_view, double def) const override { return def; }
    string_view get_string(string_view, string_view def) const override { return def; }
    std::unique_ptr<JsonReader> get_object(string_view) const override { return nullptr; }
    bool has_error() const override { return false; }
    string_view get_error() const override { return err_; }

private:
    std::string err_;
};

// ---------------------------------------------------------------------------
// ErrorJsonBackend: backend whose reader always reports a Notecard error.
// ---------------------------------------------------------------------------
class ErrorJsonBackend : public JsonBackend {
public:
    std::unique_ptr<JsonBuilder> create_builder() override {
        return std::make_unique<TestJsonBuilder>();
    }
    std::unique_ptr<JsonReader> parse_response(string_view) override {
        return std::make_unique<ErrorJsonReader>("test error");
    }
};

} // namespace note::test
