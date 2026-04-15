// migration_notec.cpp — note-arduino (note-c) examples for the migration guide.
//
// Every note-c snippet in docs/migration-from-note-arduino.md comes from
// this file, verified by CI. Each function body is unindented so embedme
// line references match the guide exactly.
//
// Build: c++ -std=c++17 examples/migration_notec.cpp && ./a.out

// Note: snippet indentation matches the source; the embedder could normalize it in the future.

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Minimal note-c stubs for compilation
extern "C" {
typedef struct _J { char type; } J;
static J _stub_obj;
J* NoteNewRequest(const char*) { return &_stub_obj; }
J* NoteNewCommand(const char*) { return &_stub_obj; }
void JAddStringToObject(J*, const char*, const char*) {}
void JAddNumberToObject(J*, const char*, double) {}
void JAddBoolToObject(J*, const char*, bool) {}
J* JAddObjectToObject(J*, const char*) { return &_stub_obj; }
char* JGetString(J*, const char*) { return const_cast<char*>(""); }
double JGetNumber(J*, const char*) { return 0.0; }
bool NoteRequest(J*) { return true; }
J* NoteRequestResponse(J*) { return &_stub_obj; }
bool NoteResponseError(J*) { return false; }
void NoteDeleteResponse(J*) {}
#define TFLOAT32 14.1
#define TINT16   12
} // extern "C"

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

Notecard nc;

// ── hub.set ─────────────────────────────────────────────────────────────
void hub_set() {
J *req = nc.newRequest("hub.set");
JAddStringToObject(req, "product",
    "com.example.app");
JAddStringToObject(req, "mode", "periodic");
JAddNumberToObject(req, "outbound", 60);
nc.sendRequest(req);
}

// ── hub.set: conditional ────────────────────────────────────────────────
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
}
else {
    JAddStringToObject(req, "mode", "periodic");
}
nc.sendRequest(req);
}

// ── note.add ────────────────────────────────────────────────────────────
void note_add() {
struct Readings {
    float temperature;
    int16_t humidity;
};

Readings r{.temperature = 22.5f, .humidity = 60};
J *req = nc.newRequest("note.add");
JAddStringToObject(req, "file", "sensors.qo");
J *body = JAddObjectToObject(req, "body");
JAddNumberToObject(body, "temp", r.temperature);
JAddNumberToObject(body, "humidity", r.humidity);
nc.sendRequest(req);
}

// ── note.add: error handling ────────────────────────────────────────────
void note_add_error_handling() {
J *req = nc.newRequest("note.add"); // setup for the error handling snippet
J *rsp = nc.requestAndResponse(req);
if (rsp == NULL) {
    Serial.println("no response");
} else if (nc.responseError(rsp)) {
    // "note.add: queue full" — you parse this yourself
    Serial.println(JGetString(rsp, "err"));
    nc.deleteResponse(rsp);
} else {
    nc.deleteResponse(rsp);
}
}

// ── note.template ───────────────────────────────────────────────────────
void note_template() {
// Type constants from note.h — you pick the
// right one for each field manually.
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

// Configure periodic monitoring
J *req = nc.newRequest("card.temp");
JAddNumberToObject(req, "minutes", 5);
nc.sendRequest(req);
}

// ── card.version ────────────────────────────────────────────────────────
void card_version() {
J *rsp = nc.requestAndResponse(
    nc.newRequest("card.version"));
if (rsp != NULL) {
    char *ver = JGetString(rsp, "version");
    char *dev = JGetString(rsp, "device");
    Serial.println(ver);
    Serial.println(dev);
    nc.deleteResponse(rsp);
}
}

// ── card.attn ───────────────────────────────────────────────────────────
void card_attn() {
// Arm for connectivity + motion triggers
J *req = nc.newRequest("card.attn");
JAddStringToObject(req, "mode",
    "arm,connected,motion");
JAddNumberToObject(req, "seconds", 120);
nc.sendRequest(req);

// Disarm
req = nc.newRequest("card.attn");
JAddStringToObject(req, "mode",
    "disarm,-all");
nc.sendRequest(req);
}

// ── card.attn: sleep ────────────────────────────────────────────────────
void card_attn_sleep() {
// Sleep — save state across reset
J *req = nc.newCommand("card.attn");
JAddStringToObject(req, "mode", "sleep");
JAddNumberToObject(req, "seconds", 3600);
JAddStringToObject(req, "payload", "checkpoint-v1");
nc.sendRequest(req);
// Enter deep sleep...

// On wake — retrieve saved state
{ J *wake = nc.newRequest("card.attn");
JAddBoolToObject(wake, "start", true);
J *rsp = nc.requestAndResponse(wake);
if (rsp != NULL) {
    char *payload = JGetString(rsp, "payload");
    if (payload && payload[0]) {
        // Resume from saved state
    }
    nc.deleteResponse(rsp);
}
} // inner scope for req reuse
}

// ── env.get ─────────────────────────────────────────────────────────────
void env_get() {
// Read a single env var
J *req = nc.newRequest("env.get");
JAddStringToObject(req, "name", "interval");
J *rsp = nc.requestAndResponse(req);
if (rsp != NULL) {
    char *text = JGetString(rsp, "text");
    (void)atoi(text);  // use the value
    nc.deleteResponse(rsp);
}

// Set a default
J *req2 = nc.newRequest("env.default");
JAddStringToObject(req2, "name", "interval");
JAddStringToObject(req2, "text", "60");
nc.sendRequest(req2);
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

// ── fire-and-forget ─────────────────────────────────────────────────────
void hub_sync_command() {
// newCommand sends "cmd" not "req" — Notecard
// doesn't respond. Easy to confuse with newRequest.
J *req = nc.newCommand("hub.sync");
nc.sendRequest(req);
}

int main() {
    hub_set();
    hub_set_conditional();
    note_add();
    // note_add_error_handling uses undefined 'req' — compile-check only
    note_template();
    card_temp();
    card_version();
    card_attn();
    card_attn_sleep();
    env_get();
    error_handling();
    hub_sync_command();
    std::puts("All note-c migration examples compiled.");
    return 0;
}
