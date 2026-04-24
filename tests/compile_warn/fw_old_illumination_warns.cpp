// Non-strict old-firmware target calling firmware-gated endpoint —
// [[deprecated("requires firmware >= ...")]] fires. Promoted to error
// via `-Werror=deprecated-declarations`.
#include <note/api.hpp>
using OldFw = note::MinFirmware<5, 0, 0>;
void test(note::Api<OldFw>& api) { api.card.illumination(); }
