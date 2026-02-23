#pragma once

#include "types.hpp"

namespace note {

class NotecardIO {
public:
    virtual ~NotecardIO() = default;

    // Send a request and wait for the response.
    // Takes ownership of req. Returns an owned response handle that the
    // caller must wrap via JsonBackend::wrap_response() or free via
    // JsonBackend::free_response().
    virtual Result<json_handle> request_response(json_handle req,
                                                  uint32_t timeout_ms) = 0;

    // Send a command (fire-and-forget, no response expected).
    // Takes ownership of req.
    virtual Result<void> send(json_handle req) = 0;

    // Binary transfer: host -> Notecard (COBS-encoded on the wire).
    virtual Result<void> binary_transmit(const uint8_t* data, uint32_t len,
                                          uint32_t offset) = 0;

    // Binary transfer: Notecard -> host. Returns bytes received.
    virtual Result<uint32_t> binary_receive(uint8_t* buf,
                                             uint32_t buf_len) = 0;

    // Reset the Notecard's binary storage area.
    virtual Result<void> binary_reset() = 0;
};

} // namespace note
