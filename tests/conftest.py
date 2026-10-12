"""Integration test fixtures.

Integration tests are skipped unless a daemon binary is supplied explicitly.
When CI=true the suite fails instead of skipping so a missing binary cannot
silently pass as a skipped run.
"""

from __future__ import annotations

import contextlib
import ctypes
import dataclasses
import os
import re
import shlex
import signal
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Generator
from pathlib import Path

import psutil
import pytest
from taz.c3 import TazClient
from taz.c3.settings import Backlog, Keepalive

# Sanitizer/leak reports that fail a test even though the daemon may keep
# running: UBSan recovers from "runtime error" by default, and ASan/TSan/LSan
# reports would otherwise only surface as a lost connection. Graceful shutdown
# closes every handle, so a stopped daemon must never print any of these.
_SANITIZER_ERRORS = (
    "ERROR: AddressSanitizer",
    "WARNING: ThreadSanitizer",
    "runtime error:",
    "LeakSanitizer",
)

_PR_SET_PDEATHSIG = 1

# Resolved here, at import, never in the forked child: a dlopen/dlsym there
# could deadlock on a loader lock another thread held at fork time.
_prctl = ctypes.CDLL(None, use_errno=True).prctl if sys.platform == "linux" else None


def _pdeathsig_preexec(parent_pid: int) -> Callable[[], None]:
    """A ``preexec_fn`` that makes the daemon child get SIGTERM if the
    thread that forked it dies first, even if an outer pytest timeout kills
    the test before fixture teardown runs (two tazd from a prior run were
    once found still running 11 hours later). ``_launch`` runs on pytest's
    main thread, so that thread's death is pytest's own."""

    def _apply() -> None:
        assert _prctl is not None
        _prctl(_PR_SET_PDEATHSIG, signal.SIGTERM)
        # The parent died between fork and prctl: no signal will ever come.
        if os.getppid() != parent_pid:
            os._exit(1)

    return _apply


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
    """A running ``tazd`` subprocess listening on ``port``.

    A background thread drains stderr for the process's whole lifetime: a
    log-heavy test can write far more than the 64 KiB pipe buffer, and
    nothing else reads stderr until ``stop()``, so an undrained pipe would
    deadlock the daemon mid-test.
    """

    proc: subprocess.Popen[bytes]
    port: int
    # The pid ``stop()`` signals. Equal to ``proc.pid`` at launch; a restart
    # test repoints this to the successor's pid on Windows, where the
    # restarted daemon is a new process that ``proc`` no longer refers to.
    pid: int = 0
    stderr: str = ""
    # > 1 under a slow wrapper (e.g. Valgrind): timing assertions multiply
    # their deadline by this.
    timing_scale: float = 1.0
    # Set when stop() had to fall back to a forced kill: a graceful stop
    # must never need it, so it fails the strict-exit check on its own.
    _killed: bool = dataclasses.field(default=False, repr=False, compare=False)
    _stderr_chunks: list[bytes] = dataclasses.field(
        default_factory=list, repr=False, compare=False
    )
    _stderr_thread: threading.Thread | None = dataclasses.field(
        default=None, repr=False, compare=False
    )
    # Set at launch; used by await_restart() to find the Windows successor
    # (same binary, bound to the same port, created no earlier than this).
    _binary: Path | None = dataclasses.field(default=None, repr=False, compare=False)
    _create_time: float = dataclasses.field(default=0.0, repr=False, compare=False)
    # Set by await_restart() on Windows once the successor is found: a new
    # process ``proc`` no longer refers to, so stop() must signal and wait
    # on it (via psutil) instead.
    _successor: psutil.Process | None = dataclasses.field(
        default=None, repr=False, compare=False
    )
    _successor_returncode: int | None = dataclasses.field(
        default=None, repr=False, compare=False
    )
    # await_restart()'s stdout reader; still blocked in readline() after a
    # timeout, in which case stop() must not close stdout under it.
    _stdout_thread: threading.Thread | None = dataclasses.field(
        default=None, repr=False, compare=False
    )

    def await_restart(self, timeout: float = 10.0) -> None:
        """Wait for a RESTART's re-exec to come back up.

        Reads the next ``LISTENING port=`` line from the inherited stdout
        pipe (must name the same port) with a bounded wait, since nothing
        else is draining stdout between restarts. On POSIX the pid is
        unchanged (``execv`` keeps it); on Windows the successor is a new
        process, found via psutil (executable path, ``--port <port>`` in
        its command line, and a creation time no earlier than the
        currently-tracked process's), and ``pid``/``stop()`` are repointed
        at it.
        """
        assert self.proc.stdout is not None
        stdout = self.proc.stdout
        line_box: list[bytes] = []

        def _read() -> None:
            line_box.append(stdout.readline())

        reader = threading.Thread(target=_read, daemon=True)
        self._stdout_thread = reader
        reader.start()
        reader.join(timeout)
        assert not reader.is_alive() and line_box, (
            f"daemon did not print a second LISTENING line within {timeout}s"
        )
        line = line_box[0]
        m = re.search(rb"LISTENING port=(\d+)", line)
        assert m is not None, f"expected a LISTENING line after restart, got {line!r}"
        new_port = int(m.group(1))
        assert new_port == self.port, (
            f"restarted on port {new_port}, expected the original port {self.port}"
        )
        if sys.platform == "win32" and not self._adopt_windows_successor(timeout):
            pytest.fail(
                f"no restart successor (exe={self._binary}, port={self.port})"
                f" found within {timeout}s"
            )

    def _adopt_windows_successor(self, timeout: float) -> bool:
        """Find the process a Windows restart spawned and repoint ``pid`` and
        ``stop()`` at it; False if none turns up within ``timeout``."""
        assert self._binary is not None
        want_exe = str(self._binary.resolve())
        port_token = str(self.port)
        previous_pid = self.pid
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for proc in psutil.process_iter(["pid", "exe", "cmdline", "create_time"]):
                if proc.pid == previous_pid:
                    continue
                try:
                    info = proc.info
                    exe = info["exe"]
                    if not exe or str(Path(exe).resolve()) != want_exe:
                        continue
                    if info["create_time"] < self._create_time:
                        continue
                    cmdline = info["cmdline"] or []
                    if "--port" not in cmdline or port_token not in cmdline:
                        continue
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    continue
                self.pid = proc.pid
                self._successor = proc
                self._successor_returncode = None
                return True
            time.sleep(0.05)
        return False

    def _start_stderr_reader(self) -> None:
        assert self.proc.stderr is not None
        stderr = self.proc.stderr

        def _drain() -> None:
            for chunk in stderr:
                self._stderr_chunks.append(chunk)

        self._stderr_thread = threading.Thread(target=_drain, daemon=True)
        self._stderr_thread.start()

    def stop(self) -> None:
        """Stop the daemon gracefully and collect its stderr; idempotent.

        A new process group ignores CTRL_C, so Windows asks for a graceful
        stop with CTRL_BREAK_EVENT instead of ``Popen.terminate()`` (which
        is ``TerminateProcess`` on Windows and would never exercise the
        daemon's console-control handler at all).

        After a Windows restart ``proc`` refers to the original process,
        which has already exited on its own once it spawned the successor;
        the successor (found by ``await_restart()``) is what actually needs
        signalling and waiting on here.
        """
        if (
            sys.platform == "win32"
            and self._successor is None
            and self.proc.returncode is None
            and self.proc.poll() == 0
        ):
            # The launcher exited 0 before anything stopped it, so a test
            # that failed between a restart and await_restart() left a
            # successor running: find it, so it is still stopped and its
            # exit still checked.
            self._adopt_windows_successor(timeout=2.0)
        if self._successor is not None:
            if self._successor_returncode is None:
                stop_signal: int
                if sys.platform == "win32":
                    stop_signal = signal.CTRL_BREAK_EVENT
                else:
                    stop_signal = signal.SIGTERM
                # psutil re-checks the process's identity before signalling,
                # so a reused pid is never hit.
                with contextlib.suppress(psutil.NoSuchProcess):
                    self._successor.send_signal(stop_signal)
                try:
                    self._successor_returncode = self._successor.wait(timeout=10)
                except psutil.TimeoutExpired:
                    self._killed = True
                    self._successor.kill()
                    self._successor_returncode = self._successor.wait(timeout=5)
        elif self.proc.returncode is None:
            # send_signal is a no-op once the process has already exited.
            if sys.platform == "win32":
                self.proc.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._killed = True
                self.proc.kill()
                self.proc.wait(timeout=5)
        if self._stderr_thread is not None:
            self._stderr_thread.join(timeout=5)
        self.stderr = b"".join(self._stderr_chunks).decode(errors="replace")
        stdout_reader_alive = (
            self._stdout_thread is not None and self._stdout_thread.is_alive()
        )
        if self.proc.stdout is not None and not stdout_reader_alive:
            self.proc.stdout.close()
        # A child that outlived tazd can still hold the pipe open; closing it
        # under the blocked reader would then wait for that child, so leave
        # it to the (daemon) thread.
        reader_alive = (
            self._stderr_thread is not None and self._stderr_thread.is_alive()
        )
        if self.proc.stderr is not None and not reader_alive:
            self.proc.stderr.close()


def _launch(binary: Path, env: dict[str, str] | None = None) -> Daemon:
    # TAZ_DAEMON_WRAPPER (e.g. a Valgrind invocation) is prepended to argv;
    # under a wrapper the daemon is much slower to start and to stop, so
    # timing assertions must scale their deadlines too.
    wrapper_env = os.environ.get("TAZ_DAEMON_WRAPPER")
    wrapper = shlex.split(wrapper_env) if wrapper_env else []
    argv = [*wrapper, str(binary), "--port", "0"]
    merged_env = {**os.environ, **env} if env else None
    if sys.platform == "win32":
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=merged_env,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
        )
    else:
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=merged_env,
            preexec_fn=(
                _pdeathsig_preexec(os.getpid()) if sys.platform == "linux" else None
            ),
        )
    assert proc.stdout is not None
    daemon = Daemon(proc, 0, pid=proc.pid, timing_scale=5.0 if wrapper else 1.0)
    daemon._start_stderr_reader()
    # The daemon flushes this line once it is bound and its signal handlers
    # are installed, so readline() returns promptly and stop() may follow at
    # once; EOF means it died before listening.
    line = proc.stdout.readline()
    m = re.search(rb"LISTENING port=(\d+)", line)
    if m is None:
        daemon.stop()
        pytest.fail(
            f"daemon never printed LISTENING port=<N> (got {line!r});"
            f" stderr:\n{daemon.stderr}"
        )
    daemon.port = int(m.group(1))
    daemon._binary = binary
    daemon._create_time = psutil.Process(proc.pid).create_time()
    return daemon


def _check_exit(daemon: Daemon) -> None:
    """Every stopped daemon must exit 0 with no sanitizer/leak output, on
    every platform; a forced-kill fallback is itself a failure. After a
    Windows restart, the successor's own exit code is what matters:
    ``proc`` only ever reports the original process's (already-0) exit."""
    returncode = (
        daemon._successor_returncode
        if daemon._successor is not None
        else daemon.proc.returncode
    )
    err = daemon.stderr
    if any(marker in err for marker in _SANITIZER_ERRORS):
        pytest.fail(f"daemon reported a sanitizer error:\n{err}")
    if daemon._killed:
        pytest.fail(
            f"daemon did not stop within the timeout and had to be"
            f" force-killed; stderr:\n{err}"
        )
    if returncode != 0:
        pytest.fail(f"daemon exited with {returncode} after stop(); stderr:\n{err}")


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
