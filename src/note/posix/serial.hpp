#pragma once

// POSIX serial HAL for the Notecard.
//
// Portable across Linux, macOS, and BSDs. Opens a TTY via termios in raw
// 8N1 mode with non-blocking I/O, then exposes it as a note::transport::SerialHal
// for use with note::transport::NotecardSerial.
//
// Usage (low-level):
//
//   note::posix::PosixSerialHal hal("/dev/ttyUSB0", 9600);
//   if (!hal) { /* open failed */ }
//   note::transport::NotecardSerial transport(hal);
//
// Most users should prefer note::posix::Notecard from <note/posix.hpp>,
// which wraps this HAL behind a begin() convenience API.

#if defined(__unix__) || defined(__APPLE__)

#include <note/transport/serial.hpp>
#include <note/posix/clock.hpp>

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <unistd.h>

#include <cstdint>

namespace note::posix {

class PosixSerialHal : public transport::SerialHal {
public:
    PosixSerialHal(const char* port, int baud = 9600) {
        fd_ = ::open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) return;

        struct termios tty{};
        if (::tcgetattr(fd_, &tty) != 0) { close(); return; }

        const speed_t speed = baud_to_speed(baud);
        ::cfsetispeed(&tty, speed);
        ::cfsetospeed(&tty, speed);

        // Raw mode, 8N1, no flow control, no echo, no signals, no CR→NL.
        // Explicit tcflag_t casts avoid -Wsign-conversion on platforms where
        // the termios macros are unsigned but their bitwise-OR is int.
        tty.c_cflag &= static_cast<tcflag_t>(~(PARENB | CSTOPB | CSIZE | CRTSCTS));
        tty.c_cflag |= static_cast<tcflag_t>(CS8 | CREAD | CLOCAL);
        tty.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ECHOE | ISIG));
        tty.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | IXANY | ICRNL));
        tty.c_oflag &= static_cast<tcflag_t>(~OPOST);
        // VMIN=0/VTIME=0 means: non-blocking, return whatever is available.
        tty.c_cc[VMIN]  = 0;
        tty.c_cc[VTIME] = 0;

        if (::tcsetattr(fd_, TCSANOW, &tty) != 0) { close(); return; }

        // Drain any bytes the kernel buffered during USB enumeration. Without
        // this, the first reset handshake sometimes sees stale garbage on the
        // input stream and reports "not_ready" on the first request after
        // open, even though the Notecard itself is responding correctly.
        ::tcflush(fd_, TCIFLUSH);
    }

    ~PosixSerialHal() override { close(); }

    PosixSerialHal(const PosixSerialHal&) = delete;
    PosixSerialHal& operator=(const PosixSerialHal&) = delete;

    explicit operator bool() const { return fd_ >= 0; }
    bool is_open() const { return fd_ >= 0; }

    bool transmit(const uint8_t* data, size_t len) override {
        if (fd_ < 0) return false;
        size_t total = 0;
        while (total < len) {
            const ssize_t n = ::write(fd_, data + total, len - total);
            if (n > 0) {
                total += static_cast<size_t>(n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                // Kernel output buffer full (typical for bulk transfers at
                // 9600 baud) or interrupted — wait briefly, then retry.
                clock::sleep_ms(1);
            } else {
                return false;  // hard error
            }
        }
        return true;
    }

    size_t receive(uint8_t* buf, size_t max_len) override {
        if (fd_ < 0 || max_len == 0) return 0;
        const ssize_t n = ::read(fd_, buf, max_len);
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

    uint32_t millis() override         { return clock::millis(); }
    void     delay(uint32_t ms) override { clock::sleep_ms(ms); }

private:
    static speed_t baud_to_speed(int baud) {
        switch (baud) {
            case 1200:   return B1200;
            case 2400:   return B2400;
            case 4800:   return B4800;
            case 9600:   return B9600;
            case 19200:  return B19200;
            case 38400:  return B38400;
            case 57600:  return B57600;
            case 115200: return B115200;
            default:     return B9600;
        }
    }

    void close() {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    int fd_ = -1;
};

}  // namespace note::posix

#endif  // __unix__ || __APPLE__
