/// FreeRTOS bus-lock adapter. Wraps a SemaphoreHandle_t (typically a mutex
/// semaphore created with xSemaphoreCreateMutex()) as an IBusLock, so a
/// Notecard sharing an I2C bus under FreeRTOS can serialize against other
/// tasks. Include only on FreeRTOS targets; never pulled in by host builds.
#pragma once

#include <note/bus_lock.hpp>

#include <FreeRTOS.h>
#include <semphr.h>

namespace note {

struct FreeRtosBusLock : IBusLock {
    explicit FreeRtosBusLock(SemaphoreHandle_t h) : h_(h) {}
    void lock()   override { xSemaphoreTake(h_, portMAX_DELAY); }
    void unlock() override { xSemaphoreGive(h_); }
private:
    SemaphoreHandle_t h_;
};

} // namespace note
