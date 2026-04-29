// Binary size comparison: note-cpp side.
//
// Minimal but realistic app: configure hub, register a template,
// read temperature, publish a note. Same operations as main_notec.cpp.
//
// Uses cJSON backend (heap-allocated, like note-c) for a like-for-like
// comparison. The BufferJsonBackend alternative eliminates heap allocation
// but adds ~1.7 KB of static RAM.

#if 1

// Explicit includes (not <note.hpp>) because this uses the cJSON heap
// backend and raw SerialHal for a like-for-like comparison with note-c.
#include <note/backends/cjson.hpp>
#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/arduino/serial.hpp>
#include <note/protocol.hpp>
#include <note/transport/serial.hpp>

static note::backends::CjsonBackend backend;
static note::arduino::SerialHal<HardwareSerial> hal(Serial1, 9600);
static note::transport::NotecardSerial<> serial_transport(hal);
static note::Protocol streaming(serial_transport);
// Protocol directly satisfies ITransact — the buffered
// Notecard ctor pulls bytes via the new transact(req, span, …) overload.
// Notecard owns the response staging buffer (NOTE_RSP_BUF_SIZE).
static note::Notecard notecard(backend, streaming);
static note::Api nc(notecard);

void setup() {
    Serial.begin(115200);
    Serial1.begin(9600);

    // Configure connection
    nc.hub.set()
        .product("com.example.size-test")
        .mode("periodic")
        .outbound(60)
        .execute();

    // Register template
    notecard.request("note.template", [](note::JsonBuilder& b) {
        b.add("file", note::string_view("sensors.qo"));
        b.begin_object("body");
        b.add("temperature", 14.1);
        b.add("humidity", int32_t{1});
        b.end_object();
    });
}

void loop() {
    // Read temperature
    auto temp = nc.card.temp().read().execute();
    float temperature = 0;
    if (temp) {
        temperature = temp.value;
    }

    // Publish sensor data
    notecard.request("note.add", [&](note::JsonBuilder& b) {
        b.add("file", note::string_view("sensors.qo"));
        b.begin_object("body");
        b.add("temperature", static_cast<double>(temperature));
        b.add("humidity", int32_t{60});
        b.end_object();
    });

    delay(60000);
}

#endif // USE_NOTECPP
