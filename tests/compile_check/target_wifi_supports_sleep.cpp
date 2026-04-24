// WiFi target (non-strict) calling WiFi-supported endpoints — clean.
// Compile-check runs with `-Werror=deprecated-declarations`, so an
// unexpected deprecation on a supported endpoint would fail the test.
#include <note/api.hpp>
using WifiTarget = note::Target<note::Radios::WiFi>;
void test(note::Api<WifiTarget>& api) { api.card.sleep(); api.hub.set(); }
