/// I2C bus lock HAL for multi-device and multi-threaded Notecard use.
///
/// On a shared I2C bus, or when the Notecard is driven from multiple threads,
/// concurrent transactions corrupt both sides of the exchange. This mirrors
/// note-c's `NoteSetFnI2CMutex` / `NoteSetFnNoteMutex` hooks but as an
/// idiomatic C++ interface: callers implement `IBusLock` (or compose it via
/// the adapters below) and attach it to the transport; the transport acquires
/// the lock for one complete request/response exchange and releases between
/// exchanges.
///
/// Optional: if no IBusLock is attached, every transaction proceeds without
/// any locking. Single-device / single-threaded users pay nothing.
#pragma once

#include <note/note_config.hpp>

namespace note {

/// Optional bus lock. When registered, bracket every Notecard request/response
/// exchange with lock() / unlock(). See note/bus_lock.hpp for details.
struct IBusLock {
    /// Acquire the lock. Blocks until the lock is available.
    virtual void lock() = 0;
    /// Release the lock.
    virtual void unlock() = 0;
    virtual ~IBusLock() = default;
};

/// RAII guard that acquires an IBusLock on construction and releases it on
/// destruction. Safe with a null pointer (no-op in both directions).
class BusLockGuard {
public:
    explicit BusLockGuard(IBusLock* lock) : lock_(lock) {
        if (lock_) lock_->lock();
    }
    ~BusLockGuard() {
        if (lock_) lock_->unlock();
    }

    BusLockGuard(const BusLockGuard&) = delete;
    BusLockGuard& operator=(const BusLockGuard&) = delete;
    BusLockGuard(BusLockGuard&&) = delete;
    BusLockGuard& operator=(BusLockGuard&&) = delete;

private:
    IBusLock* lock_;
};

/// No-op lock for the static / AVR template path where a real mutex would
/// never be needed. Has no vtable; usable as a template parameter.
struct NullLock {
    void lock() noexcept {}
    void unlock() noexcept {}
};

/// Adapts any C++ Lockable (e.g. std::mutex, std::recursive_mutex) to the
/// IBusLock interface without requiring the lockable to inherit from it.
template<class Lockable>
struct LockAdapter : IBusLock {
    explicit LockAdapter(Lockable& m) : m_(m) {}
    void lock() override   { m_.lock(); }
    void unlock() override { m_.unlock(); }
private:
    Lockable& m_;
};

/// Adapts a pair of C-style function pointers to the IBusLock interface.
/// Useful for integrating with RTOS or bare-metal mutex APIs that expose a
/// C callback surface.
struct CallbackBusLock : IBusLock {
    using Fn = void(*)(void*);

    CallbackBusLock(Fn lock_fn, Fn unlock_fn, void* ctx)
        : lock_(lock_fn), unlock_(unlock_fn), ctx_(ctx) {}

    void lock() override   { if (lock_)   lock_(ctx_); }
    void unlock() override { if (unlock_) unlock_(ctx_); }

private:
    Fn    lock_;
    Fn    unlock_;
    void* ctx_;
};

} // namespace note
