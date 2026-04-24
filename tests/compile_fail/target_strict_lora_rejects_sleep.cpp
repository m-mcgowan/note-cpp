// Strict LoRa target can't call card.sleep (WiFi-only endpoint).
// Expect compile-fail: the [[deprecated]] overload is disabled by
// `requires !T_::strict`, leaving only the supported overload, which
// is constrained out by `requires target_supports<T_, ...>`.
#include <note/api.hpp>
using LoRaStrict = note::Target<note::Radios::LoRa, 0, 0, 0, true>;
void test(note::Api<LoRaStrict>& api) { api.card.sleep(); }
