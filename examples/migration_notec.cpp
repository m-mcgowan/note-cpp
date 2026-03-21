// migration_notec.cpp — note-arduino (note-c) examples for the migration guide.
//
// Every note-c snippet in docs/migration-from-note-arduino.md comes from
// this file, verified by CI. The note-cpp snippets are in migration.cpp.
//
// This compiles against the note-arduino API using a mock Notecard that
// stubs out the transport layer. It verifies the note-c examples are
// syntactically correct without requiring real hardware.
//
// Build: c++ -std=c++17 -I ../note-arduino/src -I ../note-c examples/migration_notec.cpp

// Stub out the Notecard class so we can compile without Arduino runtime.
// We only need the J* API (from note-c) and the Notecard wrapper methods.
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Minimal note-c stubs for compilation
extern "C" {

typedef struct _J {
    char type;
} J;

static J _stub_obj;

J* JCreateObject(void) { return &_stub_obj; }
J* NoteNewRequest(const char*) { return &_stub_obj; }
J* NoteNewCommand(const char*) { return &_stub_obj; }
void JAddStringToObject(J*, const char*, const char*) {}
void JAddNumberToObject(J*, const char*, double) {}
void JAddBoolToObject(J*, const char*, bool) {}
J* JAddObjectToObject(J*, const char*) { return &_stub_obj; }
J* JAddArrayToObject(J*, const char*) { return &_stub_obj; }
void JAddItemToObject(J*, const char*, J*) {}
void JAddItemToArray(J*, J*) {}
J* JCreateStringArray(const char**, int) { return &_stub_obj; }
char* JGetString(J*, const char*) { return const_cast<char*>(""); }
double JGetNumber(J*, const char*) { return 0.0; }
bool JGetBool(J*, const char*) { return false; }
bool NoteRequest(J*) { return true; }
J* NoteRequestResponse(J*) { return &_stub_obj; }
bool NoteResponseError(J*) { return false; }
void NoteDeleteResponse(J*) {}
void JDelete(J*) {}

// note-c template type constants
#define TBOOL           true
#define TINT8           11
#define TINT16          12
#define TINT24          13
#define TINT32          14
#define TINT64          18
#define TFLOAT16        12.1
#define TFLOAT32        14.1
#define TFLOAT64        18.1

} // extern "C"

// Minimal Notecard class stub (mirrors note-arduino's Notecard.h)
struct Serial_t {
    void println(const char* s) { std::printf("  %s\n", s); }
    void println(double v) { std::printf("  %f\n", v); }
};
static Serial_t Serial;

class Notecard {
public:
    void begin(Serial_t&, int) {}
    J* newRequest(const char* req) { return NoteNewRequest(req); }
    J* newCommand(const char* req) { return NoteNewCommand(req); }
    bool sendRequest(J* req) { return NoteRequest(req); }
    J* requestAndResponse(J* req) { return NoteRequestResponse(req); }
    void deleteResponse(J* rsp) { NoteDeleteResponse(rsp); }
    bool responseError(J* rsp) { return NoteResponseError(rsp); }
};


// ════════════════════════════════════════════════════════════════════════
// Migration guide examples — note-c side
// ════════════════════════════════════════════════════════════════════════

Notecard nc;

void setup() {
    nc.begin(Serial, 9600);
}


// ── hub.set ─────────────────────────────────────────────────────────────

void hub_set() {
    J *req = nc.newRequest("hub.set");
    JAddStringToObject(req, "product",
        "com.example.app");
    JAddStringToObject(req, "mode", "periodic");
    JAddNumberToObject(req, "outbound", 60);
    nc.sendRequest(req);
}

void hub_set_conditional() {
    bool use_continuous = false;
    J *req = nc.newRequest("hub.set");
    JAddStringToObject(req, "product",
        "com.example.app");
    JAddNumberToObject(req, "outbound", 60);
    if (use_continuous) {
        JAddStringToObject(req, "mode",
            "continuous");
        JAddBoolToObject(req, "sync", true);
    } else {
        JAddStringToObject(req, "mode",
            "periodic");
    }
    nc.sendRequest(req);
}


// ── note.add ────────────────────────────────────────────────────────────

struct Readings {
    float temperature;
    int16_t humidity;
};

void note_add() {
    Readings r{.temperature = 22.5f, .humidity = 60};
    J *req = nc.newRequest("note.add");
    JAddStringToObject(req, "file", "sensors.qo");
    J *body = JAddObjectToObject(req, "body");
    JAddNumberToObject(body, "temp", r.temperature);
    JAddNumberToObject(body, "humidity", r.humidity);
    nc.sendRequest(req);
}

void note_add_error_handling() {
    Readings r{.temperature = 22.5f, .humidity = 60};
    J *req = nc.newRequest("note.add");
    JAddStringToObject(req, "file", "sensors.qo");
    J *body = JAddObjectToObject(req, "body");
    JAddNumberToObject(body, "temp", r.temperature);
    JAddNumberToObject(body, "humidity", r.humidity);
    J *rsp = nc.requestAndResponse(req);
    if (rsp == NULL) {
        Serial.println("no response");
    } else if (nc.responseError(rsp)) {
        Serial.println(JGetString(rsp, "err"));
        nc.deleteResponse(rsp);
    } else {
        nc.deleteResponse(rsp);
    }
}


// ── note.template ───────────────────────────────────────────────────────

void note_template() {
    J *req = nc.newRequest("note.template");
    JAddStringToObject(req, "file", "sensors.qo");
    J *body = JAddObjectToObject(req, "body");
    JAddNumberToObject(body, "temp", TFLOAT32);
    JAddNumberToObject(body, "humidity", TINT16);
    nc.sendRequest(req);
}


// ── card.temp ───────────────────────────────────────────────────────────

void card_temp() {
    J *rsp = nc.requestAndResponse(
        nc.newRequest("card.temp"));
    if (rsp == NULL) {
        Serial.println("no response");
    } else if (nc.responseError(rsp)) {
        Serial.println(JGetString(rsp, "err"));
        nc.deleteResponse(rsp);
    } else {
        double temp = JGetNumber(rsp, "value");
        Serial.println(temp);
        nc.deleteResponse(rsp);
    }
}


// ── card.version ────────────────────────────────────────────────────────

void card_version() {
    J *rsp = nc.requestAndResponse(
        nc.newRequest("card.version"));
    if (rsp != NULL) {
        char *ver = JGetString(rsp, "version");
        char *dev = JGetString(rsp, "device");
        Serial.println(ver);
        nc.deleteResponse(rsp);
    }
}


// ── card.attn ───────────────────────────────────────────────────────────

void card_attn_arm() {
    J *req = nc.newRequest("card.attn");
    JAddStringToObject(req, "mode",
        "arm,connected,motion");
    JAddNumberToObject(req, "seconds", 120);
    nc.sendRequest(req);
}

void card_attn_disarm() {
    J *req = nc.newRequest("card.attn");
    JAddStringToObject(req, "mode",
        "disarm,-all");
    nc.sendRequest(req);
}

void card_attn_sleep() {
    J *req = nc.newCommand("card.attn");
    JAddStringToObject(req, "mode", "sleep");
    JAddNumberToObject(req, "seconds", 3600);
    JAddStringToObject(req, "payload",
        "checkpoint-v1");
    nc.sendRequest(req);
}

void card_attn_retrieve() {
    J *req = nc.newRequest("card.attn");
    JAddBoolToObject(req, "start", true);
    J *rsp = nc.requestAndResponse(req);
    if (rsp != NULL) {
        char *payload = JGetString(rsp, "payload");
        if (payload && payload[0]) {
            // Resume from saved state
        }
        nc.deleteResponse(rsp);
    }
}


// ── env.get ─────────────────────────────────────────────────────────────

void env_get() {
    J *req = nc.newRequest("env.get");
    JAddStringToObject(req, "name", "interval");
    J *rsp = nc.requestAndResponse(req);
    if (rsp != NULL) {
        char *text = JGetString(rsp, "text");
        int interval = atoi(text);
        (void)interval;
        nc.deleteResponse(rsp);
    }
}

void env_set_default() {
    J *req = nc.newRequest("env.default");
    JAddStringToObject(req, "name", "interval");
    JAddStringToObject(req, "text", "60");
    nc.sendRequest(req);
}


// ── error handling ──────────────────────────────────────────────────────

void error_handling() {
    J *rsp = nc.requestAndResponse(
        nc.newRequest("card.version"));
    if (rsp == NULL) {
        Serial.println("no response");
    } else if (nc.responseError(rsp)) {
        char *err = JGetString(rsp, "err");
        Serial.println(err);
        nc.deleteResponse(rsp);
    } else {
        // use response...
        nc.deleteResponse(rsp);
    }
}


// ── fire-and-forget command ─────────────────────────────────────────────

void hub_sync_command() {
    J *req = nc.newCommand("hub.sync");
    nc.sendRequest(req);
}


int main() {
    setup();
    hub_set();
    hub_set_conditional();
    note_add();
    note_add_error_handling();
    note_template();
    card_temp();
    card_version();
    card_attn_arm();
    card_attn_disarm();
    card_attn_sleep();
    card_attn_retrieve();
    env_get();
    env_set_default();
    error_handling();
    hub_sync_command();
    std::puts("All note-c migration examples compiled.");
    return 0;
}
