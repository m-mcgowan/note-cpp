// cJSON backend for note-cpp.
// Include this header when cJSON is available (bundled with note-c, ESP-IDF, etc.).
//
// Usage:
//   #include <note/backends/cjson.hpp>
//   note::backends::CjsonBackend backend;
//   note::Notecard nc(backend, transport);
#pragma once

// Allow users to pre-include cJSON before this header (e.g. from a
// non-standard path). If not already included, try to find it.
#ifndef CJSON_PUBLIC
#   if __has_include(<cJSON.h>)
#       include <cJSON.h>
#   elif __has_include("cJSON.h")
#       include "cJSON.h"
#   else
#       error "cJSON not found. Include cJSON.h before this header or add it to your include path."
#   endif
#endif

#include <note/json.hpp>

#include <memory>
#include <string>

namespace note::backends {

// ---------------------------------------------------------------------------
// CjsonBuilder: builds a JSON object using cJSON, serializes to string.
// ---------------------------------------------------------------------------
class CjsonBuilder : public JsonBuilder {
public:
    CjsonBuilder() : root_(cJSON_CreateObject()), stack_{root_} {}
    ~CjsonBuilder() override { if (root_) cJSON_Delete(root_); }

    CjsonBuilder(const CjsonBuilder&) = delete;
    CjsonBuilder& operator=(const CjsonBuilder&) = delete;

    CjsonBuilder& add(string_view key, bool value) override {
        cJSON_AddItemToObject(current(), zkey(key), cJSON_CreateBool(value));
        return *this;
    }
    CjsonBuilder& add(string_view key, int32_t value) override {
        cJSON_AddItemToObject(current(), zkey(key), cJSON_CreateNumber(value));
        return *this;
    }
    CjsonBuilder& add(string_view key, double value) override {
        cJSON_AddItemToObject(current(), zkey(key), cJSON_CreateNumber(value));
        return *this;
    }
    CjsonBuilder& add(string_view key, string_view value) override {
        auto* s = cJSON_CreateString(zstr(value));
        cJSON_AddItemToObject(current(), zkey(key), s);
        return *this;
    }
    CjsonBuilder& begin_object(string_view key) override {
        auto* obj = cJSON_CreateObject();
        cJSON_AddItemToObject(current(), zkey(key), obj);
        stack_.push_back(obj);
        return *this;
    }
    CjsonBuilder& end_object() override {
        if (stack_.size() > 1) stack_.pop_back();
        return *this;
    }
    CjsonBuilder& begin_array(string_view key) override {
        auto* arr = cJSON_CreateArray();
        cJSON_AddItemToObject(current(), zkey(key), arr);
        stack_.push_back(arr);
        return *this;
    }
    CjsonBuilder& end_array() override {
        if (stack_.size() > 1) stack_.pop_back();
        return *this;
    }
    std::string to_string() override {
        char* raw = cJSON_PrintUnformatted(root_);
        std::string result(raw);
        cJSON_free(raw);
        return result;
    }

private:
    cJSON* root_;
    std::vector<cJSON*> stack_;

    cJSON* current() { return stack_.back(); }

    // cJSON requires null-terminated strings. Since string_view may not be
    // null-terminated, we copy into a small buffer.
    thread_local static inline char key_buf_[256];
    thread_local static inline char str_buf_[4096];

    static const char* zkey(string_view sv) {
        auto n = sv.size() < sizeof(key_buf_) ? sv.size() : sizeof(key_buf_) - 1;
        sv.copy(key_buf_, n);
        key_buf_[n] = '\0';
        return key_buf_;
    }
    static const char* zstr(string_view sv) {
        auto n = sv.size() < sizeof(str_buf_) ? sv.size() : sizeof(str_buf_) - 1;
        sv.copy(str_buf_, n);
        str_buf_[n] = '\0';
        return str_buf_;
    }
};

// ---------------------------------------------------------------------------
// CjsonReader: reads fields from a parsed cJSON tree.
// ---------------------------------------------------------------------------
class CjsonReader : public JsonReader {
public:
    // Owning constructor — takes ownership of the parsed cJSON tree.
    explicit CjsonReader(cJSON* root) : root_(root), owned_(true) {}

    // Non-owning constructor — borrows a child node (must outlive this reader).
    CjsonReader(cJSON* node, bool /*non_owning*/) : root_(node), owned_(false) {}

    ~CjsonReader() override { if (owned_ && root_) cJSON_Delete(root_); }

    CjsonReader(const CjsonReader&) = delete;
    CjsonReader& operator=(const CjsonReader&) = delete;

    bool has(string_view key) const override {
        return cJSON_GetObjectItemCaseSensitive(root_, zkey(key)) != nullptr;
    }
    bool get_bool(string_view key, bool def) const override {
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, zkey(key));
        if (!item) return def;
        if (cJSON_IsBool(item)) return cJSON_IsTrue(item);
        return def;
    }
    int32_t get_int(string_view key, int32_t def) const override {
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, zkey(key));
        if (!item || !cJSON_IsNumber(item)) return def;
        return static_cast<int32_t>(item->valuedouble);
    }
    double get_double(string_view key, double def) const override {
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, zkey(key));
        if (!item || !cJSON_IsNumber(item)) return def;
        return item->valuedouble;
    }
    string_view get_string(string_view key, string_view def) const override {
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, zkey(key));
        if (!item || !cJSON_IsString(item)) return def;
        return string_view(item->valuestring);
    }
    std::unique_ptr<JsonReader> get_object(string_view key) const override {
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, zkey(key));
        if (!item || !cJSON_IsObject(item)) return nullptr;
        return std::unique_ptr<JsonReader>(new CjsonReader(item, false));
    }
    bool has_error() const override {
        return root_ == nullptr;
    }
    string_view get_error() const override {
        if (!root_) return "JSON parse error";
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, "err");
        if (item && cJSON_IsString(item)) return string_view(item->valuestring);
        return {};
    }

private:
    cJSON* root_;
    bool owned_;

    thread_local static inline char key_buf_[256];

    static const char* zkey(string_view sv) {
        auto n = sv.size() < sizeof(key_buf_) ? sv.size() : sizeof(key_buf_) - 1;
        sv.copy(key_buf_, n);
        key_buf_[n] = '\0';
        return key_buf_;
    }
};

// ---------------------------------------------------------------------------
// CjsonBackend: ties builder and reader together.
// ---------------------------------------------------------------------------
class CjsonBackend : public JsonBackend {
public:
    std::unique_ptr<JsonBuilder> create_builder() override {
        return std::make_unique<CjsonBuilder>();
    }
    std::unique_ptr<JsonReader> parse_response(string_view json) override {
        // cJSON_ParseWithLength handles non-null-terminated strings.
        cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
        return std::make_unique<CjsonReader>(root);
    }
};

} // namespace note::backends
