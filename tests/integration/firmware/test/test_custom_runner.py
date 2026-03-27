"""PlatformIO custom test runner shim.

Auto-installs pio-test-runner and embedded-bridge from GitHub on first use.
Set PIO_TEST_RUNNER_NO_AUTO_INSTALL=1 to disable auto-installation.
"""

import os
import subprocess
import sys

_PACKAGES = [
    (
        "embedded_bridge",
        "embedded-bridge @ git+https://github.com/m-mcgowan/embedded-bridge.git#subdirectory=python",
    ),
    (
        "pio_test_runner",
        "pio-test-runner @ git+https://github.com/m-mcgowan/pio-test-runner.git",
    ),
]


def _auto_install():
    for module, pip_spec in _PACKAGES:
        try:
            __import__(module)
        except ImportError:
            subprocess.check_call(
                [sys.executable, "-m", "pip", "install", pip_spec],
            )


if not os.environ.get("PIO_TEST_RUNNER_NO_AUTO_INSTALL"):
    _auto_install()

from pio_test_runner.runner import EmbeddedTestRunner  # noqa: E402


class CustomTestRunner(EmbeddedTestRunner):
    pass
