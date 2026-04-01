#pragma once

/// @file static_notecard.hpp
/// StaticNotecard<Stack> — zero-vtable Notecard for constrained platforms.
///
/// Owns the transport stack by value. All calls go through concrete types —
/// no virtual dispatch, no vtable entries in .data. Uses the same
/// execute_streaming() core as Notecard, just with a concrete transport ref
/// instead of an IStreamingTransport*.
///
/// Usage:
///   using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
///   SerialNotecard nc(Serial, 9600, arena_allocator(arena));
///   note::Api api(nc);
///   api.hub.set().product("com.example").execute();

#include "allocator.hpp"
#include "compiler.hpp"
#include "json.hpp"
#include "notecard.hpp"
#include "string_pool.hpp"
#include "streaming_transport.hpp"

#include <optional>
#include <type_traits>

namespace note {

/// Notecard implementation with zero virtual dispatch overhead.
/// Stack must provide a `transport` member with `transact(BuildFn, void*, JsonSink&, uint32_t)`.
template<typename Stack>
class StaticNotecard {
public:
    /// Construct by forwarding args to the Stack constructor.
    template<typename... Args>
    explicit StaticNotecard(Allocator alloc, Args&&... args)
        : stack_(std::forward<Args>(args)...)
        , alloc_(alloc) {}

    void set_allocator(Allocator alloc) { alloc_ = alloc; }
    void set_default_timeout(uint32_t ms) { default_timeout_ms_ = ms; }
    uint32_t default_timeout() const { return default_timeout_ms_; }

    template<typename RequestT>
    ApiResult<typename RequestT::Response> execute(const RequestT& req) {
        using Rsp = typename RequestT::Response;

        auto build = [&](JsonBuilder& b) {
            b.add("req", RequestT::notecard_request);
            req.build(b);
        };
        BuildFn build_fn = [](JsonBuilder& b, void* p) {
            (*static_cast<decltype(build)*>(p))(b);
        };

        StringPool pool(alloc_);

        if constexpr (std::is_void_v<Rsp>) {
            JsonSink null_sink;
            auto ei = Notecard::execute_streaming(
                stack_.transport, default_timeout_ms_,
                build_fn, &build, null_sink, pool);
            if (ei.code != Error{}) return ApiResult<void>(ei);
            return ApiResult<void>{};
        } else {
            Rsp rsp_val{};
            typename Rsp::Sink response_sink(rsp_val, pool);
            auto ei = Notecard::execute_streaming(
                stack_.transport, default_timeout_ms_,
                build_fn, &build, response_sink, pool);
            if (ei.code != Error{}) return ApiResult<Rsp>(ei);
            return ApiResult<Rsp>(std::move(rsp_val));
        }
    }

    template<typename RequestT>
    Result<void> command_typed(const RequestT& req) {
        auto build = [&](JsonBuilder& b) {
            b.add("cmd", RequestT::notecard_request);
            req.build(b);
        };
        BuildFn build_fn = [](JsonBuilder& b, void* p) {
            (*static_cast<decltype(build)*>(p))(b);
        };
        return stack_.transport.send(build_fn, &build);
    }

    /// Access the transport stack (e.g. for binary I/O).
    Stack& stack() { return stack_; }

private:
    Stack stack_;
    Allocator alloc_;
    uint32_t default_timeout_ms_ = 10000;
};

} // namespace note
