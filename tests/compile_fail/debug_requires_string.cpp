// Compile-fail: NOTE_DEBUG_ENABLED=1 with NOTE_NO_STD_STRING=1 must error.
// Debug wire capture uses std::string, so these flags are incompatible.

#define NOTE_NO_STD_STRING 1
#define NOTE_DEBUG_ENABLED 1
#include <note/note_config.hpp>

void test() {}
