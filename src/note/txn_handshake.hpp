/// Transaction handshake HAL for Notecard SKUs with RTX/CTX edge-connector pins.
///
/// On some Notecard SKUs, the host must signal intent to transact on an
/// RTX pin and wait for a CTX "ready" signal before sending a request.
/// Equally important: the host must release RTX after the transaction so
/// the Notecard is allowed to sleep. Leaving RTX asserted prevents sleep
/// even though requests still work.
///
/// This mirrors note-c's `NoteSetFnTransaction(txnStartFn, txnStopFn)`
/// (see n_hooks.c:313, called around every transaction in n_request.c).
/// A reference Arduino implementation lives in note-arduino's
/// `NoteTxn_Arduino.cpp`.
///
/// Optional: if no TxnHandshake is attached, every transaction proceeds
/// without the handshake. Users on SKUs without these pins pay nothing.
#pragma once

#include <note/note_config.hpp>

#include <stdint.h>

namespace note {

/// Optional transaction handshake. Bracket every Notecard transaction
/// with start() / stop() when registered. See note/txn_handshake.hpp for
/// details.
class TxnHandshake {
public:
    virtual ~TxnHandshake() = default;

    /// Blocking. Called before each transaction (request/command).
    /// Implementations should assert the transaction-request signal (e.g.
    /// drive RTX high) and wait up to `timeout_ms` for the ready signal
    /// (e.g. CTX high). Return true on ready; false on timeout.
    virtual bool start(uint32_t timeout_ms) = 0;

    /// Non-blocking. Called after each transaction (success or failure).
    /// Implementations should release the transaction-request signal (e.g.
    /// float RTX) so the Notecard can sleep.
    virtual void stop() = 0;
};

} // namespace note
