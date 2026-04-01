#pragma once

/// Define NOTE_EXTRAS=0 to disable extra()/operator[] on requests.
/// Saves ~24 bytes .data (variant visitor tables) and eliminates
/// std::variant/std::array from each request type.
#ifndef NOTE_EXTRAS
#define NOTE_EXTRAS 1
#endif

#if NOTE_EXTRAS

#include <array>
#include <cstdint>
#include <variant>
#include <note/field.hpp>
#include <note/types.hpp>

/// Maximum number of undocumented extra fields per request.
/// Override before including any note/api headers: #define NOTE_EXTRAS_MAX 8
#ifndef NOTE_EXTRAS_MAX
#define NOTE_EXTRAS_MAX 4
#endif

namespace note {

/// Type-erased value for extra() fields and operator[] routing.
/// std::monostate is intentionally excluded: extras slots are only accessed
/// up to extras_count_, so uninitialized slots are never visited. Excluding
/// monostate avoids a no-op lambda instantiation per-endpoint in build().
using DynValue = std::variant<bool, int32_t, double, note::string_view>;

namespace detail {

/// Setter that writes a DynValue into a typed Field<T>.
/// Type mismatch silently no-ops — wrong type for a known key.
/// For unit types (Minutes, Seconds, Milliseconds) that are constructible
/// from int32_t, extracts the int32_t from the variant and wraps it.
template<typename T>
void set_typed_field(void* ptr, DynValue val) {
    auto* f = static_cast<Field<T>*>(ptr);
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int32_t>
               || std::is_same_v<T, double> || std::is_same_v<T, note::string_view>) {
        if (auto* v = std::get_if<T>(&val))
            *f = *v;
    } else if constexpr (std::is_constructible_v<T, int32_t>) {
        if (auto* v = std::get_if<int32_t>(&val))
            *f = T{*v};
    }
}

/// Setter that stores a DynValue directly into an extras slot.
inline void set_dyn_slot(void* ptr, DynValue val) {
    *static_cast<DynValue*>(ptr) = std::move(val);
}

/// One entry in a request struct's extras buffer.
struct ExtraSlot {
    note::string_view key;
    DynValue value;
};

} // namespace detail

/// Proxy returned by operator[] for dynamic field access.
///
/// For known keys, writes through to the typed Field<T>.
/// For unknown keys, writes into the extras buffer slot.
///
/// Use as a temporary at the assignment site only.
struct DynField {
    using SetterFn = void(*)(void*, DynValue);

    void*    target_ = nullptr;
    SetterFn setter_ = nullptr;

    DynField() = default;
    DynField(void* target, SetterFn setter) : target_(target), setter_(setter) {}

    DynField& operator=(bool v)               { apply(DynValue{v}); return *this; }
    DynField& operator=(int32_t v)            { apply(DynValue{v}); return *this; }
    DynField& operator=(double v)             { apply(DynValue{v}); return *this; }
    DynField& operator=(note::string_view v)  { apply(DynValue{v}); return *this; }
    DynField& operator=(const char* v)        { apply(DynValue{note::string_view{v}}); return *this; }

private:
    void apply(DynValue v) { if (setter_) setter_(target_, std::move(v)); }
};

/// Create a DynField pointing to a typed Field<T>.
template<typename T>
DynField dyn_field_for(Field<T>& f) {
    return {&f, detail::set_typed_field<T>};
}

/// Create a DynField pointing to a plain (non-optional) required field.
template<typename T>
void set_plain_field(void* ptr, DynValue val) {
    auto* f = static_cast<T*>(ptr);
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int32_t>
               || std::is_same_v<T, double> || std::is_same_v<T, note::string_view>) {
        if (auto* v = std::get_if<T>(&val))
            *f = *v;
    } else if constexpr (std::is_constructible_v<T, int32_t>) {
        if (auto* v = std::get_if<int32_t>(&val))
            *f = T{*v};
    }
}

inline DynField dyn_field_for(bool& f)              { return {&f, set_plain_field<bool>}; }
inline DynField dyn_field_for(int32_t& f)           { return {&f, set_plain_field<int32_t>}; }
inline DynField dyn_field_for(double& f)            { return {&f, set_plain_field<double>}; }
inline DynField dyn_field_for(note::string_view& f) { return {&f, set_plain_field<note::string_view>}; }

/// Create a DynField pointing to an extras DynValue slot.
inline DynField dyn_field_for(DynValue& slot) {
    return {&slot, detail::set_dyn_slot};
}

} // namespace note

#endif // NOTE_EXTRAS
