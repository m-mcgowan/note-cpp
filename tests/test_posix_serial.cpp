// Unit tests for note::posix::PosixSerialHal using a pty loopback.
//
// posix_openpt() returns a master/slave fd pair that behaves like a real
// serial link: bytes written on the master are readable on the slave and
// vice versa. We open the slave via PosixSerialHal (exercising termios
// setup, open, read, write, millis, delay) and drive the other end from
// the test to assert correct behaviour — no hardware required.

#include <doctest.h>

#include <note/posix/serial.hpp>

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

// RAII alarm — kills the process with SIGALRM if a test takes longer than
// its budget. Cheap safety net so a regression in a HAL call that blocks
// forever (e.g. tcdrain on a quiescent pty) can't wedge CI for minutes.
class TestTimeout {
public:
    explicit TestTimeout(unsigned seconds = 5) { ::alarm(seconds); }
    ~TestTimeout() { ::alarm(0); }
};

// RAII wrapper around a pty master/slave pair.
class PtyPair {
public:
    PtyPair() {
        master_fd_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_fd_ < 0) return;
        if (::grantpt(master_fd_) != 0)  { close(); return; }
        if (::unlockpt(master_fd_) != 0) { close(); return; }

        const char* name = ::ptsname(master_fd_);
        if (!name) { close(); return; }
        slave_path_ = name;

        // Put the master in non-blocking mode so read()s don't hang the
        // test suite if bytes are late or the HAL writes less than expected.
        const int flags = ::fcntl(master_fd_, F_GETFL, 0);
        ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);
    }

    ~PtyPair() { close(); }

    PtyPair(const PtyPair&)            = delete;
    PtyPair& operator=(const PtyPair&) = delete;

    bool               ok() const            { return master_fd_ >= 0; }
    int                master_fd() const     { return master_fd_; }
    const std::string& slave_path() const    { return slave_path_; }

    // Read up to `max` bytes with a short timeout. Returns how many bytes
    // were read; 0 on timeout.
    size_t read_with_timeout(uint8_t* buf, size_t max, int timeout_ms = 100) {
        struct pollfd pfd{master_fd_, POLLIN, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) return 0;
        const ssize_t n = ::read(master_fd_, buf, max);
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

    bool write_all(const void* data, size_t len) {
        return ::write(master_fd_, data, len) == static_cast<ssize_t>(len);
    }

private:
    void close() {
        if (master_fd_ >= 0) { ::close(master_fd_); master_fd_ = -1; }
    }

    int         master_fd_ = -1;
    std::string slave_path_;
};

}  // namespace

TEST_CASE("PosixSerialHal: open fails for non-existent device", "[posix][serial]") {
    TestTimeout t;
    note::posix::PosixSerialHal hal("/dev/note-cpp-nonexistent", 9600);
    REQUIRE_FALSE(hal.is_open());
    REQUIRE_FALSE(static_cast<bool>(hal));
}

TEST_CASE("PosixSerialHal: opens a valid pty slave", "[posix][serial]") {
    TestTimeout t;
    PtyPair pty;
    REQUIRE(pty.ok());

    note::posix::PosixSerialHal hal(pty.slave_path().c_str(), 9600);
    REQUIRE(hal.is_open());
    REQUIRE(static_cast<bool>(hal));
}

TEST_CASE("PosixSerialHal: transmit writes bytes visible on the peer", "[posix][serial]") {
    TestTimeout t;
    PtyPair pty;
    REQUIRE(pty.ok());
    note::posix::PosixSerialHal hal(pty.slave_path().c_str(), 9600);
    REQUIRE(hal.is_open());

    const uint8_t msg[] = {'h', 'e', 'l', 'l', 'o', '\r', '\n'};
    REQUIRE(hal.transmit(msg, sizeof(msg)));

    uint8_t got[16] = {};
    const size_t n = pty.read_with_timeout(got, sizeof(got));
    REQUIRE(n == sizeof(msg));
    REQUIRE(std::memcmp(got, msg, sizeof(msg)) == 0);
}

TEST_CASE("PosixSerialHal: receive reads bytes written by the peer", "[posix][serial]") {
    TestTimeout t;
    PtyPair pty;
    REQUIRE(pty.ok());
    note::posix::PosixSerialHal hal(pty.slave_path().c_str(), 9600);
    REQUIRE(hal.is_open());

    const uint8_t payload[] = "{\"req\":\"card.version\"}\r\n";
    REQUIRE(pty.write_all(payload, sizeof(payload) - 1));

    // Poll the HAL side until bytes arrive (pty write → kernel → pty read
    // is fast but not synchronous on all platforms).
    uint8_t buf[64] = {};
    size_t total = 0;
    for (int i = 0; i < 20 && total < sizeof(payload) - 1; ++i) {
        total += hal.receive(buf + total, sizeof(buf) - total);
        if (total < sizeof(payload) - 1) hal.delay(5);
    }
    REQUIRE(total == sizeof(payload) - 1);
    REQUIRE(std::memcmp(buf, payload, total) == 0);
}

TEST_CASE("PosixSerialHal: receive returns 0 when no data is pending", "[posix][serial]") {
    TestTimeout t;
    PtyPair pty;
    REQUIRE(pty.ok());
    note::posix::PosixSerialHal hal(pty.slave_path().c_str(), 9600);
    REQUIRE(hal.is_open());

    uint8_t buf[32] = {};
    REQUIRE(hal.receive(buf, sizeof(buf)) == 0);
}

TEST_CASE("PosixSerialHal: millis advances monotonically", "[posix][serial]") {
    TestTimeout t;
    PtyPair pty;
    REQUIRE(pty.ok());
    note::posix::PosixSerialHal hal(pty.slave_path().c_str(), 9600);

    const uint32_t t0 = hal.millis();
    hal.delay(15);
    const uint32_t t1 = hal.millis();

    // Delay is ≥ requested but can be coarser; give generous bounds.
    REQUIRE(t1 >= t0 + 10);
    REQUIRE(t1 <  t0 + 500);
}

#else

TEST_CASE("PosixSerialHal: skipped on non-POSIX host", "[posix][serial]") {
    SUCCEED("Not a POSIX host");
}

#endif  // __unix__ || __APPLE__
