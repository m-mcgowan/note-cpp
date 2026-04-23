// Talk to a real Notecard from a POSIX host (Linux, macOS, BSD).
//
// This example requires an actual Notecard connected over USB-serial or,
// on Linux, I2C. Unlike the other stdcpp examples (which mock the transport
// for host-only demonstrations), this one opens a real device.
//
// Build:
//   c++ -std=c++20 -I include examples/stdcpp/posix-hardware.cpp -o notecard
//
// Run — pass the device path as an argument:
//   ./notecard /dev/ttyUSB0                 # serial
//   ./notecard /dev/cu.usbmodem1101         # macOS serial
//   ./notecard --i2c /dev/i2c-1             # Linux I2C
//   ./notecard --binary 4096 /dev/...       # plus a 4KB binary round-trip
//
// All three ways to open are shown below; pick the one you prefer.

#include <note/posix.hpp>
#include <note/api/card_binary.hpp>
#include <note/api/card_binary_put.hpp>
#include <note/api/card_binary_get.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Deterministic fill so the put/get verification is reproducible. Not a
// CSPRNG — just a cheap byte-permuter (31 is coprime with 256, so it walks
// the whole byte range across each ~256-byte window).
void fill_pattern(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        buf[i] = static_cast<uint8_t>(i * 31u + 7u);
    }
}

// Put-then-get-then-compare. Exercises multi-segment transmit, COBS
// encode/decode, MD5 compute/verify — the real work in the HAL's long
// transmit() and receive() paths.
int binary_round_trip(note::posix::Notecard<>& nc, size_t size) {
    std::vector<uint8_t> src(size);
    std::vector<uint8_t> dst(size, 0);
    fill_pattern(src.data(), size);

    std::printf("binary: clearing store...\n");
    if (auto r = nc.card.binary.clear().execute(); !r) {
        std::fprintf(stderr, "  card.binary.clear failed: %s\n",
                     note::to_string(r.error()).c_str());
        return 1;
    }

    std::printf("binary: putting %zu bytes...\n", size);
    if (auto r = nc.card.binary.put().data(src.data(), size).execute(); !r) {
        std::fprintf(stderr, "  card.binary.put failed: %s\n",
                     note::to_string(r.error()).c_str());
        return 1;
    }

    std::printf("binary: getting %zu bytes...\n", size);
    auto r = nc.card.binary.get().into(dst.data(), dst.size()).length(static_cast<int32_t>(size)).execute();
    if (!r) {
        std::fprintf(stderr, "  card.binary.get failed: %s\n",
                     note::to_string(r.error()).c_str());
        return 1;
    }

    if (std::memcmp(src.data(), dst.data(), size) != 0) {
        std::fprintf(stderr, "  mismatch: put/get bytes differ\n");
        return 1;
    }

    std::printf("binary: round-trip %zu bytes OK\n", size);
    (void)nc.card.binary.clear().execute();  // leave the store clean
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool        use_i2c     = false;
    size_t      binary_size = 0;
    const char* path        = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--i2c") == 0) {
            use_i2c = true;
        } else if (std::strcmp(argv[i], "--binary") == 0 && i + 1 < argc) {
            binary_size = static_cast<size_t>(std::atoi(argv[++i]));
        } else {
            path = argv[i];
        }
    }

    if (!path) {
        std::fprintf(stderr,
            "usage: %s [--i2c] [--binary N] <device-path>\n"
            "  %s /dev/ttyUSB0\n"
            "  %s --i2c /dev/i2c-1\n"
            "  %s --binary 4096 /dev/cu.usbmodemNOTE1\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    note::posix::Notecard nc;

    if (use_i2c) {
#ifdef __linux__
        // Three equivalent ways to open an I2C link (all with default addr 0x17):
        //   nc.begin_i2c(path);
        //   nc.begin(note::posix::i2c{path});
        //   nc.begin(note::posix::i2c{path, 0x17});
        nc.begin_i2c(path);
#else
        std::fprintf(stderr, "I2C is Linux-only; use serial on this platform.\n");
        return 1;
#endif
    } else {
        // Three equivalent ways to open a serial link at 9600 baud:
        //   nc.begin(path);                                  // default baud
        //   nc.begin_serial(path, 9600);                      // explicit method
        //   nc.begin(note::posix::serial{path, 9600});        // tag type
        nc.begin(path);
    }

    // Fetch version info. The typed API returns a result; check it before use.
    auto result = nc.card.version().execute();
    if (!result) {
        std::fprintf(stderr, "card.version failed: %s\n",
                     note::to_string(result.error()).c_str());
        return 1;
    }

    std::printf("version: %.*s\n",
                static_cast<int>(result.version.size()), result.version.data());
    std::printf("device:  %.*s\n",
                static_cast<int>(result.device.size()),  result.device.data());

    if (binary_size > 0) {
        return binary_round_trip(nc, binary_size);
    }
    return 0;
}
