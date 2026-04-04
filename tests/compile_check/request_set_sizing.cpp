// Compile-check: RequestSet computes a valid constexpr arena size.
#include <note/request_set.hpp>
#include <note/api/card_status.hpp>
#include <note/api/card_version.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/note_add.hpp>
#include <note/arena.hpp>

using MyRequests = note::RequestSet<
    note::api::CardStatus,
    note::api::CardVersion,
    note::api::HubSet,
    note::api::NoteAdd
>;

static_assert(MyRequests::max_arena_size > 0, "arena size must be positive");
static_assert(MyRequests::max_arena_size < 8192, "arena size sanity bound");

// Verify the buffer can be stack-allocated
static char arena_buf[MyRequests::max_arena_size];
static note::MonotonicArena arena(arena_buf);
