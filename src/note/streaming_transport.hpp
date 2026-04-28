#pragma once

// Deprecated header. Use <note/protocol.hpp> instead.
//
// `StreamingTransport` was renamed to `Protocol` in Phase 4 of the
// transport refactor. `protocol.hpp` carries the canonical definition
// and exposes `using StreamingTransport = Protocol` for one release of
// source-compat. This header is a thin redirect; remove your include
// of it once you've switched to `note/protocol.hpp`.

#include <note/protocol.hpp>
