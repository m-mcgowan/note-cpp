// Compile-fail: buffered transport path requires std::string support.
// NOTE_NO_BUFFERED=0 with NOTE_NO_STD_STRING=1 must error.

#define NOTE_NO_BUFFERED 0
#define NOTE_NO_STD_STRING 1
#include <note/note_config.hpp>

void test() {}
