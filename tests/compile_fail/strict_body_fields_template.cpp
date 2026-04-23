// Compile-fail: template_of<T>() on a struct whose field type is not
// supported should fail to compile when NOTE_STRICT_BODY_FIELDS is on.

#include <note/body.hpp>

#include <vector>

struct Bad {
    std::vector<int> samples;
};

void test() {
    note::BodyValue body = note::template_of<Bad>();
    (void)body;
}
