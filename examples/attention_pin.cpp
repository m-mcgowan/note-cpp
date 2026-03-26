// Attention pin — using the Notecard's ATTN pin to wake the host MCU.
//
// Many IoT devices spend most of their time in deep sleep to conserve
// power. The Notecard's ATTN (attention) pin lets it wake the host MCU
// when something needs attention — a connectivity change, new inbound
// data, motion detected, or a timer expiring.
//
// This example shows how to:
//   - Arm ATTN for specific events (connectivity, motion, file changes)
//   - Set a watchdog timer that wakes the host periodically
//   - Put the host to sleep and pass state across the sleep/wake cycle
//   - Retrieve that state on wakeup to resume where you left off
//   - Query and disarm the ATTN state
//
// There are two API levels:
//   - Intent-based (recommended): each operation is a distinct type with
//     only the fields that apply. Safer, clearer, better autocomplete.
//   - Base API: one type with all fields. Use when the intent variants
//     don't fit your use case, or when you need the full response.
//
// See: https://dev.blues.io/api-reference/notecard-api/card-requests/#card-attn
//
// Build: c++ -std=c++20 -I include -fsyntax-only examples/attention_pin.cpp

#include <note/notecard.hpp>
#include <note/api/card_attn.hpp>

// ═════════════════════════════════════════════════════════════════════════
// Intent-based API — each operation has its own type with only the
// relevant fields. The "mode" prefix (e.g. "arm,", "sleep,") is added
// automatically so you can't get it wrong.
// ═════════════════════════════════════════════════════════════════════════

void intent_examples(note::Notecard& nc) {
    using namespace note::literals;

    // Arm ATTN to fire when the Notecard gains or loses connectivity.
    // The "arm," prefix is added automatically — you just pick triggers.
    // {"req":"card.attn","mode":"arm,connected"}
    {
        nc.execute(note::api::CardAttn::Arm{}.connected());

        // Multiple triggers can be chained:
        // nc.execute(note::api::CardAttn::Arm{}.connected().motion().files());
    }

    // Watchdog timer — wake the host after 60 seconds of inactivity.
    // Useful as a safety net: if the main loop hangs, ATTN fires.
    // {"req":"card.attn","mode":"watchdog","seconds":60}
    {
        note::api::CardAttn::Watchdog req;
        req.seconds(60_s);
        nc.execute(req);
    }

    // Sleep — tell the Notecard to pulse ATTN after 1 hour, then put
    // the host MCU into deep sleep. The Notecard stays awake.
    // {"req":"card.attn","mode":"sleep","seconds":3600}
    {
        note::api::CardAttn::Sleep req;
        req.seconds(1_hours);
        nc.execute(req);
        // After this call, enter deep sleep. The Notecard will wake you.
    }

    // Sleep with payload — pass state across the sleep/wake cycle.
    // The Notecard holds a string for you while the host sleeps. On
    // wakeup, retrieve it to decide whether to resume or reinitialize.
    // {"req":"card.attn","mode":"sleep","seconds":3600,"payload":"checkpoint-v1"}
    {
        note::api::CardAttn::Sleep req;
        req.seconds(1_hours);
        req.payload("checkpoint-v1");
        nc.execute(req);
    }

    // On wakeup: retrieve the stored payload.
    // If there's a payload, this is a sleep resume — pick up where you
    // left off. If not, it's a fresh boot — initialize from scratch.
    // {"req":"card.attn","start":true}
    {
        auto result = nc.execute(note::api::CardAttn::Retrieve{});
        if (result && result.time != 0) {
            // Sleep resume — payload holds the state saved before sleeping
            auto payload = result.payload;  // "checkpoint-v1"
            auto stored_at = result.time;   // UNIX epoch when saved
            (void)payload; (void)stored_at;
        } else {
            // Fresh boot — no prior sleep, start from scratch
        }
    }

    // Disarm — clear all ATTN triggers.
    // {"req":"card.attn","mode":"disarm,-all"}
    {
        nc.execute(note::api::CardAttn::Disarm{});
    }

    // Query — check what ATTN triggers are currently armed.
    // {"req":"card.attn","verify":true}
    {
        note::api::CardAttn::Query req;
        req.verify(true);
        auto result = nc.execute(req);
        if (result) {
            auto set = result.set;   // true if ATTN pin is currently asserted
            (void)set;
        }
    }
}


// ═════════════════════════════════════════════════════════════════════════
// Base API — one type (CardAttn::Request) with all fields and the full
// response. Use this when the intent variants don't fit, or when you
// need to combine modes in unusual ways.
// ═════════════════════════════════════════════════════════════════════════

void base_examples(note::Notecard& nc) {
    using namespace note::literals;

    // Arm with named flag methods — type-safe, autocomplete-friendly.
    // {"req":"card.attn","mode":"arm,connected"}
    {
        note::api::CardAttn::Arm req;
        req.connected();
        auto result = nc.execute(req);
        if (result) {
            auto set = result.set;  // full response available
            (void)set;
        }
    }

    // Arm with flag constants and operator| — concise for multiple flags.
    // {"req":"card.attn","mode":"arm,connected,files,motion"}
    {
        using namespace note::attn;
        note::api::CardAttn::Arm req;
        req.triggers = connected | files | motion;
        nc.execute(req);
    }

    // Disarm — a separate intent, not a flag on arm.
    // {"req":"card.attn","mode":"disarm,-all"}
    {
        note::api::CardAttn::Disarm req;
        nc.execute(req);
    }
}


int main() {
    // These examples use -fsyntax-only (compilation check, no linking).
    // On real hardware, pass a real Notecard instance to these functions.
    (void)intent_examples;
    (void)base_examples;
}
