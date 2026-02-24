// ATTN pin interrupt examples using note-cpp.
//
// C++ equivalents of the attention API requests from:
//   https://dev.blues.io/api-reference/notecard-api/card-requests/#card-attn
//
// Build: c++ -std=c++2b -I include -fsyntax-only examples/attention_pin.cpp

#include <note/notecard.hpp>
#include <note/api/card_attn.hpp>

void examples(note::Notecard& nc) {

    // -----------------------------------------------------------------------
    // Arm ATTN for file changes
    // -----------------------------------------------------------------------

    // {"req":"card.attn","mode":"arm,files","files":["data.qi","my-settings.db"]}
    {
        note::api::CardAttn req;
        req.set_mode("arm,files");
        // Note: array properties like 'files' are not yet supported in V1.
        // Use ad-hoc request for array fields:
        nc.request("card.attn", [](note::JsonBuilder& b) {
            b.add("mode", "arm,files");
            b.begin_array("files");
            // Array item support would go here
            b.end_array();
        });
    }

    // -----------------------------------------------------------------------
    // Arm ATTN for connectivity changes
    // -----------------------------------------------------------------------

    // {"req":"card.attn","mode":"arm,connected"}
    {
        note::api::CardAttn req;
        req.set_mode("arm,connected");
        nc.execute(req);
    }

    // -----------------------------------------------------------------------
    // Watchdog timer
    // -----------------------------------------------------------------------

    // {"req":"card.attn","mode":"watchdog","seconds":60}
    {
        note::api::CardAttn req;
        req.set_mode("watchdog").set_seconds(60);
        nc.execute(req);
    }

    // -----------------------------------------------------------------------
    // Sleep with payload
    // -----------------------------------------------------------------------

    // {"req":"card.attn","mode":"sleep","seconds":3600}
    {
        note::api::CardAttn req;
        req.set_mode("sleep").set_seconds(3600);
        nc.execute(req);
    }

    // -----------------------------------------------------------------------
    // Disarm
    // -----------------------------------------------------------------------

    // {"req":"card.attn","mode":"disarm,-all"}
    {
        note::api::CardAttn req;
        req.set_mode("disarm,-all");
        nc.execute(req);
    }

    // -----------------------------------------------------------------------
    // Query ATTN state
    // -----------------------------------------------------------------------

    // {"req":"card.attn"}
    {
        auto result = nc.execute(note::api::CardAttn{});
        if (result) {
            auto set = result->set;
            (void)set;
        }
    }
}

int main() { return 0; }
