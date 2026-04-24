// Combined radios+firmware: WiFi (supported by card.illumination) plus
// firmware too old for its 9.1.1 minimum. Deprecation fires on the
// firmware leg; `-Werror=deprecated-declarations` promotes it to error.
#include <note/api.hpp>
using OldWifi = note::Target<note::Radios::WiFi, 5, 0, 0, false>;
void test(note::Api<OldWifi>& api) { api.card.illumination(); }
