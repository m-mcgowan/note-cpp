// Binary size comparison: note-c (note-arduino) side.
//
// Realistic 8-endpoint app: configure, template, read sensors, publish,
// check status, read voltage, read inbound notes, get environment vars.
// Same operations as main_avr_notecpp.cpp.

#ifdef USE_NOTEC

#include <Notecard.h>

Notecard nc;

// ── Heap watermark ──────────────────────────────────────────────────────
extern char *__brkval;
extern char __heap_start;

static uint16_t heap_peak;

static void heap_sample() {
    if (__brkval) {
        uint16_t used = __brkval - &__heap_start;
        if (used > heap_peak) heap_peak = used;
    }
}

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
    heap_sample();

    // Register template
    {
        J *req = nc.newRequest("note.template");
        JAddStringToObject(req, "file", "sensors.qo");
        J *body = JAddObjectToObject(req, "body");
        JAddNumberToObject(body, "temperature", 14.1);
        JAddNumberToObject(body, "humidity", 1);
        nc.sendRequest(req);
    }
    heap_sample();
}

void loop() {
    // Read temperature
    float temperature = 0;
    {
        J *rsp = nc.requestAndResponse(nc.newRequest("card.temp"));
        heap_sample();
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
    heap_sample();

    // Check connection status
    bool connected = false;
    {
        J *rsp = nc.requestAndResponse(nc.newRequest("card.status"));
        heap_sample();
        if (rsp != NULL) {
            connected = JGetBool(rsp, "connected");
            nc.deleteResponse(rsp);
        }
    }

    // Read battery voltage
    double voltage = 0;
    {
        J *rsp = nc.requestAndResponse(nc.newRequest("card.voltage"));
        heap_sample();
        if (rsp != NULL) {
            voltage = JGetNumber(rsp, "value");
            nc.deleteResponse(rsp);
        }
    }

    // Read inbound note body
    float note_temp = 0;
    int note_humidity = 0;
    {
        J *req = nc.newRequest("note.get");
        JAddStringToObject(req, "file", "config.qi");
        J *rsp = nc.requestAndResponse(req);
        heap_sample();
        if (rsp != NULL) {
            J *body = JGetObject(rsp, "body");
            if (body) {
                note_temp = JGetNumber(body, "temperature");
                note_humidity = (int)JGetNumber(body, "humidity");
            }
            nc.deleteResponse(rsp);
        }
    }

    // Read environment variables
    {
        J *rsp = nc.requestAndResponse(nc.newRequest("env.get"));
        heap_sample();
        if (rsp != NULL) {
            nc.deleteResponse(rsp);
        }
    }

    (void)connected;
    (void)voltage;
    (void)note_temp;
    (void)note_humidity;

    // Report peak heap
    Serial.print("HEAP_PEAK:");
    Serial.println(heap_peak);

    delay(60000);
}

#endif // USE_NOTEC
