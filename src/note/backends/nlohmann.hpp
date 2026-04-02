// nlohmann-json backend for note-cpp.
// Include this header when nlohmann/json.hpp is available.
//
// Usage:
//   #include <note/backends/nlohmann.hpp>
//   note::backends::NlohmannBackend backend;
//   note::Notecard nc(backend, transport);
#pragma once

#if __has_include(<nlohmann/json.hpp>)
#   include <nlohmann/json.hpp>
#else
#   error "nlohmann/json.hpp not found. Install nlohmann-json or add it to your include path."
#endif

#include <note/json.hpp>

#include <memory>
#include <string>

namespace note::backends {

// ---------------------------------------------------------------------------
// NlohmannBuilder: builds a JSON object using nlohmann::json.
// ---------------------------------------------------------------------------
class NlohmannBuilder : public JsonBuilder {
public:
    NlohmannBuilder() { stack_.push_back(&root_); }

    NlohmannBuilder& add(string_view key, bool value) override {
        (*current())[std::string(key)] = value;
        return *this;
    }
    NlohmannBuilder& add(string_view key, int32_t value) override {
        (*current())[std::string(key)] = value;
        return *this;
    }
    NlohmannBuilder& add(string_view key, double value) override {
        (*current())[std::string(key)] = value;
        return *this;
    }
    NlohmannBuilder& add(string_view key, string_view value) override {
        (*current())[std::string(key)] = std::string(value);
        return *this;
    }
    NlohmannBuilder& add_raw(string_view key, string_view json_fragment) override {
        (*current())[std::string(key)] = nlohmann::json::parse(
            json_fragment.data(), json_fragment.data() + json_fragment.size(),
            nullptr, false);
        return *this;
    }
    NlohmannBuilder& begin_object(string_view key) override {
        auto k = std::string(key);
        (*current())[k] = nlohmann::json::object();
        stack_.push_back(&(*current())[k]);
        return *this;
    }
    NlohmannBuilder& end_object() override {
        if (stack_.size() > 1) stack_.pop_back();
        return *this;
    }
    NlohmannBuilder& begin_array(string_view key) override {
        auto k = std::string(key);
        (*current())[k] = nlohmann::json::array();
        stack_.push_back(&(*current())[k]);
        return *this;
    }
    NlohmannBuilder& end_array() override {
        if (stack_.size() > 1) stack_.pop_back();
        return *this;
    }
    NlohmannBuilder& add_element(bool value) override {
        current()->push_back(value); return *this;
    }
    NlohmannBuilder& add_element(int32_t value) override {
        current()->push_back(value); return *this;
    }
    NlohmannBuilder& add_element(double value) override {
        current()->push_back(value); return *this;
    }
    NlohmannBuilder& add_element(string_view value) override {
        current()->push_back(std::string(value)); return *this;
    }
    string_view to_view() override {
        view_cache_ = root_.dump();
        return view_cache_;
    }
    void reset() override {
        root_ = nlohmann::json::object();
        stack_.clear();
        stack_.push_back(&root_);
    }

private:
    nlohmann::json root_ = nlohmann::json::object();
    std::vector<nlohmann::json*> stack_;
    std::string view_cache_;  // cache for to_view() — nlohmann::dump() returns std::string

    nlohmann::json* current() { return stack_.back(); }
};

// ---------------------------------------------------------------------------
// NlohmannReader: reads fields from a parsed nlohmann::json object.
// ---------------------------------------------------------------------------
class NlohmannReader : public JsonReader {
public:
    explicit NlohmannReader(nlohmann::json json) : json_(std::move(json)) {}

    bool has(string_view key) const override {
        return json_.contains(std::string(key));
    }
    bool get_bool(string_view key, bool def) const override {
        auto it = json_.find(std::string(key));
        if (it == json_.end() || !it->is_boolean()) return def;
        return it->get<bool>();
    }
    int32_t get_int(string_view key, int32_t def) const override {
        auto it = json_.find(std::string(key));
        if (it == json_.end() || !it->is_number()) return def;
        return it->get<int32_t>();
    }
    double get_double(string_view key, double def) const override {
        auto it = json_.find(std::string(key));
        if (it == json_.end() || !it->is_number()) return def;
        return it->get<double>();
    }
    string_view get_string(string_view key, string_view def) const override {
        auto it = json_.find(std::string(key));
        if (it == json_.end() || !it->is_string()) return def;
        // Cache the string so the returned string_view remains valid.
        cached_string_ = it->get<std::string>();
        return cached_string_;
    }
    size_t get_string_array(string_view key, string_view* out, size_t max) const override {
        auto it = json_.find(std::string(key));
        if (it == json_.end() || !it->is_array()) return 0;
        size_t n = 0;
        cached_array_.clear();
        for (auto& elem : *it) {
            if (n >= max) break;
            if (elem.is_string()) {
                cached_array_.push_back(elem.get<std::string>());
                out[n++] = cached_array_.back();
            }
        }
        return n;
    }
    std::unique_ptr<JsonReader> get_object(string_view key) const override {
        auto it = json_.find(std::string(key));
        if (it == json_.end() || !it->is_object()) return nullptr;
        return std::make_unique<NlohmannReader>(*it);
    }
    bool has_error() const override {
        return json_.is_null() || json_.is_discarded();
    }
    string_view get_error() const override {
        if (json_.is_null() || json_.is_discarded()) return "JSON parse error";
        auto it = json_.find("err");
        if (it != json_.end() && it->is_string()) {
            cached_string_ = it->get<std::string>();
            return cached_string_;
        }
        return {};
    }

private:
    nlohmann::json json_;
    mutable std::string cached_string_;
    mutable std::vector<std::string> cached_array_;
};

// ---------------------------------------------------------------------------
// NlohmannBackend: ties builder and reader together.
// ---------------------------------------------------------------------------
class NlohmannBackend : public JsonBackend {
public:
    std::unique_ptr<JsonBuilder> create_builder() override {
        return std::make_unique<NlohmannBuilder>();
    }
    std::unique_ptr<JsonReader> parse_response(string_view json) override {
        auto parsed = nlohmann::json::parse(json, nullptr, false);
        return std::make_unique<NlohmannReader>(std::move(parsed));
    }
    JsonBuilder& get_builder() override {
        builder_.reset();
        return builder_;
    }
private:
    NlohmannBuilder builder_;
};

} // namespace note::backends
