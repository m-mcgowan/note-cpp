// README snippet source — compiled by CI so the code blocks in README.md
// can't drift from working, compilable sketches.
//
// Each `// readme:<name>` ... `// readme:end` region corresponds to a
// `<!-- snippet:<name> examples/arduino/readme_snippets/readme_snippets.ino -->`
// marker in README.md. Running `tools/inject-snippets.py --check` asserts
// the two match.
//
// Users: open examples/arduino/quickstart/ for a clean intro sketch.
// This file exists for doc-verification only.

// readme:arduino-quickstart
#include <note.hpp>
// readme:end

// readme:body-struct-def
struct Readings {
    float temperature;
    int16_t humidity;
    NOTE_FIELDS(temperature, humidity)  // optional on C++20
};
// readme:end

// readme:arduino-declare
Notecard nc;
// readme:end

void setup() {
    // readme:arduino-setup
    nc.begin(Serial1, 9600);       // serial — or nc.begin(Wire) for I2C

    nc.hub.set()
        .product("com.example.app")
        .mode("periodic")
        .execute();
    // readme:end

    // readme:fluent-api
    nc.hub.set()
       .product("com.example.app")
       .mode("periodic")
       .outbound(60_mins)
       .execute();
    // readme:end

    // readme:direct-assignment
    auto req = nc.hub.set();
    req.product = "com.example.app";
    req.mode = "periodic";
    req.outbound = 60;
    req.execute();
    // readme:end

    // readme:body-send
    Readings readings{.temperature = 22.5f, .humidity = 60};
    nc.note.add()
       .file("sensors.qo")
       .body(readings)
       .execute();
    // readme:end

    // readme:body-receive
    Readings data{};
    nc.note.pop("sensors.qi").into(data).execute();
    // readme:end

#if __cplusplus >= 202002L
    // readme:body-template
    nc.note.templates().define("sensors.qo").body(template_of(Readings())).execute();
    // readme:end
#endif

    // readme:read-response
    auto rsp = nc.card.version().execute();
    if (rsp) {
        Serial.println(rsp.version);
        Serial.println(rsp.device);
    } else {
        Serial.println(rsp.error());
    }
    // readme:end
}

void loop() {
    delay(60000);
}
