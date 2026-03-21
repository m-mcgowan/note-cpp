// Location tracking — GPS and geofencing with the Notecard.
//
// The Notecard has an onboard GPS/GNSS module that can track location
// independently of the host MCU. You configure it once, and the Notecard
// handles satellite acquisition, periodic fixes, and syncing location
// data to Notehub — all while the host sleeps.
//
// This example shows how to:
//   - Read the current location
//   - Configure periodic GPS fixes (e.g. every 5 minutes)
//   - Set continuous mode for real-time tracking
//   - Pin a fixed location (no GPS needed — useful for stationary devices)
//   - Set up a geofence (alert when the device moves beyond a radius)
//   - Start and stop location tracking to a Notefile
//
// See:
//   https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location
//   https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location-mode
//   https://dev.blues.io/api-reference/notecard-api/card-requests/#card-location-track
//
// Build: c++ -std=c++20 -I include -fsyntax-only examples/location_tracking.cpp

#include <note/notecard.hpp>
#include <note/api/card_location.hpp>
#include <note/api/card_location_mode.hpp>
#include <note/api/card_location_track.hpp>

void examples(note::Notecard& nc) {
    using namespace note::literals;

    // ─── Read current location ──────────────────────────────────────────
    // Returns the most recent GPS fix. If the Notecard hasn't acquired a
    // fix yet, lat/lon will be zero.
    // {"req":"card.location"}
    {
        auto result = nc.execute(note::api::CardLocation{});
        if (result) {
            auto lat = result.lat;
            auto lon = result.lon;
            auto time = result.time;  // UNIX epoch of the fix
            (void)lat; (void)lon; (void)time;
        }
    }

    // ─── Periodic mode ──────────────────────────────────────────────────
    // Take a GPS fix every N seconds. The Notecard powers the GPS module
    // on, gets a fix, and powers it off. Good balance of accuracy and
    // power consumption for most tracking applications.
    // {"req":"card.location.mode","mode":"periodic","seconds":300}
    {
        note::api::CardLocationMode::Periodic req;
        req.seconds(300_s);  // fix every 5 minutes
        nc.execute(req);
        // "mode":"periodic" is added automatically by the Periodic type
    }

    // ─── Continuous mode ────────────────────────────────────────────────
    // Keep the GPS module powered on for real-time tracking. Uses more
    // power but gives the most accurate and frequent location data.
    // {"req":"card.location.mode","mode":"continuous"}
    {
        nc.execute(note::api::CardLocationMode::Continuous{});
    }

    // ─── Fixed location ─────────────────────────────────────────────────
    // Pin a known location without using GPS. Useful for stationary
    // devices (gateways, environmental monitors) where you know the
    // coordinates at deployment time.
    // {"req":"card.location.mode","mode":"fixed","lat":42.577,"lon":-70.871}
    {
        note::api::CardLocationMode::Fixed req;
        req.lat(42.577).lon(-70.871);
        nc.execute(req);
    }

    // ─── Geofencing ─────────────────────────────────────────────────────
    // Periodic mode with a geofence: the Notecard takes GPS fixes and
    // checks whether the device has moved beyond a radius from a center
    // point. If it has, ATTN fires (or a note is added).
    //   lat/lon = center of the geofence
    //   max     = radius in meters
    //   minutes = how often to check
    // {"req":"card.location.mode","mode":"periodic","lat":42.577,"lon":-70.871,"max":100,"minutes":2}
    {
        note::api::CardLocationMode::Periodic req;
        req.lat(42.577).lon(-70.871);
        req.max(100);      // 100 meter radius
        req.minutes(2);    // check every 2 minutes
        nc.execute(req);
    }

    // ─── Base API — full field access ───────────────────────────────────
    // The intent types (Periodic, Continuous, Fixed) cover most use cases.
    // The base Set type gives access to all fields when you need it.
    // {"req":"card.location.mode","mode":"periodic","seconds":300}
    {
        note::api::CardLocationMode::Set req;
        req.mode("periodic").seconds(300_s);
        nc.execute(req);
    }

    // ─── Query current mode ─────────────────────────────────────────────
    // {"req":"card.location.mode"}
    {
        auto result = nc.execute(note::api::CardLocationMode::Get{});
        if (result) {
            auto mode = result.mode;  // "periodic", "continuous", "fixed", or "off"
            (void)mode;
        }
    }

    // ─── Reset to defaults ──────────────────────────────────────────────
    // {"req":"card.location.mode","delete":true}
    {
        nc.execute(note::api::CardLocationMode::Delete{});
    }

    // ─── Location tracking to a Notefile ────────────────────────────────
    // Start recording location data to a tracking Notefile. The Notecard
    // adds a note with each GPS fix, and heartbeat notes at a longer
    // interval even when the device hasn't moved.
    // {"req":"card.location.track","start":true,"heartbeat":true,"hours":12}
    {
        note::api::CardLocationTrack req;
        req.start(true);
        req.heartbeat(true);
        req.hours(12);  // heartbeat every 12 hours even if stationary
        nc.execute(req);
    }

    // Stop tracking.
    // {"req":"card.location.track","stop":true}
    {
        note::api::CardLocationTrack req;
        req.stop(true);
        nc.execute(req);
    }
}

int main() {
    // These examples use -fsyntax-only (compilation check, no linking).
    // On real hardware, pass a real Notecard instance.
    (void)examples;
}
