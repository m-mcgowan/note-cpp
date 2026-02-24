// Getting started with note-cpp: common Notecard operations.
//
// Each example shows the type-safe C++ equivalent of the JSON requests
// from the Blues Notecard API documentation:
//   https://dev.blues.io/api-reference/notecard-api/introduction/
//
// Build: c++ -std=c++2b -I include examples/getting_started.cpp

#include <note/api_context.hpp>
#include <note/api/hub_set.hpp>
#include <note/api/hub_sync.hpp>
#include <note/api/hub_status.hpp>
#include <note/api/card_version.hpp>
#include <note/api/card_status.hpp>
#include <note/api/card_wireless.hpp>
#include <note/api/note_add.hpp>
#include <note/api/note_get.hpp>
#include <note/api/env_set.hpp>
#include <note/api/env_get.hpp>

// Users provide their own JsonBackend and NotecardIO implementations.
// See examples/smoke.cpp for mock implementations.
void examples(note::Notecard& nc) {
    // Create an Api instance bound to this notecard.
    note::Api api(nc);

    // -----------------------------------------------------------------------
    // Product configuration
    // https://dev.blues.io/api-reference/notecard-api/hub-requests/#hub-set
    // -----------------------------------------------------------------------

    // Fluent factory chain:
    // {"req":"hub.set","mode":"periodic","outbound":60,"product":"com.example.app"}
    api.hubSet()
       .set_product("com.example.app")
       .set_mode("periodic")
       .set_outbound(60)
       .execute();

    // Direct field assignment:
    // {"req":"hub.set","mode":"continuous"}
    {
        auto req = api.hubSet();
        req.mode = "continuous";
        req.execute();
    }

    // Designated initializers (fields in declaration order):
    // {"req":"hub.set","mode":"minimum","product":"com.example.app"}
    api.execute(note::api::HubSet{.mode = "minimum", .product = "com.example.app"});

    // Compile-time validated enum values:
    {
        auto req = api.hubSet();
        req.mode = note::api::HubSet::validated_mode("periodic");  // OK
        // req.mode = note::api::HubSet::validated_mode("typo");   // COMPILE ERROR
        req.execute();
    }

    // -----------------------------------------------------------------------
    // Syncing
    // https://dev.blues.io/api-reference/notecard-api/hub-requests/#hub-sync
    // -----------------------------------------------------------------------

    // {"req":"hub.sync"}
    api.hubSync().execute();

    // -----------------------------------------------------------------------
    // Device info
    // https://dev.blues.io/api-reference/notecard-api/card-requests/#card-version
    // -----------------------------------------------------------------------

    // {"req":"card.version"}
    {
        auto result = api.cardVersion().execute();
        if (result) {
            note::string_view version = result->version;
            note::string_view device = result->device;
            (void)version;
            (void)device;
        }
    }

    // -----------------------------------------------------------------------
    // Device status
    // https://dev.blues.io/api-reference/notecard-api/card-requests/#card-status
    // -----------------------------------------------------------------------

    // {"req":"card.status"}
    {
        auto result = api.cardStatus().execute();
        if (result) {
            auto usb = result->usb;
            auto storage = result->storage;
            (void)usb;
            (void)storage;
        }
    }

    // -----------------------------------------------------------------------
    // Adding Notes (outbound data)
    // https://dev.blues.io/api-reference/notecard-api/note-requests/#note-add
    // -----------------------------------------------------------------------

    // {"req":"note.add","body":"{\"temp\":22.5}","file":"sensors.qo","sync":true}
    {
        auto req = api.noteAdd();
        req.file = "sensors.qo";
        req.body = R"({"temp":22.5})";  // V1: body is opaque JSON string
        req.sync = true;
        req.execute();
    }

    // -----------------------------------------------------------------------
    // Reading Notes (inbound data)
    // https://dev.blues.io/api-reference/notecard-api/note-requests/#note-get
    // -----------------------------------------------------------------------

    // {"req":"note.get","file":"data.qi"} — query without deleting
    {
        auto result = api.noteGet().query().set_file("data.qi").execute();
        if (result) {
            auto payload = result->payload;
            (void)payload;
        }
    }

    // {"req":"note.get","delete":true,"file":"requests.qi"} — pop from queue
    api.noteGet().delete_().set_file("requests.qi").execute();

    // -----------------------------------------------------------------------
    // Environment variables
    // https://dev.blues.io/api-reference/notecard-api/env-requests/#env-set
    // -----------------------------------------------------------------------

    // Designated initializers:
    // {"req":"env.set","name":"interval","text":"300"}
    api.execute(note::api::EnvSet{.name = "interval", .text = "300"});

    // -----------------------------------------------------------------------
    // Fire-and-forget commands
    // https://dev.blues.io/api-reference/notecard-api/hub-requests/#hub-set
    // -----------------------------------------------------------------------

    // {"cmd":"hub.set","product":"com.example.app"}
    {
        auto req = api.hubSet();
        req.product = "com.example.app";
        req.command();
    }

    // -----------------------------------------------------------------------
    // Standalone usage (without Api factory)
    // -----------------------------------------------------------------------

    // req.execute(nc) — pass notecard explicitly:
    {
        note::api::CardVersion req;
        auto result = req.execute(nc);
        (void)result;
    }

    // nc.execute(req) — original style:
    {
        note::api::HubSet req;
        req.set_product("test");
        nc.execute(req);
    }

    // Ad-hoc request (for anything not yet generated):
    {
        auto result = nc.request("card.version");
        if (result) {
            auto version = (*result)->get_string("version");
            (void)version;
        }
    }
}

int main() {
    // This file is for illustration only — requires real backend and IO
    // implementations to compile and link. See examples/smoke.cpp for
    // a compilable mock example.
    return 0;
}
