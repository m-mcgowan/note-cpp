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

#  ifndef NOTE_DEBUG_ENABLED
#    define NOTE_DEBUG_ENABLED 0
#  endif

#  ifndef NOTE_NO_RETRY
#    define NOTE_NO_RETRY
#  endif

#  ifndef NOTE_NO_REQUEST_IDS
#    define NOTE_NO_REQUEST_IDS
#  endif

#  ifndef NOTE_UNICODE_ESCAPES
// NOTE_MINIMAL strips \uXXXX handling. Define NOTE_UNICODE_ESCAPES to keep it.
#  endif

#  ifndef NOTE_INT32_MATH
#    define NOTE_INT32_MATH 1
#  endif

#  ifndef NOTE_SINGLETON
#    define NOTE_SINGLETON 1
#  endif

#  ifndef NOTE_STATIC_HAL
#    define NOTE_STATIC_HAL 1
#  endif

#  ifndef NOTE_MUTABLE_POLICY
#    define NOTE_MUTABLE_POLICY 0
#  endif

#endif // NOTE_MINIMAL

// NOTE_MUTABLE_POLICY — when 1, protocol policy fields are mutable instance
// members (uint32_t, 28 bytes). When 0, fields are static constexpr (zero bytes).
// Default: 1 (mutable). NOTE_MINIMAL sets 0.
#ifndef NOTE_MUTABLE_POLICY
#define NOTE_MUTABLE_POLICY 1
#endif

// NOTE_STATIC_HAL — when 1, transport types are templated on concrete HAL types
// instead of using virtual base references. Eliminates SerialHal and TransportHal
// vtables (~600 bytes on AVR). The DX is unchanged — SerialTransportStack
// handles the type plumbing internally.
#ifndef NOTE_STATIC_HAL
#define NOTE_STATIC_HAL 0
#endif

// NOTE_SINGLETON — when 1, Api uses a single static notecard pointer instead
// of per-group storage. Eliminates ~28 pointer inits in the constructor.
// Safe for the 99% of projects that use one Notecard. Define NOTE_SINGLETON=0
// to support multiple Notecard instances.
#ifndef NOTE_SINGLETON
#define NOTE_SINGLETON 0
#endif

// NOTE_INT32_MATH — when 1, uses int32_t instead of int64_t for number
// formatting (dtoa). Saves ~286 bytes on platforms with software 64-bit math
// (AVR, Cortex-M0). Values above INT32_MAX will be truncated.
// Default: 0 (full int64_t precision).
#ifndef NOTE_INT32_MATH
#define NOTE_INT32_MATH 0
#endif

// Default: runtime debug available (can be activated with set_debug()).
#ifndef NOTE_DEBUG_ENABLED
#define NOTE_DEBUG_ENABLED 1
#endif
