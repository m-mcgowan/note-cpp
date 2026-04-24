// Strict old-firmware target can't call card.illumination (needs 9.1.1).
#include <note/api.hpp>
using OldFwStrict = note::MinFirmware<5, 0, 0, true>;
void test(note::Api<OldFwStrict>& api) { api.card.illumination(); }
