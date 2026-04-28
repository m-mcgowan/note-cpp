#pragma once

/// @file note_config.hpp
/// Feature flags with defaults. Include this before any other note-cpp header.
///
/// All flags use 1/0 values, not bare #define. Test with #if, not #ifdef.
///
/// NOTE_MINIMAL — when 1, sets conservative defaults for all flags to
/// minimize binary size on constrained platforms (AVR, etc.).
/// Individual flags can still be overridden after NOTE_MINIMAL.
///
/// Usage in platformio.ini:
///   build_flags = -DNOTE_MINIMAL=1    ; sets all size-saving defaults
///
/// Instead of:
///   build_flags = -DNOTE_NO_BUFFERED=1 -DNOTE_NO_STD_STRING=1 -DNOTE_NO_MD5=1
///                 -DNOTE_NO_CRC=1 -DNOTE_PRINTABLE=0 -DNOTE_EXTRAS=0
///                 -DNOTE_SHORT_ERRORS=1

// ── NOTE_MINIMAL defaults ──────────────────────────────────────────────

#ifndef NOTE_MINIMAL
#define NOTE_MINIMAL 0
#endif

#if NOTE_MINIMAL

#  ifndef NOTE_NO_BUFFERED
#    define NOTE_NO_BUFFERED 1
#  endif

#  ifndef NOTE_NO_STD_STRING
#    define NOTE_NO_STD_STRING 1
#  endif

#  ifndef NOTE_NO_MD5
#    define NOTE_NO_MD5 1
#  endif

#  ifndef NOTE_NO_CRC
#    define NOTE_NO_CRC 0
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
#    define NOTE_NO_RETRY 1
#  endif

#  ifndef NOTE_NO_REQUEST_IDS
#    define NOTE_NO_REQUEST_IDS 1
#  endif

#  ifndef NOTE_JSONB
#    define NOTE_JSONB 1
#  endif

#  ifndef NOTE_UNICODE_ESCAPES
#    define NOTE_UNICODE_ESCAPES 0
#  endif

#  ifndef NOTE_INT32_MATH
#    define NOTE_INT32_MATH 1
#  endif

#  ifndef NOTE_NO_POLYMORPHIC
#    define NOTE_NO_POLYMORPHIC 1
#  endif

#  ifndef NOTE_RESPONSE_BODY
#    define NOTE_RESPONSE_BODY 0
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

#  ifndef NOTE_TXN_HANDSHAKE
#    define NOTE_TXN_HANDSHAKE 0
#  endif

#endif // NOTE_MINIMAL

// ── Individual flag defaults ───────────────────────────────────────────
// Each flag defaults to its non-MINIMAL value if not already set.

#ifndef NOTE_NO_BUFFERED
#define NOTE_NO_BUFFERED 0
#endif

#ifndef NOTE_NO_STD_STRING
#define NOTE_NO_STD_STRING 0
#endif

#ifndef NOTE_NO_MD5
#define NOTE_NO_MD5 0
#endif

#ifndef NOTE_NO_CRC
#define NOTE_NO_CRC 0
#endif

#ifndef NOTE_NO_RETRY
#define NOTE_NO_RETRY 0
#endif

#ifndef NOTE_NO_REQUEST_IDS
#define NOTE_NO_REQUEST_IDS 0
#endif

#ifndef NOTE_NO_API_GROUPS
#define NOTE_NO_API_GROUPS 0
#endif

#ifndef NOTE_EXTRAS
#define NOTE_EXTRAS 1
#endif

// NOTE_TXN_HANDSHAKE — when 1, enable the optional transaction-handshake
// (CTX/RTX) hook on Protocol. Each transaction is bracketed by
// TxnHandshake::start()/stop() when a TxnHandshake is registered via
// set_handshake(). When 0, the hook code is fully compiled out.
//
// Only meaningful for Notecard SKUs that expose CTX/RTX pins (ESP32-based,
// STM32WL LoRa, STM32U5 cellular where AUX2/AUX3 are configured as
// CTX/RTX). See include/note/sku_info.hpp for per-SKU capability.
#ifndef NOTE_TXN_HANDSHAKE
#define NOTE_TXN_HANDSHAKE 1
#endif

#ifndef NOTE_PRINTABLE
#ifdef ARDUINO
#define NOTE_PRINTABLE 1
#else
#define NOTE_PRINTABLE 0
#endif
#endif

#ifndef NOTE_SHORT_ERRORS
#define NOTE_SHORT_ERRORS 0
#endif

#ifndef NOTE_JSONB
#define NOTE_JSONB 0
#endif

#ifndef NOTE_PROGMEM
#ifdef __AVR__
#define NOTE_PROGMEM 1
#else
#define NOTE_PROGMEM 0
#endif
#endif

#ifndef NOTE_UNICODE_ESCAPES
#define NOTE_UNICODE_ESCAPES 1
#endif

// NOTE_RESPONSE_BODY — when 1, response types include body parsing support
// (on_object_begin/end tracking, BodyHandler dispatch). When 0, body fields
// in responses are ignored and all endpoints use GenericResponseSink.
// Default: 1. NOTE_MINIMAL sets 0.
#ifndef NOTE_RESPONSE_BODY
#define NOTE_RESPONSE_BODY 1
#endif

// NOTE_MUTABLE_POLICY — when 1, protocol policy fields are mutable instance
// members (uint32_t, 28 bytes). When 0, fields are static constexpr (zero bytes).
// Default: 1 (mutable). NOTE_MINIMAL sets 0.
#ifndef NOTE_MUTABLE_POLICY
#define NOTE_MUTABLE_POLICY 1
#endif

// NOTE_NO_POLYMORPHIC — when 1, the polymorphic Notecard class (with runtime
// transport pointers and virtual IStreamingTransport base) is stripped.
// Use StaticNotecard<StackT> instead — fully compile-time resolved, no vtables.
//
// What you lose:
//   - Polymorphic Notecard class (notecard.hpp)
//   - NotecardApi / BareNotecard / DirectChannel convenience wrappers
//   - request() lambda builder and transact() raw JSON (lower API layers)
//   - Runtime transport switching and mock injection
//
// What you keep:
//   - Typed API (guided and unguided requests via StaticNotecard + Api<>)
//   - Streaming body parsing (.into())
//   - All codegen'd request/response types
//
// The lambda builder and JsonReader are templates, not inherently polymorphic.
// They are currently unavailable under NOTE_NO_POLYMORPHIC because of their
// std::string/std::function code size overhead, not because they require
// virtual dispatch.
#ifndef NOTE_NO_POLYMORPHIC
#define NOTE_NO_POLYMORPHIC 0
#endif

// NOTE_STATIC_HAL — when 1, transport types are templated on concrete HAL types
// instead of using virtual base references. Eliminates SerialHal and Hal
// vtables (~600 bytes on AVR). The DX is unchanged — SerialTransportStack
// handles the type plumbing internally.
// Independent of NOTE_NO_POLYMORPHIC — you can use static HAL with the
// polymorphic Notecard, or virtual HAL with StaticNotecard.
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

// NOTE_INT32_MATH — when 1, uses int32_t instead of int64_t for general
// integer fields. Saves ~286 bytes on platforms with software 64-bit math
// (AVR, Cortex-M0). Does NOT affect timestamps — those stay int64_t
// unless NOTE_SHORT_TIMESTAMPS is also set.
// Default: 0 (full int64_t precision).
#ifndef NOTE_INT32_MATH
#define NOTE_INT32_MATH 0
#endif

// NOTE_SHORT_TIMESTAMPS — when 1 (and NOTE_INT32_MATH=1), narrows
// timestamp fields from int64_t to int32_t. int32_t UNIX epoch
// overflows on 2038-01-19. Only set this if you need to eliminate
// ALL 64-bit math on severely constrained platforms.
// Default: 0 (timestamps always int64_t, even under NOTE_INT32_MATH).
#ifndef NOTE_SHORT_TIMESTAMPS
#define NOTE_SHORT_TIMESTAMPS 0
#endif

// NOTE_ARDUINO_STUBS — when 1, test environments provide their own
// Print/Printable stubs instead of including Arduino headers.
#ifndef NOTE_ARDUINO_STUBS
#define NOTE_ARDUINO_STUBS 0
#endif

// Default: runtime debug available (can be activated with set_debug()).
// Requires std::string support (NOTE_NO_STD_STRING=0).
#ifndef NOTE_DEBUG_ENABLED
#if NOTE_NO_STD_STRING
#define NOTE_DEBUG_ENABLED 0
#else
#define NOTE_DEBUG_ENABLED 1
#endif
#endif

// ── Flag compatibility checks ──────────────────────────────────────────
// Catch invalid combinations early with clear messages instead of
// cryptic template errors deep in the library.

#if NOTE_DEBUG_ENABLED && NOTE_NO_STD_STRING
#error "NOTE_DEBUG_ENABLED=1 requires std::string support (NOTE_NO_STD_STRING=0). \
Disable debug or enable std::string."
#endif

#if !NOTE_NO_BUFFERED && NOTE_NO_STD_STRING
#error "The buffered transport path requires std::string support (NOTE_NO_STD_STRING=0). \
Set NOTE_NO_BUFFERED=1 or enable std::string."
#endif

