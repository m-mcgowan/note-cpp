# note-cpp source and header lists for CMake.
#
# Hand-curated lists of the public API surface and test sources.
# Generated file lists are in cmake/note-cpp-generated.cmake (emitted by codegen).
#
# Usage:
#   include(cmake/note-cpp-sources.cmake)
#   include(cmake/note-cpp-generated.cmake)  # after running codegen

# ── Public headers (hand-written) ─────────────────────────────────────────

set(NOTE_CPP_PUBLIC_HEADERS
    include/note/allocator.hpp
    include/note/alloc.hpp
    include/note/arena.hpp
    include/note/array_field.hpp
    include/note/binary_request.hpp
    include/note/body.hpp
    include/note/compiler.hpp
    include/note/dyn_field.hpp
    include/note/error.hpp
    include/note/field.hpp
    include/note/field_desc.hpp
    include/note/flag_set.hpp
    include/note/generic_sink.hpp
    include/note/detail/number_format.hpp
    include/note/json.hpp
    include/note/json_buf.hpp
    include/note/jsonb.hpp
    include/note/wire_format.hpp
    include/note/json_sax.hpp
    include/note/md5.hpp
    include/note/notecard.hpp
    include/note/notecard_api.hpp
    include/note/print.hpp
    include/note/request_set.hpp
    include/note/safety.hpp
    include/note/span.hpp
    include/note/string_pool.hpp
    include/note/struct_sink.hpp
    include/note/target.hpp
    include/note/transport.hpp
    include/note/types.hpp
    include/note/units.hpp
    include/note/voltage_variable.hpp
)

# ── Transport headers ─────────────────────────────────────────────────────

set(NOTE_CPP_TRANSPORT_HEADERS
    include/note/transport/cobs.hpp
    include/note/transport/detail/crc32.hpp
    include/note/transport/i2c.hpp
    include/note/transport/protocol_policy.hpp
    include/note/transport/serial.hpp
)

# ── Arduino headers (require Arduino SDK) ─────────────────────────────────

set(NOTE_CPP_ARDUINO_HEADERS
    include/note/arduino.hpp
    include/note/arduino/compat.hpp
    include/note/arduino/i2c.hpp
    include/note/arduino/serial.hpp
    include/note/arduino/txn.hpp
)

# ── POSIX headers (Linux / macOS / BSD; I2C is Linux-only) ────────────────

set(NOTE_CPP_POSIX_HEADERS
    include/note/posix.hpp
    include/note/posix/clock.hpp
    include/note/posix/serial.hpp
    include/note/posix/i2c.hpp
)

# ── Backend headers ───────────────────────────────────────────────────────

set(NOTE_CPP_BACKEND_HEADERS
    include/note/backends/buffer.hpp
    include/note/backends/cjson.hpp
    include/note/backends/nlohmann.hpp
)

# ── Third-party (vendored) ────────────────────────────────────────────────

set(NOTE_CPP_THIRD_PARTY_HEADERS
    include/note/third_party/expected.hpp
    include/note/third_party/reflect.hpp
)

# ── Test sources (hand-written) ───────────────────────────────────────────
# Paths relative to project root.

# Tests that compile under both regular and NOTE_MINIMAL builds.
set(NOTE_CPP_TEST_SOURCES_COMMON
    test_buffer_backend.cpp
    test_cobs.cpp
    test_jsonb.cpp
    test_flag_set.cpp
    test_json_buf.cpp
    test_json_fmt.cpp
    test_json_sax.cpp
    test_json_sax_streaming.cpp
    test_json_scan.cpp
    test_error_message.cpp
    test_json_lexer.cpp
    test_retry.cpp
    test_state_store.cpp
    test_target.cpp
    test_transport_crc32.cpp
    test_body_capture_arena.cpp
    test_static_sizing.cpp
    test_static_notecard.cpp
    test_sax_dispatch.cpp
    test_struct_sink.cpp
    test_generic_sink.cpp
    test_units.cpp
    test_voltage_variable.cpp
)

# Tests that require the polymorphic Notecard class (not available under NOTE_MINIMAL).
set(NOTE_CPP_TEST_SOURCES_FULL_ONLY
    test_arduino_printable.cpp
    test_allocator_growth.cpp
    test_attention.cpp
    test_bare_notecard.cpp
    test_binary_execute.cpp
    test_body.cpp
    test_channel.cpp
    test_connection.cpp
    test_debug.cpp
    test_intent_flags.cpp
    test_make_api.cpp
    test_migration_support.cpp
    test_notecard.cpp
    test_notecard_streaming.cpp
    test_property_functor.cpp
    test_setup.cpp
    test_streaming_builder.cpp
    test_streaming_errors.cpp
    test_struct_field_symmetry.cpp
    test_sync.cpp
    test_templates.cpp
    test_transport_i2c.cpp
    test_transport_serial.cpp
    test_transport_streaming.cpp
    test_transport_timing.cpp
    test_txn_handshake.cpp
    test_wire_format.cpp
)

set(NOTE_CPP_TEST_SOURCES
    ${NOTE_CPP_TEST_SOURCES_COMMON}
    ${NOTE_CPP_TEST_SOURCES_FULL_ONLY}
)
