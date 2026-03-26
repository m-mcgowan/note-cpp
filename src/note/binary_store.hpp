#pragma once
/// @file binary_store.hpp
/// High-level binary store convenience functions.
///
/// Equivalent to note-c's NoteBinaryStoreTransmit / NoteBinaryStoreReceive /
/// NoteBinaryStoreReset — handles the full binary transfer lifecycle in one call.
///
///   note::binary_store_reset(nc);
///   note::binary_store_transmit(nc, data, len);
///   note::binary_store_receive(nc, buf, buf_len, &decoded_len);

#include <note/notecard.hpp>
#include <note/api/card_binary.hpp>
#include <note/api/card_binary_put.hpp>
#include <note/api/card_binary_get.hpp>

namespace note {

/// Clear the Notecard's binary store.
/// Equivalent to note-c's NoteBinaryStoreReset().
inline Result<void> binary_store_reset(Notecard& nc) {
    api::CardBinary::Clear req;
    req.nc_ = &nc;
    auto r = req.execute();
    if (!r) return Unexpected(r.error());
    return {};
}

/// Store binary data on the Notecard.
///
/// Handles: status check → space validation → card.binary.put with COBS
/// and MD5 → stream raw bytes. Source buffer stays const.
///
/// @param nc       Notecard instance
/// @param data     Source data (unencoded)
/// @param len      Length of source data
/// @param offset   Byte offset into the Notecard's binary store (0 for first/only segment)
/// @returns        Error string on failure, empty on success
///
/// Equivalent to note-c's NoteBinaryStoreTransmit().
inline Result<void> binary_store_transmit(Notecard& nc,
                                           const uint8_t* data, size_t len,
                                           uint32_t offset = 0) {
    // Check status and available space
    api::CardBinary::Status status_req;
    status_req.nc_ = &nc;
    if (offset == 0) {
        // Reset on first segment (like note-c does with reset:true)
        auto reset_r = binary_store_reset(nc);
        if (!reset_r) return reset_r;
    }
    auto status = status_req.execute();
    if (!status) return Unexpected(status.error());

    int32_t max_bytes = status.max;
    if (max_bytes <= 0) {
        return Unexpected(ErrorInfo{Error::Notecard, "binary store not available (max is 0)"});
    }
    if (static_cast<int32_t>(len) > max_bytes - static_cast<int32_t>(offset)) {
        return Unexpected(ErrorInfo{Error::Overflow, "data exceeds available binary store space"});
    }

    // Put with data attached — execute handles COBS + MD5
    api::CardBinaryPut put;
    put.nc_ = &nc;
    if (offset > 0) {
        put.offset = static_cast<int32_t>(offset);
    }
    put.data(data, len);
    auto r = nc.execute(put);  // mutable ref → triggers binary pipeline
    if (!r) return Unexpected(r.error());
    return {};
}

/// Receive binary data from the Notecard.
///
/// Handles: status check → card.binary.get with COBS decode into buffer.
///
/// @param nc           Notecard instance
/// @param buf          Destination buffer (must be large enough for decoded data)
/// @param buf_len      Size of destination buffer
/// @param decoded_len  [out] Number of decoded bytes written to buf
/// @param offset       Byte offset to read from (0 for start)
/// @returns            Error string on failure, empty on success
///
/// Equivalent to note-c's NoteBinaryStoreReceive().
inline Result<void> binary_store_receive(Notecard& nc,
                                          uint8_t* buf, size_t buf_len,
                                          size_t* decoded_len = nullptr,
                                          uint32_t offset = 0) {
    // Query status to get cobs length
    api::CardBinary::Status status_req;
    status_req.nc_ = &nc;
    auto status = status_req.execute();
    if (!status) return Unexpected(status.error());

    int32_t cobs_len = status.cobs;
    int32_t data_len = status.length;
    if (data_len <= 0) {
        return Unexpected(ErrorInfo{Error::Notecard, "no binary data available"});
    }
    if (static_cast<size_t>(data_len) > buf_len) {
        return Unexpected(ErrorInfo{Error::Overflow, "buffer too small for binary data"});
    }

    // Get with destination buffer attached
    api::CardBinaryGet get;
    get.nc_ = &nc;
    get.cobs = cobs_len;
    get.length = data_len;
    if (offset > 0) {
        get.offset = static_cast<int32_t>(offset);
    }
    get.into(buf, buf_len);
    auto r = nc.execute(get);  // mutable ref → triggers binary pipeline
    if (!r) return Unexpected(r.error());

    if (decoded_len) {
        *decoded_len = static_cast<size_t>(data_len);
    }
    return {};
}

} // namespace note
