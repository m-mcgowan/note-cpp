#pragma once

// Linux I2C HAL for the Notecard (via /dev/i2c-N and the i2c-dev interface).
//
// Linux-only: uses <linux/i2c-dev.h> and the I2C_SLAVE ioctl. On macOS,
// BSDs, or other POSIX systems, this header compiles to an empty stub —
// code referencing LinuxI2cHal will fail to link/compile on those platforms.
//
// Implements the Notecard SoI2C (Serial-over-I2C) framing:
//   transmit: [size_byte, data...]
//   receive:  write [0x00, N] then read [available, good_bytes, data...]
//             priming query (N==0): write [0x00, 0x00], read [available, 0x00]
//
// Typical usage on a Raspberry Pi:
//
//   note::posix::LinuxI2cHal hal("/dev/i2c-1");          // default addr 0x17
//   if (!hal) { /* open or ioctl failed */ }
//   note::transport::NotecardI2c transport(hal);
//
// Prefer note::posix::Notecard from <note/posix.hpp> for the convenience API.

#ifdef __linux__

#include <note/transport/i2c.hpp>
#include <note/posix/clock.hpp>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/i2c-dev.h>

#include <cstdint>
#include <cstring>

namespace note::posix {

class LinuxI2cHal : public transport::I2CHal {
public:
    // SoI2C response header: [available, good_bytes]
    static constexpr uint8_t kResponseHeaderSize = 2;

    explicit LinuxI2cHal(const char* device,
                         uint8_t address   = transport::kI2cDefaultAddress,
                         size_t  max_xfer  = transport::kI2cDefaultMtu)
        : address_(address), max_xfer_(max_xfer) {
        fd_ = ::open(device, O_RDWR);
        if (fd_ < 0) return;
        if (::ioctl(fd_, I2C_SLAVE, static_cast<int>(address_)) < 0) close();
    }

    ~LinuxI2cHal() override { close(); }

    LinuxI2cHal(const LinuxI2cHal&) = delete;
    LinuxI2cHal& operator=(const LinuxI2cHal&) = delete;

    explicit operator bool() const { return fd_ >= 0; }
    bool is_open() const { return fd_ >= 0; }

    // Re-open the I2C handle. The kernel's i2c-dev has no "bus reset" —
    // closing and reopening clears any pending driver state, which is the
    // closest portable equivalent to the hardware reset this interface
    // models on microcontrollers.
    bool reset() override {
        if (fd_ < 0) return false;
        return true;  // no-op: kernel handles bus recovery
    }

    // Transmit len bytes: [size_byte, data...]
    bool transmit(const uint8_t* data, size_t len) override {
        if (fd_ < 0) return false;
        if (len > max_xfer_) return false;

        uint8_t buf[transport::kI2cMaxMtu + 1];
        buf[0] = static_cast<uint8_t>(len);
        if (len > 0) std::memcpy(buf + 1, data, len);

        const ssize_t want = static_cast<ssize_t>(len + 1);
        return ::write(fd_, buf, static_cast<size_t>(want)) == want;
    }

    // Receive from the Notecard.
    //   len == 0: priming query — write [0x00, 0x00], read 2-byte header.
    //   len  > 0: write [0x00, len], read header + payload.
    bool receive(uint8_t* buf, size_t len, uint32_t& available) override {
        if (fd_ < 0) return false;
        if (len > max_xfer_) return false;

        // Request header: [0x00, len]
        const uint8_t req[2] = {0x00, static_cast<uint8_t>(len)};
        if (::write(fd_, req, sizeof(req)) != static_cast<ssize_t>(sizeof(req))) {
            return false;
        }

        // Brief pause lets the Notecard's I2C ISR stage the response.
        struct timespec ts{0, 2 * 1000 * 1000L};  // 2 ms
        ::nanosleep(&ts, nullptr);

        // Read 2-byte response header + payload bytes.
        uint8_t rx[transport::kI2cMaxMtu + kResponseHeaderSize];
        const ssize_t want = static_cast<ssize_t>(len + kResponseHeaderSize);
        const ssize_t got = ::read(fd_, rx, static_cast<size_t>(want));
        if (got < static_cast<ssize_t>(kResponseHeaderSize)) return false;

        available = static_cast<uint32_t>(rx[0]);
        const uint8_t good = rx[1];
        if (good > len) return false;

        if (good > 0 && buf != nullptr) std::memcpy(buf, rx + kResponseHeaderSize, good);
        return true;
    }

    uint32_t millis() override          { return clock::millis(); }
    void     delay(uint32_t ms) override { clock::sleep_ms(ms); }
    size_t   max_transfer() override    { return max_xfer_; }

private:
    void close() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    int     fd_       = -1;
    uint8_t address_;
    size_t  max_xfer_;
};

}  // namespace note::posix

#endif  // __linux__
