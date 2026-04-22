// Compile check: verify generated request/response fields use the
// expected C++ types. This catches regressions in the type refinement
// pipeline (spec_parser.py + type_refinements.json + codegen template).

#include <note/api.hpp>
#include <type_traits>

// ── Core type definitions ──────────────────────────────────────────────

#if !NOTE_INT32_MATH
static_assert(std::is_same_v<note::json_int_t, int64_t>,
    "json_int_t should be int64_t without NOTE_INT32_MATH");
#else
static_assert(std::is_same_v<note::json_int_t, int32_t>,
    "json_int_t should be int32_t with NOTE_INT32_MATH");
#endif

#if !NOTE_SHORT_TIMESTAMPS
static_assert(std::is_same_v<note::json_time_t, int64_t>,
    "json_time_t should be int64_t by default (Y2038-safe)");
#endif

// ── Response integer fields → json_int_t ───────────────────────────────
// API response integer fields default to json_int_t.

static_assert(std::is_same_v<
    decltype(std::declval<note::api::HubSyncStatus::Response>().completed),
    note::ResponseField<note::json_int_t>>,
    "hub.sync.status response.completed should be json_int_t");

// ── UNIX timestamp fields → json_time_t ────────────────────────────────
// Fields with format: unix-time auto-map to json_time_t.

static_assert(std::is_same_v<
    decltype(std::declval<note::api::CardAux::Response>().time),
    note::ResponseField<note::json_time_t>>,
    "card.aux response.time should be json_time_t (format: unix-time)");

// ── Request integer fields → json_int_t ────────────────────────────────
// Note: HubSet::seconds is a named struct (seconds_t) that inherits
// Field<json_int_t>, so we check the Field base type.

static_assert(std::is_base_of_v<
    note::Field<note::json_int_t>,
    note::api::HubSet::seconds_t>,
    "hub.set seconds should inherit Field<json_int_t>");

// ── Float response fields → double ─────────────────────────────────────

static_assert(std::is_same_v<
    decltype(std::declval<note::api::CardTemp::Read::Response>().value),
    note::ResponseField<double>>,
    "card.temp response.value should be double (JSON number type)");

void test() {}
