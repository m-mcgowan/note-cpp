// Binary size comparison: note-cpp on AVR.
//
// Full streaming path: request JSON streamed directly to transport via
// StreamingJsonBuilder, response SAX-parsed directly from transport.
// No intermediate buffers, no JsonBackend needed.
//
// Arduino Uno has only one HW serial (Serial), shared with Notecard.
// Same transport as the note-c AVR example for a fair comparison.

#ifdef USE_NOTECPP_AVR

// NOTE_NO_STD_STRING is set via build_flags, before any includes.

#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/streaming_transport.hpp>
#include <note/arduino/serial.hpp>
#include <note/arena.hpp>

// Arena for string interning during SAX parse (response string fields).
alignas(4) static char arena_buf[64];
static note::MonotonicArena arena(arena_buf);

static note::arduino::SerialHal<HardwareSerial> hal(Serial, 9600);
static note::transport::NotecardSerial serial_hal(hal);
static note::StreamingTransport transport(serial_hal);
static note::Notecard notecard(transport, note::arena_allocator(arena));
static note::Api nc(notecard);

void setup() {
    Serial.begin(9600);

    // Configure connection
    nc.hub.set()
        .product("com.example.size-test")
        .mode("periodic")
        .outbound(60)
        .execute();
}

void loop() {
    arena.reset();  // reclaim arena between requests

    // Read temperature
    auto temp = nc.card.temp().read().execute();
    float temperature = 0;
    if (temp) {
        temperature = temp.value;
    }

    // Publish sensor data
    nc.note.add()
        .file("sensors.qo")
        .execute();

    delay(60000);
}

#endif // USE_NOTECPP_AVR
