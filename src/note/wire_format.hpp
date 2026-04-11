#pragma once
/// @file wire_format.hpp
/// Wire format policy tags for StreamingTransport.
///
/// The wire format determines how requests are encoded and responses decoded:
/// - JsonWireFormat:  JSON text + optional CRC (default)
/// - JsonbWireFormat: JSONB binary + COBS framing (no CRC)
///
/// Selection is compile-time via NOTE_JSONB.
/// NOTE_MINIMAL implies NOTE_JSONB=1 unless explicitly set to 0.

#ifndef NOTE_JSONB
#   ifdef NOTE_MINIMAL
#       define NOTE_JSONB 1
#   else
#       define NOTE_JSONB 0
#   endif
#endif

namespace note {

struct JsonWireFormat {};
struct JsonbWireFormat {};

#if NOTE_JSONB
using ActiveWireFormat = JsonbWireFormat;
#else
using ActiveWireFormat = JsonWireFormat;
#endif

}  // namespace note
