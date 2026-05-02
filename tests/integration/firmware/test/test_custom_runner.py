"""PlatformIO custom test runner shim.

Auto-installs embedded-bridge and embedded-test-runner from GitHub on
first use, and reinstalls when the pinned SHA drifts. Set
PIO_TEST_RUNNER_NO_AUTO_INSTALL=1 to disable installation entirely.

Bumping the runner: edit `_RUNNER_PINNED_SHA` to the new commit on
https://github.com/m-mcgowan/embedded-test-runner. The next test run
detects the drift and reinstalls. Editable installs (e.g.
`pip install -e ~/e/pio-test-runner`) are honoured — the SHA check
is skipped so local development isn't disturbed.
"""

import os
import subprocess
import sys
from importlib import metadata

# Pinned commit on https://github.com/m-mcgowan/embedded-test-runner.
# Bump intentionally; CI and fresh checkouts auto-reinstall on mismatch.
_RUNNER_PINNED_SHA = "0a40178"

_RUNNER_DIST = "embedded-test-runner"
_RUNNER_REPO = "https://github.com/m-mcgowan/embedded-test-runner.git"

_PACKAGES = [
    (
        "embedded_bridge",
        "embedded-bridge",
        "embedded-bridge @ git+https://github.com/m-mcgowan/embedded-bridge.git#subdirectory=python",
        None,
    ),
    (
        "etst",
        _RUNNER_DIST,
        f"{_RUNNER_DIST} @ git+{_RUNNER_REPO}@{_RUNNER_PINNED_SHA}",
        _RUNNER_PINNED_SHA,
    ),
]


def _is_editable(dist_name: str) -> bool:
    """Detect whether a distribution was installed in editable mode.

    Editable installs land a `.pth` shim and (modern pip) a
    `direct_url.json` whose `dir_info.editable` is true. We treat any
    direct_url.json as "user-managed" and skip SHA enforcement so local
    development checkouts aren't disturbed by reinstalls.
    """
    try:
        dist = metadata.distribution(dist_name)
    except metadata.PackageNotFoundError:
        return False
    direct_url = dist.read_text("direct_url.json")
    return direct_url is not None and "dir_info" in direct_url


def _installed_sha(dist_name: str) -> str | None:
    """Return the git commit SHA recorded for a non-editable git install.

    Pip writes `direct_url.json` with `vcs_info.commit_id` for any
    `pip install git+...` install. Returns None if the package was
    installed from a different source (PyPI wheel, sdist, etc.) or not
    installed at all.
    """
    try:
        dist = metadata.distribution(dist_name)
    except metadata.PackageNotFoundError:
        return None
    raw = dist.read_text("direct_url.json")
    if raw is None:
        return None
    try:
        import json
        data = json.loads(raw)
    except ValueError:
        return None
    return (data.get("vcs_info") or {}).get("commit_id")


def _pip_install(spec: str, *, force: bool = False) -> None:
    cmd = [sys.executable, "-m", "pip", "install"]
    if force:
        cmd += ["--force-reinstall", "--no-deps"]
    cmd.append(spec)
    subprocess.check_call(cmd)


def _ensure_installed():
    for module, dist_name, spec, pinned_sha in _PACKAGES:
        try:
            __import__(module)
            installed = True
        except ImportError:
            installed = False

        if not installed:
            _pip_install(spec)
            continue

        if pinned_sha is None:
            continue
        if _is_editable(dist_name):
            continue

        current = _installed_sha(dist_name)
        # current may legitimately differ in length from the pin (full
        # 40-char SHA vs short SHA we pin). Prefix-match in either
        # direction is enough for drift detection.
        if current is None or not (
            current.startswith(pinned_sha) or pinned_sha.startswith(current)
        ):
            _pip_install(spec, force=True)


if not os.environ.get("PIO_TEST_RUNNER_NO_AUTO_INSTALL"):
    _ensure_installed()

from etst.runner import EmbeddedTestRunner  # noqa: E402


class CustomTestRunner(EmbeddedTestRunner):
    """note-cpp test runner.

    The class exists primarily so PIO's ``custom_test_runner`` setting
    has a target whose import triggers the auto-install bootstrap above
    (etst + embedded-bridge at the pinned SHA). It carries no
    behavioural overrides today.

    COV: line capture used to live here as a hand-rolled router
    receiver; pio-gcov v0.1+ now ships ``CovReceiver`` as an
    ``embedded_test_runner.receivers`` entry-point plugin, so the
    capture auto-attaches whenever pio-gcov is pip-installed in PIO's
    penv. ``.cov`` files land at ``.pio/build/<env>/<partition>.cov``;
    decode with ``python3 -m pio_gcov lcov --log <cov> --output <info>``.
    """

    pass
