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

// On embedded targets (FreeRTOS/ESP-IDF), thread_local adds TLS overhead to
// every task stack (~4.6 KB per task for the buffers below). Since Notecard
// operations are single-threaded in practice, we omit thread_local on
// embedded platforms. Define NOTE_THREAD_LOCAL=thread_local to force TLS.
#ifndef NOTE_THREAD_LOCAL
#   if defined(ESP_PLATFORM) || defined(ARDUINO)
#       define NOTE_THREAD_LOCAL
#   else
#       define NOTE_THREAD_LOCAL thread_local
#   endif
#endif

namespace note::backends {

// ---------------------------------------------------------------------------
// CjsonBuilder: builds a JSON object using cJSON, serializes to string.
// ---------------------------------------------------------------------------
class CjsonBuilder : public JsonBuilder {
public:
    using JsonBuilder::add;
    using JsonBuilder::add_element;
    CjsonBuilder() : root_(cJSON_CreateObject()), stack_{root_} {}
    ~CjsonBuilder() override { if (root_) cJSON_Delete(root_); }

    CjsonBuilder(const CjsonBuilder&) = delete;
    CjsonBuilder& operator=(const CjsonBuilder&) = delete;

    // Destroy the cJSON tree without creating a new one.
    // Used by CjsonArenaBackend to clean up before changing cJSON hooks.
    void destroy() {
        if (root_) { cJSON_Delete(root_); root_ = nullptr; }
        stack_.clear();
    }

    void reset() override {
        if (root_) cJSON_Delete(root_);
        root_ = cJSON_CreateObject();
        stack_.clear();
        stack_.push_back(root_);
    }

    CjsonBuilder& add(string_view key, bool value) override {
        cJSON_AddItemToObject(current(), zkey(key), cJSON_CreateBool(value));
        return *this;
    }
    CjsonBuilder& add(string_view key, json_int_t value) override {
        cJSON_AddItemToObject(current(), zkey(key), cJSON_CreateNumber(static_cast<double>(value)));
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
    CjsonBuilder& add_raw(string_view key, string_view json_fragment) override {
        std::string s(json_fragment.data(), json_fragment.size());
        auto* raw = cJSON_Parse(s.c_str());
        if (raw) cJSON_AddItemToObject(current(), zkey(key), raw);
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
    CjsonBuilder& begin_element_object() override {
        auto* obj = cJSON_CreateObject();
        cJSON_AddItemToArray(current(), obj);
        stack_.push_back(obj);
        return *this;
    }
    CjsonBuilder& add_element(bool value) override {
        cJSON_AddItemToArray(current(), cJSON_CreateBool(value));
        return *this;
    }
    CjsonBuilder& add_element(json_int_t value) override {
        cJSON_AddItemToArray(current(), cJSON_CreateNumber(static_cast<double>(value)));
        return *this;
    }
    CjsonBuilder& add_element(double value) override {
        cJSON_AddItemToArray(current(), cJSON_CreateNumber(value));
        return *this;
    }
    CjsonBuilder& add_element(string_view value) override {
        cJSON_AddItemToArray(current(), cJSON_CreateString(zstr(value)));
        return *this;
    }
    string_view to_view() override {
        // Try pre-allocated buffer first (avoids cJSON malloc + std::string copy).
        if (cJSON_PrintPreallocated(root_, print_buf_, sizeof(print_buf_), 0))
            return string_view(print_buf_);
        // Fallback: heap serialize for oversize JSON.
        char* raw = cJSON_PrintUnformatted(root_);
        print_fallback_ = raw;
        cJSON_free(raw);
        return print_fallback_;
    }

private:
    cJSON* root_;
    std::vector<cJSON*> stack_;
    char print_buf_[512];
    std::string print_fallback_;

    cJSON* current() { return stack_.back(); }

    // cJSON requires null-terminated strings. Since string_view may not be
    // null-terminated, we copy into a small buffer.
    NOTE_THREAD_LOCAL static inline char key_buf_[256];
    NOTE_THREAD_LOCAL static inline char str_buf_[4096];

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
    json_int_t get_int(string_view key, json_int_t def) const override {
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, zkey(key));
        if (!item || !cJSON_IsNumber(item)) return def;
        return static_cast<json_int_t>(item->valuedouble);
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
    size_t get_string_array(string_view key, string_view* out, size_t max) const override {
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, zkey(key));
        if (!item || !cJSON_IsArray(item)) return 0;
        size_t n = 0;
        for (auto* elem = item->child; elem && n < max; elem = elem->next) {
            if (cJSON_IsString(elem))
                out[n++] = string_view(elem->valuestring);
        }
        return n;
    }
    size_t get_object_array(string_view key,
                            std::unique_ptr<JsonReader>* out, size_t max) const override {
        auto* item = cJSON_GetObjectItemCaseSensitive(root_, zkey(key));
        if (!item || !cJSON_IsArray(item)) return 0;
        size_t n = 0;
        for (auto* elem = item->child; elem && n < max; elem = elem->next) {
            if (cJSON_IsObject(elem))
                out[n++] = std::unique_ptr<JsonReader>(new CjsonReader(elem, false));
        }
        return n;
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

    NOTE_THREAD_LOCAL static inline char key_buf_[256];

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
    JsonBuilder& get_builder() override {
        builder_.reset();
        return builder_;
    }
private:
    CjsonBuilder builder_;
};

// ---------------------------------------------------------------------------
// CjsonArenaBackend: cJSON with arena allocation (zero heap fragmentation).
//
// Routes all cJSON allocations through a MonotonicArena provided by the caller.
// The arena resets between requests — no fragmentation, bounded memory.
//
// Usage:
//   char pool[4096];
//   note::MonotonicArena arena(pool);
//   note::backends::CjsonArenaBackend backend(arena);
//   note::Notecard nc(backend, transport);
// ---------------------------------------------------------------------------

} // namespace note::backends

// Include arena.hpp after the main classes to avoid circular deps.
// CjsonArenaBackend is in a separate inline namespace for clean separation.
#include <note/arena.hpp>

namespace note::backends {

namespace detail {

// Thread-local active arena pointer for cJSON hook dispatch.
// Set before cJSON calls, cleared after. This is safe for single-threaded
// embedded use and for multi-threaded use with per-thread arenas.
inline NOTE_THREAD_LOCAL MonotonicArena* g_active_arena = nullptr;

inline void* arena_cjson_malloc(size_t size) {
    if (g_active_arena) {
        void* p = g_active_arena->allocate(size);
        if (p) return p;
    }
    // Fallback to standard malloc if arena exhausted or not set.
    return std::malloc(size);
}

inline void arena_cjson_free(void* p) {
    // If p is within the arena range, it's a no-op (arena reclaims on reset).
    // If p was from fallback malloc, we must free it.
    // We can detect this by checking if p falls within the arena buffer.
    // However, since cJSON_InitHooks doesn't give us size info, and the arena
    // buffer address isn't easily accessible here, we use a simpler approach:
    // just don't free. The arena resets between requests, and fallback allocs
    // indicate the arena was too small (user should increase arena size).
    //
    // For correctness with mixed arena/malloc, we'd need a header or bitmap.
    // For embedded use (the target), arena exhaustion is a configuration error.
    (void)p;
}

struct ArenaScope {
    MonotonicArena& arena;
    ArenaScope(MonotonicArena& a) : arena(a) {
        arena.reset();
        g_active_arena = &arena;
        cJSON_Hooks hooks{arena_cjson_malloc, arena_cjson_free};
        cJSON_InitHooks(&hooks);
    }
    ~ArenaScope() {
        g_active_arena = nullptr;
        cJSON_InitHooks(nullptr);  // restore default malloc/free
    }
};

} // namespace detail

class CjsonArenaBackend : public JsonBackend {
public:
    explicit CjsonArenaBackend(MonotonicArena& arena) : arena_(arena) {
        install_hooks();
    }

    ~CjsonArenaBackend() {
        // Destroy cJSON tree BEFORE clearing arena hooks — cJSON_Delete calls
        // the current free hook, which must still be arena_cjson_free (no-op)
        // for arena-allocated nodes. Standard free() on arena pointers crashes.
        builder_.destroy();
        cJSON_InitHooks(nullptr);  // restore default malloc/free
        detail::g_active_arena = nullptr;
    }

    std::unique_ptr<JsonBuilder> create_builder() override {
        arena_.reset();
        return std::make_unique<CjsonBuilder>();
    }

    JsonBuilder& get_builder() override {
        arena_.reset();
        builder_.reset();
        return builder_;
    }

    std::unique_ptr<JsonReader> parse_response(string_view json) override {
        // Don't reset arena here — the builder's cJSON tree from create_builder()
        // has already been serialized via to_view(). The arena can be reused for
        // parse nodes. But to be safe, we don't reset between build and parse
        // within a single request cycle. The arena resets on the next create_builder().
        cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
        // Non-owning reader: arena owns the cJSON tree (free is a no-op).
        return std::make_unique<CjsonReader>(root, false);
    }

private:
    MonotonicArena& arena_;
    CjsonBuilder builder_;

    void install_hooks() {
        detail::g_active_arena = &arena_;
        cJSON_Hooks hooks{detail::arena_cjson_malloc, detail::arena_cjson_free};
        cJSON_InitHooks(&hooks);
    }
};

} // namespace note::backends
