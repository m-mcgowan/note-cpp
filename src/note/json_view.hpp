// note::JsonView — ergonomic wrapper around a `string_view` of JSON.
//
// Pairs with `note::JsonBuf` for the build/read split:
//   JsonBuf  — build a JSON request.
//   JsonView — scan fields out of a JSON response, no SAX parser.
//
// JsonView is a thin inline wrapper over `note::scan::*`; methods
// forward directly to the free functions and cost nothing extra
// once inlined. Calls chain for nested traversal:
//
//   note::JsonView resp(body_cstr);
//   float temp = resp.object("body").get("temp", 0.0f);
//
// For the underlying primitives and caveats, see json_scan.hpp.
#pragma once

#include <note/json_scan.hpp>
#include <note/types.hpp>

namespace note {

class JsonView {
public:
    constexpr JsonView() = default;
    constexpr JsonView(string_view sv) : sv_(sv) {}
    constexpr JsonView(const char* s, size_t n) : sv_(s, n) {}

    /// Unwrap an optional-like result (e.g. `Result<string_view>` from
    /// `transact_raw`, `std::optional<string_view>`, `expected<...>`).
    /// Error / disengaged values yield an empty view — which then
    /// propagates naturally through field lookups (missing key →
    /// default value), so callers can skip the `if (resp) ... *resp`
    /// boilerplate when they only need best-effort extraction.
    template<class Opt, class = std::enable_if_t<
        !std::is_convertible_v<const Opt&, string_view>
        && !std::is_same_v<std::decay_t<Opt>, JsonView>
        && std::is_convertible_v<decltype(*std::declval<const Opt&>()), string_view>>>
    constexpr JsonView(const Opt& opt)
        : sv_(static_cast<bool>(opt) ? string_view(*opt) : string_view{}) {}

    constexpr string_view view() const { return sv_; }
    constexpr bool empty() const { return sv_.empty(); }
    constexpr size_t size() const { return sv_.size(); }
    constexpr bool operator==(string_view other) const { return sv_ == other; }

    /// Return the raw JSON substring for `key`, or empty.
    constexpr string_view field(string_view key) const {
        return scan::field(sv_, key);
    }

    /// Object-valued field, wrapped in a `JsonView` for chaining.
    /// Returns an empty view if missing or not an object.
    constexpr JsonView object(string_view key) const {
        return JsonView{scan::object(sv_, key)};
    }

    /// Array-valued field as a raw string_view (`[...]`), or empty.
    constexpr string_view array(string_view key) const {
        return scan::array(sv_, key);
    }

    /// Typed extraction. `T` deduced from `def`. See scan::get for
    /// supported types and caveats.
    template<class T>
    constexpr T get(string_view key, T def) const {
        return scan::get<T>(sv_, key, def);
    }

    // Named variants — unambiguous, skip the template for readability.
    constexpr json_int_t  get_int   (string_view key, json_int_t def = 0)      const { return scan::get_int   (sv_, key, def); }
    constexpr double      get_double(string_view key, double def = 0.0)        const { return scan::get_double(sv_, key, def); }
    constexpr float       get_float (string_view key, float def = 0.0f)        const { return scan::get_float (sv_, key, def); }
    constexpr bool        get_bool  (string_view key, bool def = false)        const { return scan::get_bool  (sv_, key, def); }
    constexpr string_view get_str   (string_view key, string_view def = {})    const { return scan::get_str   (sv_, key, def); }

    /// Single-pass visitor over top-level pairs. See scan::for_each.
    template<class Visitor>
    constexpr void for_each(Visitor&& visitor) const {
        scan::for_each(sv_, static_cast<Visitor&&>(visitor));
    }

    /// Populate a NOTE_FIELDS struct. Strategy selected by tag
    /// (see scan::walk / scan::pick); default is walk (single pass).
    template<class T, class Tag = scan::walk_t>
    void into(T& obj, Tag tag = {}) const { scan::into(sv_, obj, tag); }

private:
    string_view sv_;
};

} // namespace note
