# POSIX HAL hardware smoke test

End-to-end smoke test for `note::posix::PosixSerialHal` against a real
Notecard connected over USB-serial. Drives the `posix-hardware` example
and asserts that `card.version` returned plausible output.

The host unit tests (`tests/test_posix_serial.cpp`) already cover the
HAL's termios / open / read / write / timing contract via a pty loopback;
this script adds confidence that the HAL also works end-to-end against
real Notecard firmware when you have the hardware at hand.

## Running

Default: skipped. Gate behind an env var so CI without hardware is
unaffected:

```sh
# Explicit device path
NOTE_CPP_POSIX_HW_TEST=1 ./run.sh /dev/cu.usbmodem14301

# Or a usb-device-registered name
NOTE_CPP_POSIX_HW_TEST=1 ./run.sh "Notecard Alpha"
```

Pass criteria: the `posix-hardware` example prints `version: <string>`
and `device: <string>` — i.e. the full chain (open → transmit →
protocol → receive → JSON parse → typed accessors) works against live
firmware.

## Prerequisites

- A Notecard exposed as a USB-serial device (typically `/dev/cu.usbmodem*`
  on macOS, `/dev/ttyUSB*` or `/dev/ttyACM*` on Linux).
- Current user has read/write access to the device node (on Linux you may
  need to be in the `dialout` group).
- Optional: [`usb-device`](https://github.com/m-mcgowan/usb-device) installed
  and the Notecard registered with a friendly name, so you can pass the
  name instead of the raw path.
