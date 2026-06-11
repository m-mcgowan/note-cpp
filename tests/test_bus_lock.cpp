#include "doctest.h"
#include <note/bus_lock.hpp>

#include <mutex>

namespace {

struct RecordingLock : note::IBusLock {
    int locks = 0;
    int unlocks = 0;
    int held = 0;
    int max_held = 0;
    void lock() override   { ++locks; ++held; if (held > max_held) max_held = held; }
    void unlock() override { --held; ++unlocks; }
};

} // namespace

TEST_CASE("BusLockGuard brackets lock()/unlock() around its scope") {
    RecordingLock lk;
    {
        note::BusLockGuard guard{&lk};
        CHECK(lk.locks == 1);
        CHECK(lk.unlocks == 0);
        CHECK(lk.held == 1);
    }
    CHECK(lk.unlocks == 1);
    CHECK(lk.held == 0);
}

TEST_CASE("BusLockGuard with null lock is a no-op") {
    note::BusLockGuard guard{nullptr};
    CHECK(true);
}

TEST_CASE("LockAdapter wraps any C++ lockable") {
    std::mutex m;
    note::LockAdapter<std::mutex> adapter{m};
    note::IBusLock& as_lock = adapter;
    as_lock.lock();
    CHECK(m.try_lock() == false);
    as_lock.unlock();
    CHECK(m.try_lock() == true);
    m.unlock();
}

TEST_CASE("CallbackBusLock forwards to C function pointers") {
    int calls = 0;
    auto lk = [](void* ctx) { (*static_cast<int*>(ctx)) += 1; };
    auto ul = [](void* ctx) { (*static_cast<int*>(ctx)) += 10; };
    note::CallbackBusLock cb{lk, ul, &calls};
    cb.lock();
    cb.unlock();
    CHECK(calls == 11);
}
