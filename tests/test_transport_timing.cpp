// Tests for transport protocol timing constraints.
// Verifies that segment pacing, intra-transaction timeouts, and other
// timing parameters match note-c defaults and are actually applied.

#include <doctest.h>
#include <note/transport/protocol_policy.hpp>
#include <note/transport/serial.hpp>
#include <note/transport/i2c.hpp>
#include <vector>
#include <cstring>

using namespace note;
using namespace note::transport;

// ---------------------------------------------------------------------------
// Default policy values match note-c
// ---------------------------------------------------------------------------

TEST_CASE("Serial default policy matches note-c") {
    SerialPolicy p;
    CHECK(p.segment_max_len  == 250);   // CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN
    CHECK(p.segment_delay_ms == 250);   // CARD_REQUEST_SERIAL_SEGMENT_DELAY_MS
    CHECK(p.intra_timeout_ms == 1000);  // CARD_INTRA_TRANSACTION_TIMEOUT_SEC * 1000
    CHECK(p.max_retries      == 5);
}

TEST_CASE("I2C default policy matches note-c") {
    I2cPolicy p;
    CHECK(p.segment_max_len  == 250);   // CARD_REQUEST_I2C_SEGMENT_MAX_LEN
    CHECK(p.segment_delay_ms == 250);   // CARD_REQUEST_I2C_SEGMENT_DELAY_MS
    CHECK(p.intra_timeout_ms == 1000);  // CARD_INTRA_TRANSACTION_TIMEOUT_SEC * 1000
    CHECK(p.io_delay_ms      == 6);
    CHECK(p.chunk_delay_ms   == 20);
    CHECK(p.nack_wait_ms     == 1000);
    CHECK(p.response_poll_ms == 50);
}

// ---------------------------------------------------------------------------
// Mock HALs that record delay calls
// ---------------------------------------------------------------------------

namespace {

struct DelayRecord {
    uint32_t ms;
};

struct MockSerialHal : SerialHal {
    std::vector<DelayRecord> delays;
    std::vector<size_t> transmit_sizes;
    uint32_t time_ms = 0;
    std::string rx_data;
    size_t rx_pos = 0;

    bool transmit(const uint8_t* data, size_t len) override {
        transmit_sizes.push_back(len);
        (void)data;
        return true;
    }
    size_t receive(uint8_t* buf, size_t max) override {
        if (rx_pos >= rx_data.size()) return 0;
        size_t n = std::min(max, rx_data.size() - rx_pos);
        std::memcpy(buf, rx_data.data() + rx_pos, n);
        rx_pos += n;
        return n;
    }
    uint32_t millis() override { return time_ms; }
    void delay(uint32_t ms) override {
        delays.push_back({ms});
        time_ms += ms;
    }
};

struct MockI2cHal : I2CHal {
    std::vector<DelayRecord> delays;
    std::vector<size_t> transmit_sizes;
    uint32_t time_ms = 0;
    size_t mtu = 32;
    uint32_t available_bytes = 0;
    std::string rx_data;
    size_t rx_pos = 0;

    bool transmit(const uint8_t* data, size_t len) override {
        transmit_sizes.push_back(len);
        (void)data;
        return true;
    }
    bool receive(uint8_t* buf, size_t len, uint32_t& available) override {
        available = available_bytes;
        if (len > 0 && rx_pos < rx_data.size()) {
            size_t n = std::min(len, rx_data.size() - rx_pos);
            std::memcpy(buf, rx_data.data() + rx_pos, n);
            rx_pos += n;
        }
        return true;
    }
    uint32_t millis() override { return time_ms; }
    void delay(uint32_t ms) override {
        delays.push_back({ms});
        time_ms += ms;
    }
    size_t max_transfer() override { return mtu; }
    bool reset() override { return true; }
};

} // namespace

// ---------------------------------------------------------------------------
// I2C segment pacing
// ---------------------------------------------------------------------------

TEST_CASE("I2C transmit applies segment delay after segment_max_len bytes") {
    MockI2cHal hal;
    hal.mtu = 32;  // 32 bytes per I2C transfer

    // Use default policy (segment_max_len=250, segment_delay_ms=250)
    NotecardI2c<I2cPolicy> transport(hal);

    // Send 300 bytes — should trigger segment delay after 250 bytes
    std::vector<uint8_t> data(300, 0x42);
    REQUIRE(transport.transmit(data.data(), data.size()));

    // Count segment delays (250ms delays, not io_delay or chunk_delay)
    int segment_delays = 0;
    for (auto& d : hal.delays) {
        if (d.ms == 250) segment_delays++;
    }
    REQUIRE(segment_delays >= 1);
}

TEST_CASE("I2C transmit applies io_delay before each chunk") {
    MockI2cHal hal;
    hal.mtu = 32;

    NotecardI2c<I2cPolicy> transport(hal);

    // Send 64 bytes = 2 chunks of 32
    std::vector<uint8_t> data(64, 0x42);
    REQUIRE(transport.transmit(data.data(), data.size()));

    // Should have io_delay (6ms) before each chunk
    int io_delays = 0;
    for (auto& d : hal.delays) {
        if (d.ms == 6) io_delays++;
    }
    REQUIRE(io_delays == 2);  // one per chunk
}

TEST_CASE("I2C transmit applies chunk_delay between chunks") {
    MockI2cHal hal;
    hal.mtu = 32;

    NotecardI2c<I2cPolicy> transport(hal);

    // Send 64 bytes = 2 chunks
    std::vector<uint8_t> data(64, 0x42);
    REQUIRE(transport.transmit(data.data(), data.size()));

    // Should have chunk_delay (20ms) after each chunk
    int chunk_delays = 0;
    for (auto& d : hal.delays) {
        if (d.ms == 20) chunk_delays++;
    }
    REQUIRE(chunk_delays == 2);
}

TEST_CASE("I2C fast policy eliminates io_delay") {
    MockI2cHal hal;
    hal.mtu = 32;

    NotecardI2c<I2cPolicy> transport(hal, I2cPolicy::fast());

    std::vector<uint8_t> data(64, 0x42);
    REQUIRE(transport.transmit(data.data(), data.size()));

    // Fast policy: io_delay=0, chunk_delay=5
    for (auto& d : hal.delays) {
        CHECK(d.ms != 6);   // no default io_delay
    }
}

// ---------------------------------------------------------------------------
// Serial segment pacing
// ---------------------------------------------------------------------------

TEST_CASE("Serial transmit chunks at segment_max_len with pacing delay") {
    // note-c chunks serial transmits at CARD_REQUEST_SERIAL_SEGMENT_MAX_LEN (250)
    // with CARD_REQUEST_SERIAL_SEGMENT_DELAY_MS (250) between segments.
    // note-cpp must match this to avoid overflowing the Notecard's UART buffer.
    MockSerialHal hal;

    NotecardSerial<SerialPolicy> transport(hal);

    // Send 600 bytes — should be 3 segments (250+250+100) with delays between
    std::vector<uint8_t> data(600, 0x42);
    REQUIRE(transport.transmit(data.data(), data.size()));

    // Should chunk into segments <= 250 bytes
    REQUIRE(hal.transmit_sizes.size() >= 3);
    for (auto sz : hal.transmit_sizes) {
        CHECK(sz <= 250);
    }

    // Should have segment delays (250ms) between segments
    int segment_delays = 0;
    for (auto& d : hal.delays) {
        if (d.ms == 250) segment_delays++;
    }
    REQUIRE(segment_delays >= 2);  // at least 2 delays for 3 segments
}

// ---------------------------------------------------------------------------
// Reset applies segment_delay
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Intra-transaction timeout (response receive)
// ---------------------------------------------------------------------------

TEST_CASE("Serial read times out after intra_timeout_ms with no data") {
    MockSerialHal hal;
    // No rx_data — simulates Notecard not responding

    NotecardSerial<SerialPolicy> transport(hal);

    uint8_t buf[64];
    auto result = transport.read(buf, sizeof(buf), 1000);
    REQUIRE(!result);
    CHECK(hal.time_ms >= 1000);  // waited at least the timeout
}

TEST_CASE("I2C read times out with response_poll_ms polling") {
    MockI2cHal hal;
    hal.available_bytes = 0;  // no data available

    NotecardI2c<I2cPolicy> transport(hal);

    uint8_t buf[64];
    auto result = transport.read(buf, sizeof(buf), 1000);
    REQUIRE(!result);

    // Should have polled at response_poll_ms (50ms) intervals
    int polls = 0;
    for (auto& d : hal.delays) {
        if (d.ms == 50) polls++;
    }
    CHECK(polls >= 15);  // ~1000/50 = 20 polls, allow margin
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST_CASE("Serial reset applies initial segment_delay") {
    MockSerialHal hal;
    hal.rx_data = "\r\n";  // clean drain

    NotecardSerial<SerialPolicy> transport(hal);
    REQUIRE(transport.reset());

    // First delay should be segment_delay_ms (250)
    REQUIRE(!hal.delays.empty());
    CHECK(hal.delays[0].ms == 250);
}
