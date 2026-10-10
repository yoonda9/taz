"""Integration test fixtures.

Integration tests are skipped unless a daemon binary is supplied explicitly.
When CI=true the suite fails instead of skipping so a missing binary cannot
silently pass as a skipped run.
"""

from __future__ import annotations

import dataclasses
import os
import re
import subprocess
import sys
from collections.abc import Generator
from pathlib import Path

import pytest
from taz.c3 import TazClient
from taz.c3.settings import Backlog, Keepalive

# Sanitizer reports that fail a test even though the daemon may keep running:
# UBSan recovers from "runtime error" by default, and ASan/TSan reports would
# otherwise only surface as a lost connection.
_SANITIZER_ERRORS = (
    "ERROR: AddressSanitizer",
    "WARNING: ThreadSanitizer",
    "runtime error:",
)


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--daemon",
        action="store",
        default=os.environ.get("TAZ_DAEMON"),
        help="Path to a tazd binary (or set TAZ_DAEMON).",
    )


@pytest.fixture(scope="session")
def daemon_binary(request: pytest.FixtureRequest) -> Path:
    value = request.config.getoption("--daemon")
    if not value:
        if os.environ.get("CI"):
            pytest.fail(
                "CI is set but no daemon binary: pass --daemon or set TAZ_DAEMON"
            )
        pytest.skip("no daemon binary: pass --daemon or set TAZ_DAEMON")
    path = Path(value)
    if not path.exists():
        pytest.fail(f"daemon binary not found: {path}")
    return path


@dataclasses.dataclass
class Daemon:
    """A running ``tazd`` subprocess listening on ``port``."""

    proc: subprocess.Popen[bytes]
    port: int
    stderr: str = ""

    def stop(self) -> None:
        """Terminate the daemon and collect its stderr; idempotent."""
        if self.proc.returncode is not None:
            return
        self.proc.terminate()
        try:
            _, err = self.proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            _, err = self.proc.communicate(timeout=5)
        self.stderr = err.decode(errors="replace")


def _launch(binary: Path, env: dict[str, str] | None = None) -> Daemon:
    proc = subprocess.Popen(
        [str(binary), "--port", "0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, **env} if env else None,
    )
    assert proc.stdout is not None
    # The daemon flushes this line right after binding, so readline() returns
    # promptly; EOF means it died before listening.
    line = proc.stdout.readline()
    m = re.search(rb"LISTENING port=(\d+)", line)
    if m is None:
        daemon = Daemon(proc, 0)
        daemon.stop()
        pytest.fail(
            f"daemon never printed LISTENING port=<N> (got {line!r});"
            f" stderr:\n{daemon.stderr}"
        )
    return Daemon(proc, int(m.group(1)))


def _check_exit(daemon: Daemon) -> None:
    err = daemon.stderr
    if any(marker in err for marker in _SANITIZER_ERRORS):
        pytest.fail(f"daemon reported a sanitizer error:\n{err}")
    # SIGTERM stops the loop without closing its handles, so an ASan build
    # exits 1 with a LeakSanitizer report; that is the only non-zero exit
    # tolerated. TerminateProcess on Windows always yields 1.
    if (
        sys.platform != "win32"
        and daemon.proc.returncode != 0
        and "LeakSanitizer" not in err
    ):
        pytest.fail(
            f"daemon exited with {daemon.proc.returncode} after SIGTERM; stderr:\n{err}"
        )


def _daemon_for(
    daemon_binary: Path, env: dict[str, str] | None = None
) -> Generator[Daemon, None, None]:
    d = _launch(daemon_binary, env)
    try:
        yield d
    finally:
        d.stop()
    _check_exit(d)


@pytest.fixture
def daemon(daemon_binary: Path) -> Generator[Daemon, None, None]:
    """Launch a daemon on an OS-assigned port; stop it and audit its exit."""
    yield from _daemon_for(daemon_binary)


@pytest.fixture
def daemon_no_pidfd(daemon_binary: Path) -> Generator[Daemon, None, None]:
    """Launch a daemon with PROCESS_MONITOR's pidfd exit detection disabled,
    exercising the timer fallback that every dev/CI host's pidfd support
    would otherwise always skip."""
    yield from _daemon_for(daemon_binary, {"TAZ_MONITOR_NO_PIDFD": "1"})


def _client_for(daemon: Daemon) -> Generator[TazClient, None, None]:
    client = TazClient(
        "127.0.0.1",
        daemon.port,
        keepalive=Keepalive(idle=60.0, timeout=10.0),
        backlog=Backlog(),
    )
    client.connect()
    try:
        yield client
    finally:
        client.close()


@pytest.fixture
def taz_client(daemon: Daemon) -> Generator[TazClient, None, None]:
    """A TazClient connected to the ``daemon`` fixture."""
    yield from _client_for(daemon)


@pytest.fixture
def taz_client_no_pidfd(daemon_no_pidfd: Daemon) -> Generator[TazClient, None, None]:
    """A TazClient connected to the ``daemon_no_pidfd`` fixture."""
    yield from _client_for(daemon_no_pidfd)
