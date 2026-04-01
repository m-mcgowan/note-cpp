// Binary size comparison: note-cpp on AVR.
//
// Uses BufferJsonBackend (zero heap, no external JSON library) with
// fixed buffers for the streaming transport path. No std::string needed.
//
// Arduino Uno has only one HW serial (Serial), shared with Notecard.
// Same transport as the note-c AVR example for a fair comparison.

#ifdef USE_NOTECPP_AVR

// NOTE_NO_STD_STRING is set via build_flags, before any includes.

#include <note/backends/buffer.hpp>
#include <note/notecard.hpp>
#include <note/api.hpp>
#include <note/arduino/serial.hpp>

// Streaming build means the backend's build buffer is unused — minimize it.
// RX buffer holds the full JSON response for parsing.
static char rx_buf[96];

static note::backends::BufferJsonBackend<1, 12> backend;
static note::arduino::SerialHal<HardwareSerial> hal(Serial, 9600);
static note::transport::NotecardSerial serial_transport(hal);
static note::Notecard notecard(backend, serial_transport);
static note::Api nc(notecard);

void setup() {
    Serial.begin(9600);

    // Use fixed buffer for the streaming path (avoids std::string)
    serial_transport.set_receive_buffer(rx_buf, sizeof(rx_buf));

    // Configure connection
    nc.hub.set()
        .product("com.example.size-test")
        .mode("periodic")
        .outbound(60)
        .execute();
}

void loop() {
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
