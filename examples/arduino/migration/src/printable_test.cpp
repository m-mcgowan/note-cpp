// Verify all Printable types compile with Serial.print().
// Catches ambiguity between Printable and String overloads.

#define NOTE_USING_NAMESPACE 0
#include <note.hpp>
using Notecard = note::arduino::Notecard<>;
using namespace note::literals;

namespace printable_test {
static Notecard nc;

void test_response_fields() {
    auto r = nc.card.version().execute();
    if (r) {
        Serial.println(r.version);     // ResponseField<string_view>
        Serial.println(r.device);
        Serial.println(r.wifi);        // ResponseField<bool>
    }
}

void test_error_info() {
    auto r = nc.card.version().execute();
    if (!r) {
        Serial.println(r.error());     // ErrorInfo
    }
}

void test_printable_wrapper() {
    auto r = nc.card.version().execute();
    Serial.println(note::printable(r));      // ApiResult via note::printable()
}

void test_request_fields() {
    auto req = nc.hub.set();
    req.product = "com.example.app";
    Serial.println(note::printable(req.product));  // via note::printable() — no vtable cost
}

void test_duration_literals() {
    (void)5_s;
    (void)5_seconds;
    (void)5_mins;
    (void)5_minutes;
    (void)5_hours;
    (void)5_days;
}

} // namespace printable_test
