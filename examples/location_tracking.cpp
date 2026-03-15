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
    // Configure location mode — intent-based API
    // https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location-mode
    // -----------------------------------------------------------------------

    // {"req":"card.location.mode","mode":"periodic","seconds":300}
    {
        note::api::CardLocationMode::Periodic req;
        req.seconds(300_s);
        nc.execute(req);
        // mode:"periodic" is emitted automatically
    }

    // {"req":"card.location.mode","mode":"continuous"}
    {
        nc.execute(note::api::CardLocationMode::Continuous{});
        // mode:"continuous" is emitted automatically
    }

    // {"req":"card.location.mode","mode":"fixed","lat":42.577,"lon":-70.871}
    {
        note::api::CardLocationMode::Fixed req;
        req.lat(42.577).lon(-70.871);
        nc.execute(req);
    }

    // {"req":"card.location.mode","mode":"periodic","lat":42.577,"lon":-70.871,"max":100,"minutes":2}
    // Geofencing is periodic mode with lat/lon/max/minutes
    {
        note::api::CardLocationMode::Periodic req;
        req.lat(42.577).lon(-70.871).max(100).minutes(2);
        nc.execute(req);
    }

    // -----------------------------------------------------------------------
    // Configure location mode — base API (full field access)
    // -----------------------------------------------------------------------

    // {"req":"card.location.mode","mode":"periodic","seconds":300}
    {
        note::api::CardLocationMode::Set req;
        req.mode("periodic").seconds(300_s);
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
