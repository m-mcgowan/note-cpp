// `card.usage` is not a real Notecard request — only `card.usage.get`
// and `card.usage.test` exist upstream. The virtual parent factory has
// no `operator()()`, so `api.card.usage()` must not compile.
#include <note/api.hpp>

void test() {
    note::Notecard nc;
    note::Api api(nc);
    api.card.usage();  // should fail: CardUsageFactory has no operator()
}
