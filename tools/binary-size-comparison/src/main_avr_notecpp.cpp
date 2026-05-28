// Binary size comparison: note-cpp on AVR.
//
// Realistic 8-endpoint app: configure, template, read sensors, publish,
// check status, read voltage, read inbound notes, get environment vars.
//
// API_STYLE selects the developer experience vs code size trade-off:
//   1 = Api groups:   api.hub.set().product(...).execute()
//   2 = Direct:       nc.execute(req)
//   3 = Raw JSON:     JsonBuf + transact_dispatch + custom JsonSink  (SAX — low RAM)
//   4 = Raw JSON:     JsonBuf + transact_raw + JsonView               (no SAX — low flash)

// Guarded by USE_NOTECPP_AVR to prevent accidental compilation
// if src_filter config changes — defensive + self-documenting.
#ifdef USE_NOTECPP_AVR

#ifndef API_STYLE
#define API_STYLE 1
#endif

// Serial-only AVR app — opt out of Wire.h to keep flash budget tight.
// Without this, <note/arduino.hpp> auto-includes <Wire.h> via __has_include.
// Link-time GC drops unused I2C overloads anyway, but the source-level
// suppression gives a hard guarantee for byte-budget tracking.
#define NOTE_ARDUINO_NO_WIRE
#include <note.hpp>
#if API_STYLE == 3
#include <note/json_sax.hpp>  // JsonSink, parse_int, parse_double
#elif API_STYLE == 4
#include <note/json_view.hpp> // JsonView (no SAX parser)
#endif

// K(s) — "scan key" helper. With USE_FLASH_KEYS=1, uses F(s) so the
// key lives in PROGMEM. FlashString has an implicit conversion from
// `const __FlashStringHelper*`, so the F() result routes straight to
// the flash-key overloads without an explicit `note::flash()` wrapper.
// Without USE_FLASH_KEYS, the literal is a plain string_view (in .data —
// RAM-backed on AVR).
#if defined(USE_FLASH_KEYS) && USE_FLASH_KEYS
#define K(s) F(s)
#else
#define K(s) s
#endif

struct Readings {
    float temperature;
    int32_t humidity;
    NOTE_FIELDS(temperature, humidity)
};

using UsedRequests = note::RequestSet<
    note::api::HubSet,
    note::api::NoteTemplate::Define,
    note::api::CardTemp::Read,
    note::api::NoteAdd,
    note::api::CardStatus,
    note::api::CardVoltage::Read,
    note::api::NoteGet::Read,
    note::api::EnvGet
>;
static note::StaticArena<UsedRequests> arena;

using SerialNotecard = note::StaticNotecard<note::arduino::SerialTransportStack<>>;
#if defined(ARDUINO_AVR_MEGA2560)
// Mega has multiple UARTs; use Serial1 for the Notecard to leave Serial
// available as a console (and to side-step the USB-Serial bridge on pins 0/1).
static SerialNotecard nc(note::arena_allocator(arena), Serial1, 9600);
#else
static SerialNotecard nc(note::arena_allocator(arena), Serial, 9600);
#endif

#if API_STYLE == 1
static note::Api<SerialNotecard> api(nc);
#endif

void setup() {
#if defined(ARDUINO_AVR_MEGA2560)
    Serial.begin(115200);  // debug console on Mega (Notecard is on Serial1)
#endif

#if API_STYLE == 1
    api.hub.set()
        .product("com.example.size-test")
        .mode("periodic")
        .outbound(60)
        .execute();

    api.note.templates().define("sensors.qo")
        .body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", 1);
        }))
        .execute();
#elif API_STYLE == 2
    {
        note::api::HubSet req;
        req.product = "com.example.size-test";
        req.mode = "periodic";
        req.outbound = 60;
        nc.execute(req);
    }
    {
        note::api::NoteTemplate::Define req;
        req.file = "sensors.qo";
        req.body(note::body([](note::JsonBuilder& b) {
            b.add("temperature", 14.1);
            b.add("humidity", 1);
        }));
        nc.execute(req);
    }
#elif API_STYLE == 3
    // Raw JSON via JsonBuf + transact_raw() for the SAX-sink path. Style 4
    // uses transact_raw_inplace below; the SAX path keeps the separate-buffer
    // shape because transact_dispatch (used for note.get) drives its own sink.
    {
        note::JsonBuf<128> req;
        req.add("req", "hub.set");
        req.add("product", "com.example.size-test");
        req.add("mode", "periodic");
        req.add("outbound", 60);
        req.close();
        char rsp[128];
        nc.transact_raw(req, rsp);
    }
    {
        note::JsonBuf<128> req;
        req.add("req", "note.template");
        req.add("file", "sensors.qo");
        req.begin_object("body");
            req.add("temperature", 14.1);
            req.add("humidity", 1);
        req.end_object();
        req.close();
        char rsp[128];
        nc.transact_raw(req, rsp);
    }
#elif API_STYLE == 4
    // Raw JSON + JsonView, single-buffer in-place: render the request into
    // `buf` via the lambda, then receive the response into the same `buf`.
    {
        char buf[128];
        nc.transact_raw_inplace(buf, [](auto& w) {
            w.add("req", "hub.set");
            w.add("product", "com.example.size-test");
            w.add("mode", "periodic");
            w.add("outbound", 60);
        });
    }
    {
        char buf[128];
        nc.transact_raw_inplace(buf, [](auto& w) {
            w.add("req", "note.template");
            w.add("file", "sensors.qo");
            w.begin_object("body");
                w.add("temperature", 14.1);
                w.add("humidity", 1);
            w.end_object();
        });
    }
#endif
}

void loop() {
    arena.reset();
    // Read temperature
#if API_STYLE == 1
    auto temp = api.card.temp().read().execute();
#elif API_STYLE == 2
    auto temp = nc.execute(note::api::CardTemp::Read{});
#endif
#if API_STYLE <= 2
    float temperature = temp ? temp.value : 0;
#elif API_STYLE == 3
    float temperature = 0;  // raw+SAX: response parsing done via BodySink in note.get only
    {
        note::JsonBuf<64> req;
        req.add("req", "card.temp");
        req.close();
        char rsp[128];
        nc.transact_raw(req, rsp);
    }
#else   // API_STYLE == 4
    float temperature = 0;
    {
        char buf[64];
        temperature = note::JsonView(
            nc.transact_raw_inplace(buf, [](auto& w) {
                w.add("req", "card.temp");
            })
        ).get_float(K("value"));
    }
#endif

    // Publish sensor data
    Readings out{temperature, 60};
#if API_STYLE == 1
    api.note.add()
        .file("sensors.qo")
        .body(out)
        .execute();
#elif API_STYLE == 2
    {
        note::api::NoteAdd req;
        req.file = "sensors.qo";
        req.body(out);
        nc.execute(req);
    }
#elif API_STYLE == 3
    {
        note::JsonBuf<128> req;
        req.add("req", "note.add");
        req.add("file", "sensors.qo");
        req.begin_object("body");
            req.add("temperature", out.temperature);
            req.add("humidity", out.humidity);
        req.end_object();
        req.close();
        char rsp[64];
        nc.transact_raw(req, rsp);
    }
#else   // API_STYLE == 4
    {
        char buf[128];
        nc.transact_raw_inplace(buf, [&](auto& w) {
            w.add("req", "note.add");
            w.add("file", "sensors.qo");
            w.begin_object("body");
                w.add("temperature", out.temperature);
                w.add("humidity", out.humidity);
            w.end_object();
        });
    }
#endif

    // Check connection status
#if API_STYLE == 1
    auto status = api.card.status().execute();
#elif API_STYLE == 2
    auto status = nc.execute(note::api::CardStatus{});
#endif
#if API_STYLE <= 2
    bool connected = status && status.connected;
#elif API_STYLE == 3
    bool connected = false;
    {
        note::JsonBuf<64> req;
        req.add("req", "card.status");
        req.close();
        char rsp[128];
        nc.transact_raw(req, rsp);
    }
#else   // API_STYLE == 4
    bool connected = false;
    {
        char buf[64];
        connected = note::JsonView(
            nc.transact_raw_inplace(buf, [](auto& w) {
                w.add("req", "card.status");
            })
        ).get_bool(K("connected"));
    }
#endif

    // Read battery voltage
#if API_STYLE == 1
    auto volt = api.card.voltage().read().execute();
#elif API_STYLE == 2
    auto volt = nc.execute(note::api::CardVoltage::Read{});
#endif
#if API_STYLE <= 2
    double voltage = volt ? volt.value : 0;
#elif API_STYLE == 3
    double voltage = 0;
    {
        note::JsonBuf<64> req;
        req.add("req", "card.voltage");
        req.close();
        char rsp[128];
        nc.transact_raw(req, rsp);
    }
#else   // API_STYLE == 4
    double voltage = 0;
    {
        char buf[64];
        voltage = note::JsonView(
            nc.transact_raw_inplace(buf, [](auto& w) {
                w.add("req", "card.voltage");
            })
        ).get_double(K("value"));
    }
#endif

    // Read inbound note body
    Readings note_body{};
#if API_STYLE == 1
    auto note_in = api.note.read("config.qi").into(note_body).execute();
#elif API_STYLE == 2
    {
        note::api::NoteGet::Read req;
        req.file = "config.qi";
        req.into(note_body);
        auto note_in = nc.execute(req);
        (void)note_in;
    }
#elif API_STYLE == 3
    // Raw JSON + custom JsonSink: transact_dispatch streams the response
    // through the SAX parser. Lowest RAM (no response buffer), but pulls
    // in the full SAX machinery (~8 KB flash on AVR).
    {
        struct BodySink : note::JsonSink {
            Readings* out;
            int depth = 0;
            void on_object_begin(note::string_view k) override {
                if (k == "body") depth = 1;
                else if (depth) ++depth;
            }
            void on_object_end(note::string_view) override { if (depth) --depth; }
            void on_number(note::string_view k, note::string_view raw) override {
                if (depth != 1) return;
                if (k == "temperature") out->temperature = static_cast<float>(note::parse_double(raw));
                else if (k == "humidity") out->humidity = static_cast<int32_t>(note::parse_int(raw));
            }
        };
        BodySink sink; sink.out = &note_body;
        note::detail::NcErrorCapture err;
        note::BuildFn build_fn = [](note::JsonBuilder& b, void*) {
            b.add("req", "note.get");
            b.add("file", "config.qi");
        };
        note::BuildFnRequestSource src(build_fn, nullptr);
        nc.stack().transport.transact_dispatch(
            src.as_source(),
            note::make_sax_dispatch(sink), 10000, err);
    }
#else   // API_STYLE == 4
    // Raw JSON + JsonView: response is scanned in place from the same
    // buffer used to render the request. No SAX parser — ~8 KB flash
    // cheaper than STYLE 3, and a single buffer instead of req+rsp.
    {
        char buf[128];
        auto body = note::JsonView(
            nc.transact_raw_inplace(buf, [](auto& w) {
                w.add("req", "note.get");
                w.add("file", "config.qi");
            })
        ).object(K("body"));
        note_body.temperature = body.get_float(K("temperature"));
        note_body.humidity    = static_cast<int32_t>(body.get_int(K("humidity")));
    }
#endif

    // Read environment variable
#if API_STYLE == 1
    auto env = api.env.get().execute();
#elif API_STYLE == 2
    auto env = nc.execute(note::api::EnvGet{});
#elif API_STYLE == 3
    {
        note::JsonBuf<64> req;
        req.add("req", "env.get");
        req.close();
        char rsp[256];
        nc.transact_raw(req, rsp);
    }
#else   // API_STYLE == 4
    {
        char buf[256];
        nc.transact_raw_inplace(buf, [](auto& w) {
            w.add("req", "env.get");
        });
    }
#endif

    (void)connected;
    (void)voltage;
    (void)note_body;

#if defined(ARDUINO_AVR_MEGA2560)
    // On Mega, Serial is free (Notecard is on Serial1). Print extracted
    // values so end-to-end scan/parse correctness can be observed in Wokwi.
    if (!Serial) Serial.begin(115200);
    Serial.print(F("temp=")); Serial.print(note_body.temperature);
    Serial.print(F(" humidity=")); Serial.print(note_body.humidity);
    Serial.print(F(" voltage=")); Serial.print(voltage);
    Serial.print(F(" connected=")); Serial.println(connected);
#endif

    delay(60000);
}

#endif
