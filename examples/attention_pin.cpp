// ATTN pin interrupt examples using note-cpp.
//
// C++ equivalents of the attention API requests from:
//   https://dev.blues.io/api-reference/notecard-api/card-requests/#card-attn
//
// Build: c++ -std=c++20 -I include -fsyntax-only examples/attention_pin.cpp

#include <note/notecard.hpp>
#include <note/api/card_attn.hpp>

// ═══════════════════════════════════════════════════════════════════════════
// Intent-based API — each intent exposes only relevant fields and returns
// a response shaped to that specific operation.
// ═══════════════════════════════════════════════════════════════════════════

void intent_examples(note::Notecard& nc) {
    using namespace note::literals;

    // Arm ATTN for connectivity changes
    // {"req":"card.attn","mode":"arm,connected"}
    {
        note::api::CardAttn::Arm req;
        req.mode("arm,connected");
        nc.execute(req);
        // Returns ApiResult<CardAttn::Arm::Response> with just .set
    }

    // Watchdog timer — mode:"watchdog" is emitted automatically
    // {"req":"card.attn","mode":"watchdog","seconds":60}
    {
        note::api::CardAttn::Watchdog req;
        req.seconds(60_s);
        nc.execute(req);
        // Returns ApiResult<void> — just check success/error
    }

    // Sleep with payload — mode:"sleep" is emitted automatically
    // {"req":"card.attn","mode":"sleep","seconds":3600}
    {
        note::api::CardAttn::Sleep req;
        req.seconds(3600_s);
        nc.execute(req);
        // Returns ApiResult<void>
    }

    // Retrieve payload after sleep — start:true is emitted automatically
    // {"req":"card.attn","start":true}
    {
        auto result = nc.execute(note::api::CardAttn::Retrieve{});
        if (result) {
            auto payload = result.payload;
            auto time = result.time;
            (void)payload; (void)time;
        }
        // Returns ApiResult<CardAttn::Retrieve::Response> with .payload, .time
    }

    // Disarm — mode:"disarm,-all" is emitted automatically
    // {"req":"card.attn","mode":"disarm,-all"}
    {
        nc.execute(note::api::CardAttn::Disarm{});
        // Returns ApiResult<void>
    }

    // Query ATTN state
    // {"req":"card.attn","verify":true}
    {
        note::api::CardAttn::Query req;
        req.verify(true);
        auto result = nc.execute(req);
        if (result) {
            auto set = result.set;
            (void)set;
        }
        // Returns ApiResult<CardAttn::Query::Response> with .set, .off
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Base API — CardAttn::Request exposes all fields and returns the full
// response. Use this when the intent variants don't fit your use case.
// ═══════════════════════════════════════════════════════════════════════════

void base_examples(note::Notecard& nc) {
    using namespace note::literals;

    // Arm ATTN for connectivity changes (same wire format as intent version)
    // {"req":"card.attn","mode":"arm,connected"}
    {
        note::api::CardAttn::Request req;
        req.mode("arm,connected");
        auto result = nc.execute(req);
        if (result) {
            // Full response available: .set, .off, .payload, .time
            auto set = result.set;
            (void)set;
        }
    }

    // Watchdog timer (must set mode explicitly)
    // {"req":"card.attn","mode":"watchdog","seconds":60}
    {
        note::api::CardAttn::Request req;
        req.mode("watchdog").seconds(60_s);
        nc.execute(req);
    }

    // Disarm
    // {"req":"card.attn","mode":"disarm,-all"}
    {
        note::api::CardAttn::Request req;
        req.mode("disarm,-all");
        nc.execute(req);
    }
}

int main() { return 0; }
