#pragma once
/// @file wire_format.hpp
/// Wire format policy tags for StreamingTransport.
///
/// The wire format determines how requests are encoded and responses decoded:
/// - JsonWireFormat:  JSON text + optional CRC (default)
/// - JsonbWireFormat: JSONB binary + COBS framing (no CRC)
///
/// Selection is compile-time via NOTE_JSONB. When NOTE_JSONB is defined,
/// StreamingTransport uses JSONB for all request/response encoding.

namespace note {

struct JsonWireFormat {};
struct JsonbWireFormat {};

#ifdef NOTE_JSONB
using ActiveWireFormat = JsonbWireFormat;
#else
using ActiveWireFormat = JsonWireFormat;
#endif

}  // namespace note
