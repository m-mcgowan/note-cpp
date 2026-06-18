/// @file test_operation_lock_hw.cpp
/// On-silicon (FreeRTOS) validation of the recursive operation lock.
///
/// tests/test_operation_lock.cpp proves on the host (with std::thread +
/// std::recursive_mutex) that run_operation() serializes two threads sharing
/// one Notecard. This is the hardware analogue: two real FreeRTOS tasks share
/// one Notecard and a recursive operation lock backed by a FreeRTOS recursive
/// mutex (xSemaphoreCreateRecursiveMutex, wired through CallbackBusLock).
///
/// A mock ITransact with an in-flight interleave detector stands in for the
/// transport — this test validates the *lock*, not Notecard comms (those are
/// covered by the binary/transport HIL tests). Using a mock keeps the test
/// independent of the real Notecard and free of the pre-existing JSONB-parser
/// crash in test_fixtures.

#include <doctest.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include <note/notecard.hpp>
#include <note/bus_lock.hpp>
#include <note/transact.hpp>
#include <note/error.hpp>

namespace {

/// Mock transport with an in-flight interleave detector. If two tasks ever
/// execute an operation concurrently, in_flight exceeds 1 and a violation is
/// recorded. The vTaskDelay(1) widens the window so a missing lock reliably
/// interleaves under the FreeRTOS scheduler.
struct InterleaveTransport : note::ITransact {
    std::atomic<int> in_flight{0};
    std::atomic<int> violations{0};
    std::atomic<int> calls{0};

    struct NoopHal : note::Hal {
        bool transmit(const uint8_t*, size_t) override { return true; }
        note::Result<size_t> read(uint8_t*, size_t, uint32_t) override { return size_t{0}; }
        bool reset() override { return true; }
        bool write_line_terminator() override { return true; }
        uint32_t millis() override { return ::millis(); }
        void delay(uint32_t ms) override { ::delay(ms); }
    } hal_;
    note::Hal& hal() override { return hal_; }

    using note::ITransact::transact;
    using note::ITransact::send;

    note::Result<note::string_view> transact(note::string_view, note::span<char> buf,
                                             uint32_t) override {
        calls.fetch_add(1, std::memory_order_relaxed);
        if (in_flight.fetch_add(1, std::memory_order_acq_rel) != 0)
            violations.fetch_add(1, std::memory_order_relaxed);
        vTaskDelay(1);  // widen the concurrency window
        static constexpr note::string_view rsp{"{}"};
        note::Result<note::string_view> r =
            note::make_error(note::Error::Overflow, "buffer too small");
        if (rsp.size() < buf.size()) {
            for (size_t i = 0; i < rsp.size(); ++i) buf.data()[i] = rsp.data()[i];
            r = note::string_view(buf.data(), rsp.size());
        }
        in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return r;
    }
    note::Result<void> send(note::string_view) override { return {}; }
    void reset() override {}
    void abort() override {}
};

struct WorkerArgs {
    note::Notecard*   nc;
    int               iters;
    SemaphoreHandle_t done;
};

void op_worker(void* p) {
    auto* a = static_cast<WorkerArgs*>(p);
    char buf[32];
    for (int i = 0; i < a->iters; ++i) {
        a->nc->transact(R"({"req":"card.status"})", note::span<char>(buf, sizeof(buf)));
    }
    xSemaphoreGive(a->done);
    vTaskDelete(nullptr);
}

// Run two worker tasks against `nc` and return the violation count.
int run_two_tasks(note::Notecard& nc, InterleaveTransport& tx, int iters) {
    SemaphoreHandle_t done = xSemaphoreCreateCounting(2, 0);
    REQUIRE(done != nullptr);
    WorkerArgs a{&nc, iters, done};

    xTaskCreate(op_worker, "op1", 4096, &a, 5, nullptr);
    xTaskCreate(op_worker, "op2", 4096, &a, 5, nullptr);

    xSemaphoreTake(done, portMAX_DELAY);
    xSemaphoreTake(done, portMAX_DELAY);

    CHECK(tx.calls.load() == 2 * iters);
    vSemaphoreDelete(done);
    return tx.violations.load();
}

constexpr int kIters = 200;

} // namespace

// Control: with NO operation lock, two FreeRTOS tasks interleave on the shared
// Notecard — proving both the detector and the scheduler actually interleave.
TEST_CASE("FreeRTOS: two tasks without an operation lock DO interleave (control)") {
    InterleaveTransport tx;
    note::Notecard nc{tx, note::Allocator{}};
    // No set_request_lock() — operations are unserialized.

    int violations = run_two_tasks(nc, tx, kIters);
    CHECK(violations > 0);
}

// With a recursive operation lock (FreeRTOS recursive mutex), the two tasks
// serialize: no operation is ever interleaved by the other task.
TEST_CASE("FreeRTOS: two tasks sharing one Notecard serialize via recursive operation lock") {
    InterleaveTransport tx;
    note::Notecard nc{tx, note::Allocator{}};

    SemaphoreHandle_t rmtx = xSemaphoreCreateRecursiveMutex();
    REQUIRE(rmtx != nullptr);
    note::CallbackBusLock op_lock{
        [](void* c) { xSemaphoreTakeRecursive(static_cast<SemaphoreHandle_t>(c), portMAX_DELAY); },
        [](void* c) { xSemaphoreGiveRecursive(static_cast<SemaphoreHandle_t>(c)); },
        rmtx
    };
    nc.set_request_lock(op_lock);

    int violations = run_two_tasks(nc, tx, kIters);
    CHECK(violations == 0);

    vSemaphoreDelete(rmtx);
}
