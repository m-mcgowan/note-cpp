// RequestSet — compile-time arena sizing for a known set of request types.
//
// Computes the maximum arena budget across all response types, enabling
// static allocation of an arena buffer that's guaranteed sufficient for
// any request in the set.
//
// Usage:
//   using MyRequests = note::RequestSet<
//       note::api::CardStatus,
//       note::api::CardVersion,
//       note::api::HubSet
//   >;
//   static constexpr size_t kArenaSize = MyRequests::max_arena_size;
//   char arena_buf[kArenaSize];
#pragma once

#include <cstddef>
#include <type_traits>

namespace note {

namespace detail {

// Trait: extract Response::max_arena_size, or 0 if no Response type or
// Response is void.
template<typename Req, typename = void>
struct response_arena_size {
    static constexpr size_t value = 0;
};

// Specialization when Req::Response exists and is not void.
template<typename Req>
struct response_arena_size<Req,
    std::void_t<decltype(Req::Response::max_arena_size)>>
{
    static constexpr size_t value = Req::Response::max_arena_size;
};

} // namespace detail

template<typename... Reqs>
struct RequestSet {
    static constexpr size_t max_arena_size =
        [] {
            size_t m = 0;
            ((m = detail::response_arena_size<Reqs>::value > m
                  ? detail::response_arena_size<Reqs>::value : m), ...);
            return m;
        }();
};

} // namespace note
