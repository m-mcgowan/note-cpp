// Sufficient-firmware target calling firmware-gated endpoint — clean.
// `-Werror=deprecated-declarations` catches regressions where a valid
// firmware is wrongly flagged as too old.
#include <note/api.hpp>
using NewFw = note::MinFirmware<9, 1, 1>;
void test(note::Api<NewFw>& api) { api.card.illumination(); api.card.version(); }
