// Compile-fail: schema struct with an unsupported field type should fail
// to serialise when NOTE_STRICT_BODY_FIELDS is on (the default).
// std::vector<int> isn't claimed by any SAX handler — ser should trip
// the terminal-else static_assert in write_field_value.

#include <note/body.hpp>

#include <vector>

struct Bad {
    std::vector<int> samples;
};

void test() {
    Bad b;
    note::BodyValue body = note::make_schema_body(b);
    (void)body;
}
