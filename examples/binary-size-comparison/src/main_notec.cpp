// Binary size comparison: note-c (note-arduino) side.
//
// Minimal but realistic app: configure hub, register a template,
// read temperature, publish a note. Same operations as main_notecpp.cpp.

#ifdef USE_NOTEC

#include <Notecard.h>

Notecard nc;

void setup() {
#ifdef ARDUINO_AVR_UNO
    nc.begin(Serial, 9600);
#else
    Serial.begin(115200);
    nc.begin(Serial1, 9600);
#endif

    // Configure connection
    {
        J *req = nc.newRequest("hub.set");
        JAddStringToObject(req, "product", "com.example.size-test");
        JAddStringToObject(req, "mode", "periodic");
        JAddNumberToObject(req, "outbound", 60);
        nc.sendRequest(req);
    }

    // Register template
    {
        J *req = nc.newRequest("note.template");
        JAddStringToObject(req, "file", "sensors.qo");
        J *body = JAddObjectToObject(req, "body");
        JAddNumberToObject(body, "temperature", 14.1);
        JAddNumberToObject(body, "humidity", 1);
        nc.sendRequest(req);
    }
}

void loop() {
    // Read temperature
    float temperature = 0;
    {
        J *rsp = nc.requestAndResponse(nc.newRequest("card.temp"));
        if (rsp != NULL) {
            temperature = JGetNumber(rsp, "value");
            nc.deleteResponse(rsp);
        }
    }

    // Publish sensor data
    {
        J *req = nc.newRequest("note.add");
        JAddStringToObject(req, "file", "sensors.qo");
        J *body = JAddObjectToObject(req, "body");
        JAddNumberToObject(body, "temperature", temperature);
        JAddNumberToObject(body, "humidity", 60);
        nc.sendRequest(req);
    }

    delay(60000);
}

#endif // USE_NOTEC
