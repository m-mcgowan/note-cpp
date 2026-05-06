// `api.card.usageGet()` no longer exists — the flat camelCase form was
// dropped when card.usage became a virtual parent factory. Users must
// reach the request via `api.card.usage.read()` instead.
#include <note/api.hpp>

void test() {
    note::Notecard nc;
    note::Api api(nc);
    api.card.usageGet();  // should fail: usageGet is not a member of CardGroup
}
