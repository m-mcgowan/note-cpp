// Location tracking examples using note-cpp.
//
// C++ equivalents of the location API requests from:
//   https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location
//   https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location-mode
//   https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location-track
//
// Build: c++ -std=c++2b -I include -fsyntax-only examples/location_tracking.cpp

#include <note/notecard.hpp>
#include <note/api/card_location.hpp>
#include <note/api/card_location_mode.hpp>
#include <note/api/card_location_track.hpp>

void examples(note::Notecard& nc) {
    using namespace note::literals;

    // -----------------------------------------------------------------------
    // Get current location
    // https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location
    // -----------------------------------------------------------------------

    // {"req":"card.location"}
    {
        auto result = nc.execute(note::api::CardLocation{});
        if (result) {
            auto lat = result.lat;
            auto lon = result.lon;
            auto time = result.time;
            (void)lat; (void)lon; (void)time;
        }
    }

    // -----------------------------------------------------------------------
    // Configure location mode
    // https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location-mode
    // -----------------------------------------------------------------------

    // {"req":"card.location.mode","mode":"periodic","seconds":300}
    {
        note::api::CardLocationMode::Set req;
        req.mode("periodic").seconds(300_s);
        nc.execute(req);
    }

    // {"req":"card.location.mode","mode":"continuous"}
    {
        note::api::CardLocationMode::Set req;
        req.mode("continuous");
        nc.execute(req);
    }

    // {"req":"card.location.mode"} — query current mode
    {
        auto result = nc.execute(note::api::CardLocationMode::Get{});
        if (result) {
            auto mode = result.mode;
            (void)mode;
        }
    }

    // {"req":"card.location.mode","delete":true} — reset to defaults
    {
        nc.execute(note::api::CardLocationMode::Delete{});
    }

    // -----------------------------------------------------------------------
    // Location tracking
    // https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location-track
    // -----------------------------------------------------------------------

    // {"req":"card.location.track","start":true,"heartbeat":true,"hours":12}
    {
        note::api::CardLocationTrack req;
        req.start(true).heartbeat(true).hours(12);
        nc.execute(req);
    }

    // {"req":"card.location.track","stop":true}
    {
        note::api::CardLocationTrack req;
        req.stop(true);
        nc.execute(req);
    }
}

int main() { return 0; }
