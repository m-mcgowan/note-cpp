// Compile check: <note/api.hpp> must transitively include <note/units.hpp>
// so that literals like 120_s work without a separate include.

#include <note/api.hpp>

using namespace note::literals;

void test() {
    auto s = 120_s;
    auto m = 5_mins;
    auto h = 2_hours;
    (void)s; (void)m; (void)h;
}
