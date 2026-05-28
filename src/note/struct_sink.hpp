// StructSink<T> — SAX-to-struct dispatcher for streaming body parse.
//
// Dispatches SAX events directly into struct fields using compile-time
// reflection (C++20) or NOTE_FIELDS macro (C++17). Primitives are
// assigned directly (zero arena cost). Strings are interned into the
// StringPool.
//
// Supports nested aggregates (NOTE_FIELDS or C++20 reflectable) via
// child dispatch, and fixed-size arrays (std::array<T,N>) of both
// primitives and structs.
//
// Architecture: StructSinkCore<T> holds all dispatch logic and a pointer
// to shared workspace (for child sinks). StructSink<T> inherits from it
// and embeds the workspace buffer. Children are StructSinkCore instances
// placed into the shared workspace, avoiding recursive size inflation.
//
// Usage:
//   SensorData data{};
//   note::StructSink<SensorData> sink(data, pool);
//   sink.on_float("temperature", 22.5);   // data.temperature = 22.5f
//   sink.on_string("label", "room-42");   // data.label = pool.intern(...)
#pragma once

#include "body.hpp"
#include "body_handler.hpp"
#include "generic_sink.hpp"
#include "json_sax.hpp"
#include "string_pool.hpp"
#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace note {

// Forward declarations.
template<typename T>
struct StructSinkCore;
template<typename T, std::size_t MaxDepth = 4>
struct StructSink;

namespace detail {

// Array + aggregate traits live in body.hpp (shared with ser/template
// paths). Keep struct_sink.hpp focused on SAX dispatch.

// ── Aggregate detection trait ─────────────────────────────────────────
// Detects types usable as nested struct fields: NOTE_FIELDS-annotated
// (C++17) or C++20 reflectable aggregates. Excludes std::array.

template<typename T, typename = void>
struct is_sax_aggregate : std::false_type {};

template<typename T>
struct is_sax_aggregate<T, std::enable_if_t<has_note_fields_trait<T>::value>>
    : std::true_type {};

#if __cplusplus >= 202002L
template<typename T>
    requires (ReflectableAggregate<T> && !has_note_fields_trait<T>::value
              && !is_std_array<T>::value)
struct is_sax_aggregate<T, void> : std::true_type {};
#endif

template<typename T>
constexpr bool is_sax_aggregate_v = is_sax_aggregate<T>::value;

// ── Type-erased vtables ──────────────────────────────────────────────

struct ChildVTable {
    void (*on_bool)(void*, string_view, bool);
    void (*on_int)(void*, string_view, json_int_t);
    void (*on_float)(void*, string_view, double);
    void (*on_string)(void*, string_view, string_view);
    void (*on_number)(void*, string_view, string_view);
    void (*on_object_begin)(void*, string_view);
    void (*on_object_end)(void*, string_view);
    void (*on_array_begin)(void*, string_view);
    void (*on_array_end)(void*, string_view);
};

struct ArrayElemVTable {
    void (*assign_bool)(void*, bool);
    void (*assign_int)(void*, json_int_t);
    void (*assign_float)(void*, double);
    void (*assign_string)(void*, string_view);
    void (*assign_number)(void*, string_view);
    void (*create_child)(void* elem, void* storage, const ChildVTable** vt,
                         void** ctx, StringPool& pool);
};

struct ArrayState {
    void* data = nullptr;
    std::size_t elem_size = 0;
    std::size_t capacity = 0;
    std::size_t index = 0;
    bool is_struct = false;
    const ArrayElemVTable* elem_vt = nullptr;

    void* current() {
        if (index >= capacity) return nullptr;
        return static_cast<char*>(data) + index * elem_size;
    }
    void advance() { ++index; }
};

// ── SAX assignment helpers ────────────────────────────────────────────
//
// Each handler exposes a `handles_v<T>` trait declaring the field types
// it can assign into. sax_dispatch uses this at compile time to instantiate
// each handler only for compatible fields — incompatible field types are
// filtered out, so handler bodies never need silent-drop fallbacks.

struct SaxAssignBool {
    template<typename T>
    static constexpr bool handles_v = std::is_same_v<T, bool>;

    bool value;
    template<typename F>
    void operator()(F& field) const {
        static_assert(handles_v<std::remove_cv_t<F>>,
            "SaxAssignBool invoked on a non-bool field — dispatch filtering bug.");
        field = value;
    }
};

struct SaxAssignInt {
    template<typename T>
    static constexpr bool handles_v =
           (std::is_integral_v<T> && !std::is_same_v<T, bool>)
        || std::is_floating_point_v<T>;

    json_int_t value;
    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        static_assert(handles_v<V>,
            "SaxAssignInt invoked on an unsupported field type — dispatch filtering bug.");
        field = static_cast<V>(value);
    }
};

struct SaxAssignFloat {
    template<typename T>
    static constexpr bool handles_v =
           std::is_floating_point_v<T>
        || (std::is_integral_v<T> && !std::is_same_v<T, bool>);

    double value;
    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        static_assert(handles_v<V>,
            "SaxAssignFloat invoked on an unsupported field type — dispatch filtering bug.");
        field = static_cast<V>(value);
    }
};

struct SaxAssignString {
    template<typename T>
    static constexpr bool handles_v =
           std::is_same_v<T, string_view>
        || is_char_array_v<T>
        // Exclude aggregates: C++20 parenthesized aggregate init makes
        // is_constructible_v<Aggregate, ...> true even when the semantics
        // are narrowing/nonsense (e.g. a struct whose first field is bool
        // gets "constructed" from a const char*). Aggregate child structs
        // are handled by the nested-sink path, not here.
        || (!std::is_aggregate_v<T> && std::is_constructible_v<T, string_view>)
        || (!std::is_aggregate_v<T> && std::is_constructible_v<T, const char*, size_t>)
        || (!std::is_aggregate_v<T> && std::is_constructible_v<T, const char*>);

    string_view value;

    // char[N] field — memcpy with null terminator. Arrays can't be
    // constructed/assigned whole, so they need a dedicated overload.
    template<size_t N>
    void operator()(char (&field)[N]) const {
        const size_t copy_len = value.size() < N ? value.size() : (N - 1);
        for (size_t i = 0; i < copy_len; ++i) field[i] = value[i];
        field[copy_len] = '\0';
    }

    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        static_assert(handles_v<V>,
            "SaxAssignString invoked on an unsupported field type — dispatch filtering bug.");
        if constexpr (std::is_same_v<V, string_view>) {
            field = value;
        } else if constexpr (std::is_constructible_v<V, string_view>) {
            // Catches std::string (whose string_view ctor is explicit, so
            // is_convertible_v returns false but is_constructible_v does
            // not). The field owns its own copy after this.
            field = V(value);
        } else if constexpr (std::is_constructible_v<V, const char*, size_t>) {
            // Types that take (ptr, len) but not string_view directly —
            // e.g. Arduino String on cores that expose that ctor.
            field = V(value.data(), value.size());
        } else if constexpr (std::is_constructible_v<V, const char*>) {
            // StringPool::intern null-terminates; data() is safe to pass
            // to a single-arg (const char*) constructor — except when the
            // value is empty (intern returns {nullptr, 0}).
            if (!value.empty()) {
                field = V(value.data());
            } else {
                field = V{};
            }
        }
    }
};

struct SaxAssignNumber {
    template<typename T>
    static constexpr bool handles_v = SaxAssignInt::handles_v<T>;

    string_view raw;
    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        static_assert(handles_v<V>,
            "SaxAssignNumber invoked on an unsupported field type — dispatch filtering bug.");
        if constexpr (std::is_floating_point_v<V>) {
            field = static_cast<V>(parse_double(raw));
        } else {
            field = static_cast<V>(parse_int(raw));
        }
    }
};

// ── Field classification ──────────────────────────────────────────────

struct SaxDetectFieldKind {
    enum Kind { none, aggregate, array };
    Kind* result;

    // Detector claims every field — it classifies them rather than
    // assigning a value.
    template<typename T>
    static constexpr bool handles_v = true;

    template<typename F>
    void operator()(F& /*field*/) const {
        using V = std::remove_cv_t<F>;
        if constexpr (is_sax_aggregate<V>::value) {
            *result = aggregate;
        } else if constexpr (is_std_array<V>::value) {
            *result = array;
        }
    }
};

// ── Deferred child/array creation ─────────────────────────────────────
// These capture function pointers (thunks) that are only instantiated
// with StructSinkCore<V> at the call site, after StructSinkCore is
// fully defined.

using CreateChildFn = void(*)(void* field, void* storage,
    const ChildVTable** vt_out, void** ctx_out, StringPool& pool);

struct SaxCaptureChildCreator {
    template<typename T>
    static constexpr bool handles_v = is_sax_aggregate<T>::value;

    void** field_ptr;
    CreateChildFn* fn_out;

    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        static_assert(handles_v<V>,
            "SaxCaptureChildCreator invoked on non-aggregate — dispatch filtering bug.");
        *field_ptr = static_cast<void*>(&field);
        *fn_out = &create_child_thunk<V>;
    }

    template<typename V>
    static void create_child_thunk(void* field, void* storage,
        const ChildVTable** vt_out, void** ctx_out, StringPool& pool);
};

struct SaxCaptureArray {
    template<typename T>
    static constexpr bool handles_v = is_std_array<T>::value;

    ArrayState* state;
    bool* matched;

    template<typename F>
    void operator()(F& field) const {
        using V = std::remove_cv_t<F>;
        static_assert(handles_v<V>,
            "SaxCaptureArray invoked on non-array — dispatch filtering bug.");
        using Elem = typename array_traits<V>::element_type;
        state->data = field.data();
        state->elem_size = sizeof(Elem);
        state->capacity = array_traits<V>::size;
        state->index = 0;
        state->is_struct = is_sax_aggregate<Elem>::value;
        state->elem_vt = &array_elem_vtable_for<Elem>();
        *matched = true;
    }

    template<typename Elem>
    static const ArrayElemVTable& array_elem_vtable_for();
};

// ── Dispatch: route SAX event to the right field ──────────────────────
//
// Filters at compile time on Handler::handles_v<F> so each handler only
// instantiates for compatible field types. Fields whose types the handler
// doesn't claim are never passed to it — the instantiation is a no-op in
// the fold expression.

template<typename T, typename Handler,
    std::enable_if_t<has_note_fields_trait<T>::value, int> = 0>
bool sax_dispatch(T& obj, string_view key, Handler&& handler) {
    return T::_note_fields_dispatch(obj, key, std::forward<Handler>(handler));
}

#if __cplusplus >= 202002L

template<typename Handler, std::size_t N, typename R, typename T>
inline bool sax_try_field(T& obj, string_view key, Handler& handler) {
    using F = std::remove_cv_t<reflect::member_type<N, R>>;
    if constexpr (handler_accepts_v<std::remove_cvref_t<Handler>, F>) {
        if (key == reflect::member_name<N, R>()) {
            handler(reflect::get<N>(obj));
            return true;
        }
    }
    return false;
}

template<typename T, typename Handler>
    requires (ReflectableAggregate<T> && !has_note_fields_trait<T>::value)
bool sax_dispatch(T& obj, string_view key, Handler&& handler) {
    using R = std::remove_cvref_t<T>;
    bool matched = false;
    [&]<std::size_t... Ns>(std::index_sequence<Ns...>) {
        ((matched = matched
            || sax_try_field<std::remove_cvref_t<Handler>, Ns, R>(
                obj, key, handler)), ...);
    }(std::make_index_sequence<reflect::size<R>()>{});
    return matched;
}
#endif

} // namespace detail


// ═══════════════════════════════════════════════════════════════════════
// StructSinkCore<T> — dispatch logic without embedded child storage.
// Children are placement-new'd into workspace_ (pointer to external buf).
// ═══════════════════════════════════════════════════════════════════════

template<typename T>
struct StructSinkCore {
    T& obj;
    StringPool& pool;
    char* workspace_;   // shared workspace for child sinks (not owned)

    // Child state.
    void* child_ctx_ = nullptr;
    const detail::ChildVTable* child_vt_ = nullptr;
    int child_depth_ = 0;

    // Skip state.
    int skip_depth_ = 0;

    // Array state.
    detail::ArrayState array_state_{};
    bool in_array_ = false;

    StructSinkCore(T& obj_, StringPool& pool_, char* ws)
        : obj(obj_), pool(pool_), workspace_(ws) {}

    void on_bool(string_view k, bool v) {
        if (skip_depth_ > 0) return;
        if (child_ctx_) { child_vt_->on_bool(child_ctx_, k, v); return; }
        if (in_array_) { array_assign_bool(v); return; }
        detail::sax_dispatch(obj, k, detail::SaxAssignBool{v});
    }

    void on_int(string_view k, json_int_t v) {
        if (skip_depth_ > 0) return;
        if (child_ctx_) { child_vt_->on_int(child_ctx_, k, v); return; }
        if (in_array_) { array_assign_int(v); return; }
        detail::sax_dispatch(obj, k, detail::SaxAssignInt{v});
    }

    void on_float(string_view k, double v) {
        if (skip_depth_ > 0) return;
        if (child_ctx_) { child_vt_->on_float(child_ctx_, k, v); return; }
        if (in_array_) { array_assign_float(v); return; }
        detail::sax_dispatch(obj, k, detail::SaxAssignFloat{v});
    }

    void on_string(string_view k, string_view v) {
        if (skip_depth_ > 0) return;
        if (child_ctx_) { child_vt_->on_string(child_ctx_, k, v); return; }
        if (in_array_) { array_assign_string(v); return; }
        v = pool.intern(v);
        detail::sax_dispatch(obj, k, detail::SaxAssignString{v});
    }

    void on_number(string_view k, string_view raw) {
        if (skip_depth_ > 0) return;
        if (child_ctx_) { child_vt_->on_number(child_ctx_, k, raw); return; }
        if (in_array_) { array_assign_number(raw); return; }
        detail::sax_dispatch(obj, k, detail::SaxAssignNumber{raw});
    }

    void on_null(string_view) {}

    void on_object_begin(string_view k) {
        if (skip_depth_ > 0) { ++skip_depth_; return; }

        if (child_ctx_) {
            ++child_depth_;
            child_vt_->on_object_begin(child_ctx_, k);
            return;
        }

        if (in_array_) {
            array_on_object_begin();
            return;
        }

        detail::SaxDetectFieldKind::Kind kind = detail::SaxDetectFieldKind::none;
        detail::sax_dispatch(obj, k, detail::SaxDetectFieldKind{&kind});

        if (kind == detail::SaxDetectFieldKind::aggregate) {
            void* field_ptr = nullptr;
            detail::CreateChildFn creator = nullptr;
            detail::sax_dispatch(obj, k,
                detail::SaxCaptureChildCreator{&field_ptr, &creator});
            if (creator) {
                creator(field_ptr, workspace_, &child_vt_, &child_ctx_, pool);
                child_depth_ = 1;
            }
            return;
        }

        skip_depth_ = 1;
    }

    void on_object_end(string_view k) {
        if (skip_depth_ > 0) { --skip_depth_; return; }

        if (child_ctx_) {
            --child_depth_;
            if (child_depth_ > 0) {
                child_vt_->on_object_end(child_ctx_, k);
            } else {
                child_ctx_ = nullptr;
                child_vt_ = nullptr;
                if (in_array_) {
                    array_state_.advance();
                }
            }
            return;
        }
    }

    void on_array_begin(string_view k) {
        if (skip_depth_ > 0) { ++skip_depth_; return; }
        if (child_ctx_) { child_vt_->on_array_begin(child_ctx_, k); return; }

        bool matched = false;
        detail::sax_dispatch(obj, k,
            detail::SaxCaptureArray{&array_state_, &matched});
        if (matched) {
            in_array_ = true;
        } else {
            skip_depth_ = 1;
        }
    }

    void on_array_end(string_view k) {
        if (skip_depth_ > 0) { --skip_depth_; return; }
        if (child_ctx_) { child_vt_->on_array_end(child_ctx_, k); return; }

        if (in_array_) {
            in_array_ = false;
        }
    }

    void reset() { obj = T{}; }

private:
    void array_assign_bool(bool v) {
        auto* elem = array_state_.current();
        if (elem) {
            array_state_.elem_vt->assign_bool(elem, v);
            array_state_.advance();
        }
    }

    void array_assign_int(json_int_t v) {
        auto* elem = array_state_.current();
        if (elem) {
            array_state_.elem_vt->assign_int(elem, v);
            array_state_.advance();
        }
    }

    void array_assign_float(double v) {
        auto* elem = array_state_.current();
        if (elem) {
            array_state_.elem_vt->assign_float(elem, v);
            array_state_.advance();
        }
    }

    void array_assign_string(string_view v) {
        v = pool.intern(v);
        auto* elem = array_state_.current();
        if (elem) {
            array_state_.elem_vt->assign_string(elem, v);
            array_state_.advance();
        }
    }

    void array_assign_number(string_view raw) {
        auto* elem = array_state_.current();
        if (elem) {
            array_state_.elem_vt->assign_number(elem, raw);
            array_state_.advance();
        }
    }

    void array_on_object_begin() {
        if (!array_state_.is_struct) {
            ++skip_depth_;
            return;
        }
        auto* elem = array_state_.current();
        if (!elem) {
            ++skip_depth_;
            return;
        }
        array_state_.elem_vt->create_child(
            elem, workspace_, &child_vt_, &child_ctx_, pool);
        child_depth_ = 1;
    }
};


// ═══════════════════════════════════════════════════════════════════════
// StructSink<T> — user-facing wrapper with embedded workspace.
// Inherits all SAX methods from StructSinkCore<T>.
// ═══════════════════════════════════════════════════════════════════════

// Workspace size: must fit N StructSinkCore instances (one per nesting level).
// All StructSinkCore<T> are the same size regardless of T (pointer-sized ref).
// Each nesting level offsets its child past itself in the workspace.
namespace detail {
    struct SinkSizeProbe { int x; NOTE_FIELDS(x) };
    inline constexpr std::size_t struct_sink_core_size =
        sizeof(StructSinkCore<SinkSizeProbe>);

    inline constexpr std::size_t struct_sink_core_aligned =
        (struct_sink_core_size + alignof(std::max_align_t) - 1)
        & ~(alignof(std::max_align_t) - 1);
}

/// @tparam T          The struct type to parse into.
/// @tparam MaxDepth   Max aggregate nesting depth (e.g. A→B→C = depth 3).
///                    Defaults to 4. Deeper nesting requires a larger value.
template<typename T, std::size_t MaxDepth>
struct StructSink : StructSinkCore<T> {
    alignas(alignof(std::max_align_t))
    char workspace_buf_[detail::struct_sink_core_aligned * MaxDepth];

    StructSink(T& obj_, StringPool& pool_)
        : StructSinkCore<T>(obj_, pool_, workspace_buf_) {}
};


// ═══════════════════════════════════════════════════════════════════════
// Post-definition: vtables and thunks that reference StructSinkCore<U>.
// ═══════════════════════════════════════════════════════════════════════

namespace detail {

template<typename U>
const ChildVTable& child_vtable_for() {
    static const ChildVTable vt = {
        [](void* ctx, string_view k, bool v) {
            static_cast<StructSinkCore<U>*>(ctx)->on_bool(k, v);
        },
        [](void* ctx, string_view k, json_int_t v) {
            static_cast<StructSinkCore<U>*>(ctx)->on_int(k, v);
        },
        [](void* ctx, string_view k, double v) {
            static_cast<StructSinkCore<U>*>(ctx)->on_float(k, v);
        },
        [](void* ctx, string_view k, string_view v) {
            static_cast<StructSinkCore<U>*>(ctx)->on_string(k, v);
        },
        [](void* ctx, string_view k, string_view v) {
            static_cast<StructSinkCore<U>*>(ctx)->on_number(k, v);
        },
        [](void* ctx, string_view k) {
            static_cast<StructSinkCore<U>*>(ctx)->on_object_begin(k);
        },
        [](void* ctx, string_view k) {
            static_cast<StructSinkCore<U>*>(ctx)->on_object_end(k);
        },
        [](void* ctx, string_view k) {
            static_cast<StructSinkCore<U>*>(ctx)->on_array_begin(k);
        },
        [](void* ctx, string_view k) {
            static_cast<StructSinkCore<U>*>(ctx)->on_array_end(k);
        },
    };
    return vt;
}

template<typename V>
void SaxCaptureChildCreator::create_child_thunk(void* field, void* storage,
    const ChildVTable** vt_out, void** ctx_out, StringPool& pool) {
    // Child is placed at the start of storage. Its own workspace is offset
    // past itself so that grandchildren don't overwrite the child.
    auto* child = ::new (storage) StructSinkCore<V>(
        *static_cast<V*>(field), pool,
        static_cast<char*>(storage) + struct_sink_core_size);
    *ctx_out = static_cast<void*>(child);
    *vt_out = &child_vtable_for<V>();
}

template<typename Elem>
const ArrayElemVTable& SaxCaptureArray::array_elem_vtable_for() {
    // Each vtable entry routes runtime array-element assignments through
    // the corresponding SaxAssign* handler. Handlers filter per Elem via
    // handles_v at compile time; incompatible combinations collapse to
    // a no-op so the vtable still has a slot for every wire event.
    static const ArrayElemVTable vt = {
        [](void* elem, bool v) {
            if constexpr (SaxAssignBool::handles_v<Elem>) {
                SaxAssignBool{v}(*static_cast<Elem*>(elem));
            }
            (void)elem; (void)v;
        },
        [](void* elem, json_int_t v) {
            if constexpr (SaxAssignInt::handles_v<Elem>) {
                SaxAssignInt{v}(*static_cast<Elem*>(elem));
            }
            (void)elem; (void)v;
        },
        [](void* elem, double v) {
            if constexpr (SaxAssignFloat::handles_v<Elem>) {
                SaxAssignFloat{v}(*static_cast<Elem*>(elem));
            }
            (void)elem; (void)v;
        },
        [](void* elem, string_view v) {
            if constexpr (SaxAssignString::handles_v<Elem>) {
                SaxAssignString{v}(*static_cast<Elem*>(elem));
            }
            (void)elem; (void)v;
        },
        [](void* elem, string_view v) {
            if constexpr (SaxAssignNumber::handles_v<Elem>) {
                SaxAssignNumber{v}(*static_cast<Elem*>(elem));
            }
            (void)elem; (void)v;
        },
        [](void* elem, void* storage, const ChildVTable** vt_out,
           void** ctx_out, StringPool& pool) {
            if constexpr (is_sax_aggregate<Elem>::value) {
                auto* child = ::new (storage) StructSinkCore<Elem>(
                    *static_cast<Elem*>(elem), pool,
                    static_cast<char*>(storage) + struct_sink_core_size);
                *ctx_out = static_cast<void*>(child);
                *vt_out = &child_vtable_for<Elem>();
            }
            (void)elem; (void)storage; (void)vt_out; (void)ctx_out; (void)pool;
        },
    };
    return vt;
}

} // namespace detail


// ═══════════════════════════════════════════════════════════════════════
// BodyHandler — type-erased SAX event forwarder for streaming body parse.
//
// Stores a context pointer and function pointers that dispatch SAX events
// to a StructSink<T> without the caller needing to know T. Created via
// make_body_handler<T>().
// ═══════════════════════════════════════════════════════════════════════

// BodyEvent and BodyHandler are defined in body_handler.hpp.

namespace detail {

/// Non-template switch that drives a StructSinkCore via its ChildVTable.
/// Per-T instantiations of make_body_handler all funnel through this one
/// function, so the switch's branches are counted once (not per RequestT).
NOTE_SINK_NOINLINE inline void dispatch_struct_body_event(
    void* core, const ChildVTable& vt,
    void (*reset_fn)(void*), void* reset_ctx,
    const BodyEvent& ev) {
    switch (ev.tag) {
    case BodyEvent::Bool:        vt.on_bool(core, ev.key, ev.b); break;
    case BodyEvent::Int:         vt.on_int(core, ev.key, ev.i); break;
    case BodyEvent::Float:       vt.on_float(core, ev.key, ev.f); break;
    case BodyEvent::String:      vt.on_string(core, ev.key, {ev.sv.data, ev.sv.len}); break;
    case BodyEvent::Number:      vt.on_number(core, ev.key, {ev.sv.data, ev.sv.len}); break;
    case BodyEvent::ObjectBegin: vt.on_object_begin(core, ev.key); break;
    case BodyEvent::ObjectEnd:   vt.on_object_end(core, ev.key); break;
    case BodyEvent::ArrayBegin:  vt.on_array_begin(core, ev.key); break;
    case BodyEvent::ArrayEnd:    vt.on_array_end(core, ev.key); break;
    case BodyEvent::Reset:       reset_fn(reset_ctx); break;
    default: NOTE_UNREACHABLE();
    }
}

/// Per-T thunks — branchless. The compiler emits one tiny function per T
/// but the only branches are in the shared dispatch helper above.
template<typename T>
void struct_sink_reset_thunk(void* c) {
    static_cast<StructSink<T>*>(c)->reset();
}

template<typename T>
void struct_sink_body_dispatch_thunk(void* c, const BodyEvent& ev) {
    auto* sink = static_cast<StructSink<T>*>(c);
    dispatch_struct_body_event(
        static_cast<StructSinkCore<T>*>(sink),
        child_vtable_for<T>(),
        &struct_sink_reset_thunk<T>, c,
        ev);
}

} // namespace detail

/// Create a BodyHandler that forwards events to a StructSink<T>.
/// Dispatch is type-erased through `dispatch_struct_body_event` so the
/// 10-way switch is counted once for coverage, not per RequestT.
template<typename T>
BodyHandler make_body_handler(StructSink<T>& sink) {
    return {
        &sink,
        &detail::struct_sink_body_dispatch_thunk<T>,
    };
}

/// Outlined dispatch helper for GenericBodySink. The lambda installed by
/// make_generic_body_handler is a thin forwarder; the switch body lives
/// once here. Mirrors dispatch_sax_event in generic_sink.hpp.
inline NOTE_SINK_NOINLINE void dispatch_body_event(GenericBodySink& s, const BodyEvent& ev) {
    switch (ev.tag) {
    case BodyEvent::Bool:   s.on_bool(ev.key, ev.b); break;
    case BodyEvent::Int:    s.on_int(ev.key, ev.i); break;
    case BodyEvent::Float:  s.on_float(ev.key, ev.f); break;
    case BodyEvent::String: s.on_string(ev.key, {ev.sv.data, ev.sv.len}); break;
    case BodyEvent::Number: s.on_number(ev.key, {ev.sv.data, ev.sv.len}); break;
    default: break;
    }
}

/// Create a BodyHandler that forwards events to a GenericBodySink.
/// Non-template: ONE instantiation for all body types.
inline BodyHandler make_generic_body_handler(GenericBodySink& sink) {
    return {
        &sink,
        [](void* c, const BodyEvent& ev) {
            dispatch_body_event(*static_cast<GenericBodySink*>(c), ev);
        },
    };
}

/// Size of the StructSink workspace for body handler storage.
/// Used by execute paths to allocate stack storage for the body sink.
/// When NOTE_RESPONSE_BODY=0, GenericBodySink replaces StructSink — much smaller.
inline constexpr std::size_t body_sink_storage_size =
#if !NOTE_RESPONSE_BODY
    sizeof(GenericBodySink);
#else
    sizeof(StructSink<detail::SinkSizeProbe>);
#endif

/// Alignment of the StructSink for body handler storage.
inline constexpr std::size_t body_sink_storage_align = alignof(std::max_align_t);

} // namespace note
