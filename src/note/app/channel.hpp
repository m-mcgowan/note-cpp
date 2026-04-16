#pragma once

#include <note/notecard.hpp>

#if !NOTE_NO_POLYMORPHIC

namespace note::app {

// DirectChannel — executes requests immediately against a Notecard instance.
// No queue, no thread safety, trivially re-entrant.
//
// Usage:
//   note::Notecard nc(backend, transport);
//   note::app::DirectChannel ch(nc);
//   auto r = ch.execute(api.cardVersion());

class DirectChannel {
public:
    explicit DirectChannel(Notecard& nc) : nc_(nc) {}

    template<typename Req>
    auto execute(const Req& req) -> ApiResult<typename Req::Response> {
        return nc_.execute(req);
    }

    template<typename Req>
    auto command(const Req& req) -> Result<void> {
        return nc_.command_typed(req);
    }

    void tick() {}

    Notecard& notecard() { return nc_; }

private:
    Notecard& nc_;
};

} // namespace note::app

#endif // NOTE_NO_POLYMORPHIC
