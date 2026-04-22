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
//
// All three ways to open are shown below; pick the one you prefer.

#include <note/posix.hpp>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s [--i2c] <device-path>\n"
            "  %s /dev/ttyUSB0\n"
            "  %s --i2c /dev/i2c-1\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const bool  use_i2c = (std::strcmp(argv[1], "--i2c") == 0);
    const char* path    = use_i2c ? argv[2] : argv[1];

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
    return 0;
}
