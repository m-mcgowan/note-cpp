// Non-strict LoRa target calling card.sleep — [[deprecated]] fires.
// Compile-warn runs with `-Werror=deprecated-declarations` so the
// deprecation becomes an error; `WILL_FAIL TRUE` asserts compile-fail.
#include <note/api.hpp>
using LoRaWarn = note::Target<note::Radios::LoRa, 0, 0, 0, false>;
void test(note::Api<LoRaWarn>& api) { api.card.sleep(); }
