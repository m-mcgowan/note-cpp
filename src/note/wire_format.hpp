#pragma once
/// @file wire_format.hpp
/// Wire format policy tags for Protocol.
///
/// The wire format determines how requests are encoded and responses decoded:
/// - JsonWireFormat:  JSON text + optional CRC (default)
/// - JsonbWireFormat: JSONB binary + COBS framing (no CRC)
///
/// Selection is compile-time via NOTE_JSONB (default set in note_config.hpp).

namespace note {

struct JsonWireFormat {};
struct JsonbWireFormat {};

#if NOTE_JSONB
using ActiveWireFormat = JsonbWireFormat;
#else
using ActiveWireFormat = JsonWireFormat;
#endif

}  // namespace note
