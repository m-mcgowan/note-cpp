#pragma once

/// @file note_config.hpp
/// Feature flags with defaults. Include this before any other note-cpp header.
///
/// NOTE_MINIMAL — when defined (or 1), sets conservative defaults for all
/// flags to minimize binary size on constrained platforms (AVR, etc.).
/// Individual flags can still be overridden after NOTE_MINIMAL.
///
/// Usage in platformio.ini:
///   build_flags = -DNOTE_MINIMAL      ; sets all size-saving defaults
///
/// Instead of:
///   build_flags = -DNOTE_NO_BUFFERED -DNOTE_NO_STD_STRING -DNOTE_NO_MD5
///                 -DNOTE_NO_CRC -DNOTE_PRINTABLE=0 -DNOTE_EXTRAS=0
///                 -DNOTE_SHORT_ERRORS=1

// NOTE_MINIMAL sets size-saving defaults when not individually overridden.
#ifdef NOTE_MINIMAL

#  ifndef NOTE_NO_BUFFERED
#    define NOTE_NO_BUFFERED
#  endif

#  ifndef NOTE_NO_STD_STRING
#    define NOTE_NO_STD_STRING
#  endif

#  ifndef NOTE_NO_MD5
#    define NOTE_NO_MD5
#  endif

#  ifndef NOTE_NO_CRC
#    define NOTE_NO_CRC
#  endif

#  ifndef NOTE_PRINTABLE
#    define NOTE_PRINTABLE 0
#  endif

#  ifndef NOTE_EXTRAS
#    define NOTE_EXTRAS 0
#  endif

#  ifndef NOTE_SHORT_ERRORS
#    define NOTE_SHORT_ERRORS 1
#  endif

#endif // NOTE_MINIMAL
