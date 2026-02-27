#pragma once

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
using DynValue = std::variant<std::monostate, bool, int32_t, double, note::string_view>;

namespace detail {

/// Setter that writes a DynValue into a typed Field<T>.
/// Type mismatch silently no-ops — wrong type for a known key.
template<typename T>
void set_typed_field(void* ptr, DynValue val) {
    auto* f = static_cast<Field<T>*>(ptr);
    if (auto* v = std::get_if<T>(&val))
        *f = *v;
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

/// Create a DynField pointing to an extras DynValue slot.
inline DynField dyn_field_for(DynValue& slot) {
    return {&slot, detail::set_dyn_slot};
}

} // namespace note
