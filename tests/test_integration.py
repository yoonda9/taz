"""Integration tests: TazClient against a real daemon subprocess."""

from __future__ import annotations

import contextlib
import gc
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from collections.abc import Callable, Generator
from pathlib import Path
from typing import Any

import google_crc32c
import psutil
import pytest
from google.protobuf import empty_pb2
from taz.c3 import (
    CommandResult,
    FileTransfer,
    Keepalive,
    ProcessMonitorUpdate,
    StreamIterator,
    TaskState,
    TaskStatus,
    TazClient,
    TazConnectionLost,
    TazError,
    TazPriorityError,
)
from taz.c3.file import Kind
from taz.v1 import (
    advanced_pb2,
    command_pb2,
    common_pb2,
    daemon_control_pb2,
    file_pb2,
    process_pb2,
)

from tests.conftest import Daemon, _check_exit, _launch, process_is_system

_PROCESS_OPCODES = {
    common_pb2.OPCODE_PROCESS_LIST,
    common_pb2.OPCODE_PROCESS_KILL,
    common_pb2.OPCODE_PROCESS_INFO,
    common_pb2.OPCODE_PROCESS_MONITOR,
}

_FILE_OPCODES = {
    common_pb2.OPCODE_FILE_CREATE,
    common_pb2.OPCODE_FILE_DELETE,
    common_pb2.OPCODE_FILE_STAT,
    common_pb2.OPCODE_FILE_CHMOD,
    common_pb2.OPCODE_DIR_MAKE,
    common_pb2.OPCODE_DIR_LIST,
    common_pb2.OPCODE_DIR_REMOVE,
    common_pb2.OPCODE_FILE_PUT,
    common_pb2.OPCODE_FILE_GET,
}

_DAEMON_CMAKELISTS = Path(__file__).resolve().parents[1] / "daemon" / "CMakeLists.txt"


def _project_version() -> str:
    """The ``VERSION`` of the daemon's ``project()`` call."""
    m = re.search(
        r"^\s*VERSION\s+(\d+\.\d+\.\d+)", _DAEMON_CMAKELISTS.read_text(), re.MULTILINE
    )
    assert m is not None, f"no project VERSION in {_DAEMON_CMAKELISTS}"
    return m.group(1)


PY = sys.executable


def _py(snippet: str) -> list[str]:
    """``args`` for a no-shell ``python -c <snippet>`` child."""
    return ["-c", snippet]


def _normalize(data: bytes) -> bytes:
    """Collapse Windows text-mode ``\\r\\n`` so output assertions are portable."""
    return data.replace(b"\r\n", b"\n")


def _touch_files(directory: Path, count: int) -> set[str]:
    """Create ``count`` empty files directly under ``directory``; return names."""
    names = {f"f{i:04d}" for i in range(count)}
    for name in names:
        (directory / name).touch()
    return names


def _process_gone(pid: int) -> bool:
    """True once ``pid`` is unreachable or a not-yet-reaped zombie.

    A killed process tree's orphaned members get reparented and reaped by
    init asynchronously, so there's a brief window where the pid still
    resolves to a zombie; that still counts as "gone" for our purposes.
    """
    try:
        return psutil.Process(pid).status() == psutil.STATUS_ZOMBIE
    except psutil.NoSuchProcess:
        return True


def _wait_process_gone(pid: int, timeout: float = 5.0) -> bool:
    """Poll :func:`_process_gone` until it holds or ``timeout`` elapses: a
    SIGKILL (or a job-object kill) lands asynchronously, so a killed tree
    can outlive the call that killed it by a moment."""
    deadline = time.monotonic() + timeout
    while not _process_gone(pid):
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)
    return True


_UNREAD_GET_SIZE = 64 << 20
_PEEK_MAX = 64 << 20


def _flood_unread_file_gets(client: TazClient, tmp_path: Path, count: int = 4) -> None:
    """Queue ``count`` FILE_GETs of a 64 MiB file and never read a chunk,
    then wait until the bytes waiting in ``client``'s socket stop growing:
    the daemon's writes to it are then backed up, so a graceful
    ``uv_shutdown`` of that connection cannot complete."""
    src = tmp_path / "unread.bin"
    with src.open("wb") as f:
        f.truncate(_UNREAD_GET_SIZE)
    req = file_pb2.FileGetRequest(src=str(src))
    for _ in range(count):
        client._conn.send_request(common_pb2.OPCODE_FILE_GET, req.SerializeToString())
    sock = client._conn._sock
    assert sock is not None
    deadline = time.monotonic() + 10
    last = -1
    while True:
        waiting = len(sock.recv(_PEEK_MAX, socket.MSG_PEEK))
        if waiting >= 64 * 1024 and waiting == last:
            return
        assert time.monotonic() < deadline, "the daemon's writes never backed up"
        last = waiting
        time.sleep(0.2)


def _send_restart_and_read_ack(client: TazClient, delay_ms: int) -> int:
    """Send RESTART and read its response, leaving the connection to the
    caller (``client.restart()`` would instead wait for the daemon's EOF)."""
    req = daemon_control_pb2.RestartRequest(delay_ms=delay_ms)
    stream_id = client._conn.send_request(
        common_pb2.OPCODE_RESTART, req.SerializeToString()
    )
    frame = client._dispatcher.recv_response(
        stream_id, client._keepalive, expected_opcode=common_pb2.OPCODE_RESTART
    )
    assert frame.type == common_pb2.FRAME_TYPE_RESPONSE
    resp = daemon_control_pb2.RestartResponse()
    resp.ParseFromString(frame.payload)
    assert resp.acknowledged
    return resp.effective_delay_ms


_SLEEP_SNIPPET = "import os, time; print(os.getpid(), flush=True); time.sleep(60)"

_SIGUSR1_SNIPPET = (
    "import os, signal, time\n"
    "print(os.getpid(), flush=True)\n"
    "signal.signal(signal.SIGUSR1, lambda *_: print('usr1', flush=True))\n"
    "print('ready', flush=True)\n"
    "time.sleep(60)\n"
)

_INVALID_UTF8_COMM_SNIPPET = (
    "import os, time\n"
    "print(os.getpid(), flush=True)\n"
    "with open('/proc/self/comm', 'wb') as f:\n"
    "    f.write(b'ab\\xff\\xfecd')\n"
    "print('ready', flush=True)\n"
    "time.sleep(60)\n"
)


def _spawn_helper(snippet: str, **popen_kw: Any) -> tuple[subprocess.Popen[bytes], int]:
    """Spawn ``python -c <snippet>``; the snippet must start with
    ``print(os.getpid(), flush=True)``. Returns the ``Popen`` and that
    printed pid, which on Windows can differ from ``Popen.pid`` when the
    venv's ``python.exe`` is a launcher for the real interpreter.
    """
    proc = subprocess.Popen([PY, "-c", snippet], stdout=subprocess.PIPE, **popen_kw)
    assert proc.stdout is not None
    line = proc.stdout.readline()
    return proc, int(line)


def _unused_pid() -> int:
    """A pid guaranteed to name no process on this host."""
    if sys.platform == "win32":
        pids = set(psutil.pids())
        candidate = 0x7FFFFFFC
        while candidate in pids:
            candidate -= 4
        return candidate
    pid_max = int(Path("/proc/sys/kernel/pid_max").read_text())
    return pid_max + 1


_MONITOR_INTERVAL = 100


def _drain(
    m: StreamIterator[ProcessMonitorUpdate], n: int
) -> list[ProcessMonitorUpdate]:
    """Take ``n`` updates from a monitor stream with ``next``."""
    return [next(m) for _ in range(n)]


def _wait_final(
    m: StreamIterator[ProcessMonitorUpdate], timeout: float
) -> ProcessMonitorUpdate:
    """Iterate ``m`` until its final update, bounded by a wall-clock
    deadline: a loop with no daemon-independent bound is a livelock
    if the final frame never arrives."""
    deadline = time.monotonic() + timeout
    update: ProcessMonitorUpdate | None = None
    while not m.ended:
        assert time.monotonic() < deadline, "stream never reached its final frame"
        update = next(m)
    assert update is not None
    return update


def _python_name_filter() -> str:
    """The substring that matches the helper's reported process name: the
    interpreter's basename, cut to the kernel's 15-byte ``comm`` limit on
    Linux.
    """
    name = os.path.basename(sys.executable)
    return name[:15] if sys.platform != "win32" else name


@pytest.fixture
def spawn_helper() -> Generator[
    Callable[..., tuple[subprocess.Popen[bytes], int]], None, None
]:
    """Factory for ``_spawn_helper`` that kills, waits and confirms every
    helper spawned during the test is gone (by its printed pid) on
    teardown."""
    spawned: list[tuple[subprocess.Popen[bytes], int]] = []

    def _spawn(snippet: str, **popen_kw: Any) -> tuple[subprocess.Popen[bytes], int]:
        proc, pid = _spawn_helper(snippet, **popen_kw)
        spawned.append((proc, pid))
        return proc, pid

    yield _spawn

    for proc, pid in spawned:
        if proc.poll() is None:
            proc.kill()
        with contextlib.suppress(subprocess.TimeoutExpired):
            proc.wait(timeout=10)
        if proc.stdout is not None:
            proc.stdout.close()
        deadline = time.monotonic() + 5
        while not _process_gone(pid) and time.monotonic() < deadline:
            time.sleep(0.05)


class TestPing:
    @pytest.mark.valgrind
    def test_ping_succeeds(self, taz_client: TazClient) -> None:
        taz_client.ping()

    def test_ping_after_idle(self, taz_client: TazClient) -> None:
        # Guards against the daemon ever adding idle disconnects; the client's
        # own keepalive (idle=60) stays quiet for the whole sleep.
        time.sleep(5)
        taz_client.ping()


class TestVersion:
    @pytest.mark.valgrind
    def test_version_matches_project_version(self, taz_client: TazClient) -> None:
        # build_info.c is generated from the CMake project version; a fallback
        # or stale value would differ.
        info = taz_client.version()
        assert info.version == _project_version()
        assert info.build
        assert info.platform


class TestCapabilities:
    @pytest.mark.valgrind
    def test_capabilities_contains_expected_opcodes(
        self, taz_client: TazClient
    ) -> None:
        cap = taz_client.capabilities()
        ops = set(cap.operations)
        assert common_pb2.OPCODE_PING in ops
        assert common_pb2.OPCODE_VERSION in ops

    def test_capabilities_protocol_major(self, taz_client: TazClient) -> None:
        cap = taz_client.capabilities()
        assert cap.protocol_major == 1


class TestConnectionLost:
    def test_daemon_exit_raises_connection_lost(self, daemon: Daemon) -> None:
        with TazClient("127.0.0.1", daemon.port) as client:
            client.ping()
            daemon.stop()
            with pytest.raises(TazConnectionLost):
                client.ping()
            # The failure sticks: the next call fails fast, chained.
            with pytest.raises(TazConnectionLost) as exc_info:
                client.version()
            assert isinstance(exc_info.value.__cause__, TazConnectionLost)


class TestConnectionLifetime:
    """Closing a client mid-exec kills its process tree without blocking
    other connections or leaving the daemon's connection state corrupt."""

    def test_closing_socket_during_exec_kills_tree_and_connection_stays_usable(
        self, daemon: Daemon, tmp_path: Path
    ) -> None:
        pid_file = tmp_path / "pid.txt"
        # No shell, per convention: the child reports its own pid via a file
        # (the client never reads the COMMAND_EXEC response in this test).
        snippet = (
            "import os, pathlib, time; "
            f"pathlib.Path(r'{pid_file}').write_text(str(os.getpid())); "
            "time.sleep(60)"
        )
        req = command_pb2.CommandExecRequest(
            command=sys.executable, args=["-c", snippet]
        )

        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        # Bypass CommandNamespace: send the raw REQUEST and never read its
        # response, simulating an abrupt client disconnect while COMMAND_EXEC
        # is in flight.
        victim._conn.send_request(
            common_pb2.OPCODE_COMMAND_EXEC, req.SerializeToString()
        )

        deadline = time.monotonic() + 10
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert pid_file.exists(), "child never started"
        pid = int(pid_file.read_text())
        assert psutil.pid_exists(pid)

        # Abrupt teardown: close the socket without waiting for a RESPONSE.
        victim.close()

        # The dangling exec must not block a second, unrelated connection.
        with TazClient("127.0.0.1", daemon.port) as other:
            start = time.monotonic()
            other.version()
            assert time.monotonic() - start < 2.0

        # conn_close cancels every in-flight exec's whole process tree.
        deadline = time.monotonic() + 10
        while psutil.pid_exists(pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        assert not psutil.pid_exists(pid)


class TestCommandExec:
    """``COMMAND_EXEC`` round trips through the typed ``client.command`` API."""

    def test_print_captures_stdout_and_exit_code(self, taz_client: TazClient) -> None:
        result = taz_client.command.exec(PY, args=_py("print('hello')"))
        assert result.exit_code == 0
        assert _normalize(result.stdout) == b"hello\n"
        assert result.timed_out is False
        assert result.truncated is False

    def test_exit_code_is_propagated(self, taz_client: TazClient) -> None:
        result = taz_client.command.exec(PY, args=_py("import sys; sys.exit(3)"))
        assert result.exit_code == 3

    def test_stderr_is_captured_separately_from_stdout(
        self, taz_client: TazClient
    ) -> None:
        result = taz_client.command.exec(
            PY, args=_py("import sys; sys.stderr.write('oops')")
        )
        assert _normalize(result.stderr) == b"oops"
        assert result.stdout == b""

    def test_env_is_merged_into_daemon_environment(self, taz_client: TazClient) -> None:
        snippet = "import os; print(os.environ.get('FOO')); print('PATH' in os.environ)"
        result = taz_client.command.exec(PY, args=_py(snippet), env={"FOO": "bar"})
        lines = _normalize(result.stdout).splitlines()
        assert lines == [b"bar", b"True"]

    def test_working_dir_changes_child_cwd(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        result = taz_client.command.exec(
            PY, args=_py("import os; print(os.getcwd())"), working_dir=str(tmp_path)
        )
        printed = _normalize(result.stdout).strip().decode()
        assert os.path.samefile(printed, tmp_path)

    def test_large_output_spans_multiple_chunks(self, taz_client: TazClient) -> None:
        result = taz_client.command.exec(PY, args=_py("print('x' * 300000)"))
        assert _normalize(result.stdout) == b"x" * 300000 + b"\n"
        assert result.truncated is False

    def test_output_cap_truncates_and_sets_flag(self, taz_client: TazClient) -> None:
        taz_client.config_update({"exec.max_output_bytes": "1000"})
        result = taz_client.command.exec(PY, args=_py("print('x' * 300000)"))
        assert result.truncated is True
        assert len(result.stdout) + len(result.stderr) <= 1000

    def test_nonexistent_command_raises_not_found_and_connection_stays_usable(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        missing = str(tmp_path / "no-such-binary")
        with pytest.raises(TazError) as exc_info:
            taz_client.command.exec(missing)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        taz_client.ping()

    def test_capabilities_advertise_command_exec(self, taz_client: TazClient) -> None:
        cap = taz_client.capabilities()
        assert common_pb2.OPCODE_COMMAND_EXEC in set(cap.operations)

    @pytest.mark.valgrind
    def test_demo_round_trip(self, taz_client: TazClient) -> None:
        result = taz_client.command.exec(PY, args=["-c", "print('demo')"])
        assert result.exit_code == 0
        assert _normalize(result.stdout) == b"demo\n"

    def test_timeout_kills_process_and_sets_timed_out(
        self, taz_client: TazClient
    ) -> None:
        start = time.monotonic()
        result = taz_client.command.exec(
            PY, args=_py("import time; time.sleep(60)"), timeout_ms=200
        )
        assert result.timed_out is True
        assert time.monotonic() - start < 10

    def test_timeout_kills_whole_process_tree(self, taz_client: TazClient) -> None:
        # The child spawns a grandchild sleeper, reports its pid (flushed so
        # the daemon's capture sees it even if the parent is killed right
        # after), then sleeps itself; timeout_ms is generous enough for a
        # fresh interpreter to start and print on a slow (Windows) host.
        snippet = (
            "import subprocess, sys, time; "
            "child = subprocess.Popen([sys.executable, '-c', "
            "'import time; time.sleep(60)']); "
            "print(child.pid); sys.stdout.flush(); "
            "time.sleep(60)"
        )
        result = taz_client.command.exec(PY, args=_py(snippet), timeout_ms=2000)
        assert result.timed_out is True
        grandchild_pid = int(_normalize(result.stdout).strip())

        deadline = time.monotonic() + 5
        while not _process_gone(grandchild_pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        assert _process_gone(grandchild_pid)

    def test_long_exec_does_not_block_a_second_connection(
        self, taz_client: TazClient, daemon: Daemon
    ) -> None:
        result_box: list[CommandResult] = []
        error_box: list[BaseException] = []

        def run_slow_exec() -> None:
            try:
                result_box.append(
                    taz_client.command.exec(PY, args=_py("import time; time.sleep(2)"))
                )
            except BaseException as exc:  # re-raised on the main thread below
                error_box.append(exc)

        thread = threading.Thread(target=run_slow_exec)
        thread.start()
        try:
            # Give the exec a head start so B's call genuinely overlaps it.
            time.sleep(0.5)
            with TazClient("127.0.0.1", daemon.port) as other:
                start = time.monotonic()
                other.version()
                assert time.monotonic() - start < 1.0
        finally:
            thread.join(timeout=10)
        assert not thread.is_alive()

        if error_box:
            raise error_box[0]
        assert result_box[0].exit_code == 0


class TestConnectionTimeout:
    """``TIMEOUT_SET`` establishes a per-connection default for COMMAND_EXEC."""

    def test_default_timeout_kills_long_sleep(self, taz_client: TazClient) -> None:
        taz_client.timeout(500)
        result = taz_client.command.exec(PY, args=_py("import time; time.sleep(2)"))
        assert result.timed_out is True

    def test_default_timeout_zero_runs_to_completion(
        self, taz_client: TazClient
    ) -> None:
        taz_client.timeout(500)
        taz_client.timeout(0)
        result = taz_client.command.exec(PY, args=_py("import time; time.sleep(0.2)"))
        assert result.exit_code == 0
        assert result.timed_out is False

    def test_per_call_timeout_overrides_connection_default(
        self, taz_client: TazClient
    ) -> None:
        taz_client.timeout(60000)
        result = taz_client.command.exec(
            PY, args=_py("import time; time.sleep(2)"), timeout_ms=300
        )
        assert result.timed_out is True

    def test_longer_per_call_timeout_overrides_shorter_default(
        self, taz_client: TazClient
    ) -> None:
        taz_client.timeout(200)
        result = taz_client.command.exec(
            PY, args=_py("import time; time.sleep(1)"), timeout_ms=30000
        )
        assert result.exit_code == 0
        assert result.timed_out is False

    @pytest.mark.valgrind
    def test_returns_previous_default(self, taz_client: TazClient) -> None:
        assert taz_client.timeout(500) == 0
        assert taz_client.timeout(1000) == 500

    def test_capabilities_advertise_timeout_set_and_log_and_run_as_only_when_privileged(
        self, taz_client: TazClient
    ) -> None:
        ops = set(taz_client.capabilities().operations)
        assert common_pb2.OPCODE_TIMEOUT_SET in ops
        assert common_pb2.OPCODE_LOG in ops
        assert (common_pb2.OPCODE_RUN_AS in ops) == _DAEMON_IS_PRIVILEGED


# The fixture's daemon inherits the test process's identity.
_DAEMON_IS_ROOT = sys.platform != "win32" and os.geteuid() == 0
_DAEMON_IS_SYSTEM = sys.platform == "win32" and process_is_system()
_DAEMON_IS_PRIVILEGED = _DAEMON_IS_ROOT or _DAEMON_IS_SYSTEM

_ROOT_ONLY = pytest.mark.skipif(
    not _DAEMON_IS_ROOT,
    reason="RUN_AS's privileged rows need a daemon started as root; no "
    "gate runs as root, so these never run here - the C unit tests "
    "(test_run_as.cpp) are the real proof",
)

_UNPRIVILEGED_ONLY = pytest.mark.skipif(
    _DAEMON_IS_PRIVILEGED,
    reason="a privileged daemon advertises RUN_AS; the test_root_*/"
    "test_system_* rows cover it instead",
)

_SYSTEM_ONLY = pytest.mark.skipif(
    not _DAEMON_IS_SYSTEM,
    reason="RUN_AS's Windows rows need a daemon started as SYSTEM; "
    "`just test-py-system` runs them",
)

_TEST_USER = os.environ.get("TAZ_TEST_USER")


def _test_user() -> str:
    """``TAZ_TEST_USER``, required by every ``system``-marked row."""
    if _TEST_USER is None:
        pytest.fail("TAZ_TEST_USER is required for RUN_AS's SYSTEM rows")
    return _TEST_USER


# A program the RUN_AS target can always execute: sys.executable may sit
# under a home directory (e.g. a 0700 /home/<user>) the target cannot
# traverse.
_ID = "id"

# Windows equivalents: a plain taz-runas account cannot read the mise
# Python under C:\Users\user\AppData, so the Windows RUN_AS rows run only
# %SystemRoot%\System32 programs, for the same reason.
_WHOAMI = os.path.expandvars(r"%SystemRoot%\System32\whoami.exe")
_CMD = os.path.expandvars(r"%SystemRoot%\System32\cmd.exe")
_PING = os.path.expandvars(r"%SystemRoot%\System32\ping.exe")


class TestRunAs:
    """``RUN_AS`` (0x0040) is gated by the daemon's *real* privilege (not a
    client-side switch): root on POSIX, SYSTEM on Windows. Most of this
    class only runs when the daemon is privileged; the Windows rows are
    ``TestRunAsSystem`` below, run by ``just test-py-system``."""

    @pytest.mark.valgrind
    @_UNPRIVILEGED_ONLY
    def test_not_privileged_raw_run_as_frame_gets_not_supported_from_daemon(
        self, taz_client: TazClient
    ) -> None:
        # The client refuses RUN_AS locally when it is unadvertised, so
        # pretend it was advertised: the frame then reaches the daemon's
        # own privilege check.
        caps = taz_client._conn._capabilities
        assert caps is not None
        caps.operations.append(common_pb2.OPCODE_RUN_AS)
        req = advanced_pb2.RunAsRequest(user="someone")
        stream_id = taz_client._conn.send_request(
            common_pb2.OPCODE_RUN_AS, req.SerializeToString()
        )
        frame = taz_client._dispatcher.recv_response(
            stream_id, taz_client._keepalive, expected_opcode=common_pb2.OPCODE_RUN_AS
        )
        assert frame.type == common_pb2.FRAME_TYPE_ERROR
        info = common_pb2.ErrorInfo()
        info.ParseFromString(frame.payload)
        assert info.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        taz_client.ping()

    @_UNPRIVILEGED_ONLY
    def test_not_privileged_run_as_raises_not_supported_and_sends_nothing(
        self, taz_client: TazClient
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.run_as("someone")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        # Local rejection (RUN_AS is unadvertised): the connection is fine.
        taz_client.ping()

    @_UNPRIVILEGED_ONLY
    def test_not_privileged_as_user_returns_not_supported_from_daemon(
        self, taz_client: TazClient
    ) -> None:
        # Unlike run_as() above, COMMAND_EXEC is advertised, so as_user
        # reaches the daemon and comes back as a real ERROR reply.
        with pytest.raises(TazError) as exc_info:
            taz_client.command.exec(PY, args=_py("print('hi')"), as_user="someone")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        taz_client.ping()

    @_ROOT_ONLY
    def test_root_run_as_switches_and_resets_command_exec_identity(
        self, taz_client: TazClient
    ) -> None:
        if sys.platform == "win32":
            return  # unreachable at runtime; gives mypy the platform narrowing
        import pwd

        target_user = os.environ.get("TAZ_TEST_USER", "nobody")
        target_uid = pwd.getpwnam(target_user).pw_uid

        assert taz_client.run_as(target_user) == target_user
        result = taz_client.command.exec(_ID, args=["-u"])
        assert result.stdout.strip() == str(target_uid).encode()

        assert taz_client.run_as("") == pwd.getpwuid(os.geteuid()).pw_name
        result = taz_client.command.exec(_ID, args=["-u"])
        assert result.stdout.strip() == b"0"

    @_ROOT_ONLY
    def test_root_run_as_sets_user_logname_and_home(
        self, taz_client: TazClient
    ) -> None:
        if sys.platform == "win32":
            return  # unreachable at runtime; gives mypy the platform narrowing
        import pwd

        target_user = os.environ.get("TAZ_TEST_USER", "nobody")
        target_home = pwd.getpwnam(target_user).pw_dir or "/"
        show = ["-c", 'printf "%s %s %s" "$USER" "$LOGNAME" "$HOME"']

        assert taz_client.run_as(target_user) == target_user
        result = taz_client.command.exec("sh", args=show)
        assert result.stdout == f"{target_user} {target_user} {target_home}".encode()
        # The request's own env still wins.
        result = taz_client.command.exec("sh", args=show, env={"HOME": "/custom-home"})
        assert result.stdout == f"{target_user} {target_user} /custom-home".encode()

    @_ROOT_ONLY
    def test_root_detach_runs_as_the_connection_identity_or_as_user(
        self, taz_client: TazClient
    ) -> None:
        if sys.platform == "win32":
            return  # unreachable at runtime; gives mypy the platform narrowing
        import pwd

        target_user = os.environ.get("TAZ_TEST_USER", "nobody")
        entry = pwd.getpwnam(target_user)
        show = ["-c", 'printf "%s %s %s" "$(id -u)" "$USER" "$HOME"']

        def detached_output(**kwargs: Any) -> bytes:
            task = taz_client.task.detach(taz_client.command.exec, "sh", show, **kwargs)
            deadline = time.monotonic() + 10
            status = taz_client.task.status(task.task_id)
            while status.state == TaskState.RUNNING:
                assert time.monotonic() < deadline, "task never finished"
                time.sleep(0.05)
                status = taz_client.task.status(task.task_id)
            assert status.state == TaskState.COMPLETED
            assert status.result is not None
            return status.result.stdout

        expected = f"{entry.pw_uid} {target_user} {entry.pw_dir or '/'}".encode()
        assert detached_output(as_user=target_user) == expected
        assert taz_client.run_as(target_user) == target_user
        assert detached_output() == expected

    @_ROOT_ONLY
    def test_root_per_call_as_user_overrides_connection_identity(
        self, taz_client: TazClient
    ) -> None:
        if sys.platform == "win32":
            return  # unreachable at runtime; gives mypy the platform narrowing
        import pwd

        target_user = os.environ.get("TAZ_TEST_USER", "nobody")
        target_uid = pwd.getpwnam(target_user).pw_uid

        result = taz_client.command.exec(_ID, args=["-u"], as_user=target_user)
        assert result.stdout.strip() == str(target_uid).encode()

        assert taz_client.run_as(target_user) == target_user
        result = taz_client.command.exec(_ID, args=["-u"], as_user="root")
        assert result.stdout.strip() == b"0"
        # The override applies to that call only.
        result = taz_client.command.exec(_ID, args=["-u"])
        assert result.stdout.strip() == str(target_uid).encode()

    @_ROOT_ONLY
    def test_root_run_as_still_allows_root_only_file_stat(
        self, taz_client: TazClient
    ) -> None:
        target_user = os.environ.get("TAZ_TEST_USER", "nobody")
        assert taz_client.run_as(target_user) == target_user
        try:
            # Only the spawned COMMAND_EXEC child's identity switches, never
            # the daemon's own - a root-only path stays readable.
            info = taz_client.file.stat("/etc/shadow")
            assert info.size >= 0
        finally:
            taz_client.run_as("")

    @_ROOT_ONLY
    def test_root_run_as_unknown_user_returns_not_found(
        self, taz_client: TazClient
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.run_as("definitely-no-such-user")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    @pytest.mark.system
    def test_demo_timeout_run_as_and_log(self, taz_client: TazClient) -> None:
        taz_client.timeout(5000)
        if sys.platform != "win32" and os.geteuid() == 0:
            import pwd

            target_user = os.environ.get("TAZ_TEST_USER", "nobody")
            assert taz_client.run_as(target_user) == target_user
            result = taz_client.command.exec("whoami")
            assert result.stdout.strip() == target_user.encode()
            assert taz_client.run_as("") == pwd.getpwuid(os.geteuid()).pw_name
        elif _DAEMON_IS_SYSTEM:
            target_user = _test_user()
            assert taz_client.run_as(target_user) == target_user
            result = taz_client.command.exec(_WHOAMI)
            assert (
                result.stdout.decode()
                .strip()
                .lower()
                .endswith(f"\\{target_user.lower()}")
            )
            assert taz_client.run_as("") == "NT AUTHORITY\\SYSTEM"
        result = taz_client.command.exec(PY, args=_py("print('demo')"))
        assert result.exit_code == 0
        entries = taz_client.log(lines=10)
        assert len(entries) >= 1


class TestRunAsSystem:
    """``RUN_AS``'s Windows rows: the daemon must be started as SYSTEM
    through a scheduled task (``just test-py-system``'s lane), which is
    what makes the real S4U logon/``CreateProcessAsUserW`` path
    reachable. Every row here carries both ``@pytest.mark.system`` (the
    lane's own selection) and ``@_SYSTEM_ONLY`` (so it skips, rather than
    fails, off the lane - the lane's own junit check is what turns a skip
    into a failure under CI)."""

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_run_as_switches_and_resets_command_exec_identity(
        self, taz_client: TazClient
    ) -> None:
        target_user = _test_user()
        assert taz_client.run_as(target_user) == target_user
        result = taz_client.command.exec(_WHOAMI)
        assert (
            result.stdout.decode().strip().lower().endswith(f"\\{target_user.lower()}")
        )

        assert taz_client.run_as("") == "NT AUTHORITY\\SYSTEM"
        result = taz_client.command.exec(_WHOAMI)
        assert result.stdout.decode().strip().lower() == "nt authority\\system"

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_per_call_as_user_overrides_connection_identity(
        self, taz_client: TazClient
    ) -> None:
        target_user = _test_user()

        def whoami() -> str:
            result = taz_client.command.exec(_WHOAMI)
            return result.stdout.decode().strip().lower()

        def whoami_as(user: str) -> str:
            result = taz_client.command.exec(_WHOAMI, as_user=user)
            return result.stdout.decode().strip().lower()

        # No connection identity yet: the override runs as the target,
        # a plain call runs as SYSTEM.
        assert whoami_as(target_user).endswith(f"\\{target_user.lower()}")
        assert whoami() == "nt authority\\system"

        assert taz_client.run_as(target_user) == target_user
        assert whoami().endswith(f"\\{target_user.lower()}")

        assert taz_client.run_as("") == "NT AUTHORITY\\SYSTEM"
        assert whoami() == "nt authority\\system"
        assert whoami_as(target_user).endswith(f"\\{target_user.lower()}")
        # The override applies to that call only.
        assert whoami() == "nt authority\\system"

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_detach_runs_as_the_connection_identity_or_as_user(
        self, taz_client: TazClient
    ) -> None:
        target_user = _test_user()

        def detached_output(**kwargs: Any) -> bytes:
            task = taz_client.task.detach(taz_client.command.exec, _WHOAMI, **kwargs)
            deadline = time.monotonic() + 10
            status = taz_client.task.status(task.task_id)
            while status.state == TaskState.RUNNING:
                assert time.monotonic() < deadline, "task never finished"
                time.sleep(0.05)
                status = taz_client.task.status(task.task_id)
            assert status.state == TaskState.COMPLETED
            assert status.result is not None
            return status.result.stdout

        assert (
            detached_output(as_user=target_user)
            .decode()
            .strip()
            .lower()
            .endswith(f"\\{target_user.lower()}")
        )
        assert taz_client.run_as(target_user) == target_user
        assert (
            detached_output()
            .decode()
            .strip()
            .lower()
            .endswith(f"\\{target_user.lower()}")
        )

        task = taz_client.task.detach(
            taz_client.command.exec, _PING, ["-n", "31", "127.0.0.1"]
        )
        status = taz_client.task.status(task.task_id)
        assert status.state == TaskState.RUNNING
        assert taz_client.task.cancel(task.task_id) is True
        deadline = time.monotonic() + 10
        while status.state == TaskState.RUNNING:
            assert time.monotonic() < deadline, "task never finished"
            time.sleep(0.05)
            status = taz_client.task.status(task.task_id)
        assert status.state == TaskState.CANCELLED
        assert _wait_process_gone(task.pid), f"detached pid {task.pid} still exists"

        taz_client.run_as("")

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_child_sees_target_username_and_userprofile(
        self, taz_client: TazClient
    ) -> None:
        target_user = _test_user()
        try:
            assert taz_client.run_as(target_user) == target_user
            result = taz_client.command.exec(_WHOAMI)
            assert (
                result.stdout.decode()
                .strip()
                .lower()
                .endswith(f"\\{target_user.lower()}")
            )

            result = taz_client.command.exec(_CMD, args=["/c", "set"])
            text = result.stdout.decode()
            assert re.search(rf"(?im)^USERNAME={re.escape(target_user)}\r?$", text)
            assert re.search(
                rf"(?im)^USERPROFILE=C:\\Users\\{re.escape(target_user)}", text
            )
            assert re.search(r"(?im)^USER=", text) is None
            assert re.search(r"(?im)^LOGNAME=", text) is None

            result = taz_client.command.exec(
                _CMD, args=["/c", "set"], env={"USERPROFILE": r"C:\custom"}
            )
            text = result.stdout.decode()
            assert re.search(r"(?im)^USERPROFILE=C:\\custom\r?$", text)
            assert re.search(rf"(?im)^USERNAME={re.escape(target_user)}\r?$", text)
        finally:
            taz_client.run_as("")

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_timeout_and_output_cap_on_the_as_user_path(
        self, taz_client: TazClient
    ) -> None:
        target_user = _test_user()
        start = time.monotonic()
        result = taz_client.command.exec(
            _PING,
            args=["-n", "31", "127.0.0.1"],
            as_user=target_user,
            timeout_ms=500,
        )
        assert result.timed_out is True
        assert time.monotonic() - start < 5.0

        result = taz_client.command.exec(
            _CMD,
            args=["/c", "for /l %i in (1,1,200000) do @echo xxxxxxxxxxxxxxxx"],
            as_user=target_user,
        )
        assert result.truncated is True
        assert len(result.stdout) <= 1 << 20

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_timeout_kills_the_grandchild(self, taz_client: TazClient) -> None:
        target_user = _test_user()
        result = taz_client.command.exec(
            _CMD,
            args=["/c", "ping -n 31 127.0.0.1"],
            as_user=target_user,
            timeout_ms=500,
        )
        assert result.timed_out is True

        def survivors() -> list[psutil.Process]:
            found = []
            for proc in psutil.process_iter(["name", "username"]):
                try:
                    info = proc.info
                    name = (info["name"] or "").lower()
                    username = (info["username"] or "").lower()
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    continue
                if name == "ping.exe" and username.endswith(f"\\{target_user.lower()}"):
                    found.append(proc)
            return found

        deadline = time.monotonic() + 5.0
        while survivors():
            assert time.monotonic() < deadline, "a ping.exe survived the job kill"
            time.sleep(0.1)

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_unknown_user_returns_not_found_and_nodomain_is_defined(
        self, taz_client: TazClient
    ) -> None:
        target_user = _test_user()
        with pytest.raises(TazError) as exc_info:
            taz_client.run_as("definitely-no-such-user")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        taz_client.ping()

        with pytest.raises(TazError) as exc_info:
            taz_client.run_as("NODOMAIN\\x")
        assert exc_info.value.code in (
            common_pb2.ERROR_CODE_NOT_FOUND,
            common_pb2.ERROR_CODE_INTERNAL,
        )
        assert "LsaLogonUser" in exc_info.value.message
        taz_client.ping()

        assert taz_client.run_as(target_user) == target_user
        taz_client.run_as("")

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_no_handle_leak_across_run_as_spawns(
        self, taz_client: TazClient, daemon: Daemon
    ) -> None:
        if sys.platform != "win32":
            return  # unreachable at runtime; gives mypy the platform narrowing
        target_user = _test_user()
        proc = psutil.Process(daemon.pid)

        assert taz_client.run_as(target_user) == target_user
        taz_client.command.exec(_WHOAMI)
        taz_client.command.exec(_WHOAMI)
        time.sleep(0.2)
        baseline = proc.num_handles()

        for _ in range(20):
            taz_client.command.exec(_WHOAMI)
        for _ in range(5):
            taz_client.command.exec(_WHOAMI, as_user=target_user)
        for _ in range(2):
            result = taz_client.command.exec(
                _PING, args=["-n", "31", "127.0.0.1"], timeout_ms=200
            )
            assert result.timed_out is True
        with pytest.raises(TazError):
            taz_client.command.exec("no-such-program-7f3a.exe")
        assert taz_client.run_as("") == "NT AUTHORITY\\SYSTEM"
        assert taz_client.run_as(target_user) == target_user

        time.sleep(1.0)
        after = proc.num_handles()
        assert after <= baseline + 2, f"baseline={baseline} after={after}"
        taz_client.run_as("")

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_capabilities_advertise_run_as(self, taz_client: TazClient) -> None:
        ops = set(taz_client.capabilities().operations)
        assert common_pb2.OPCODE_RUN_AS in ops

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_run_as_still_allows_system_only_file_stat(
        self, taz_client: TazClient
    ) -> None:
        target_user = _test_user()
        assert taz_client.run_as(target_user) == target_user
        try:
            info = taz_client.file.stat(r"C:\Windows\System32\config\SAM")
            assert info.size >= 0
        finally:
            taz_client.run_as("")

    @pytest.mark.system
    @_SYSTEM_ONLY
    def test_system_connection_close_with_identity_leaves_no_token(
        self, taz_client: TazClient, daemon: Daemon
    ) -> None:
        if sys.platform != "win32":
            return  # unreachable at runtime; gives mypy the platform narrowing
        target_user = _test_user()
        proc = psutil.Process(daemon.pid)
        time.sleep(0.2)
        baseline = proc.num_handles()

        with TazClient("127.0.0.1", daemon.port) as other:
            assert other.run_as(target_user) == target_user

        time.sleep(0.5)
        after = proc.num_handles()
        assert after <= baseline + 2, f"baseline={baseline} after={after}"


@pytest.mark.slow
class TestCommandExecSlowKeepalive:
    """A long, idle-on-the-wire exec must survive the client's own keepalive
    prober — both with the default prober running and with it disabled.

    Uses dedicated clients rather than ``taz_client`` (idle=60), which is
    longer than the 40 s exec and so would never exercise a PING."""

    def test_default_keepalive_survives_idle_exec(self, daemon: Daemon) -> None:
        with TazClient("127.0.0.1", daemon.port) as client:
            start = time.monotonic()
            result = client.command.exec(
                PY, args=_py("import time; time.sleep(40)"), timeout_ms=0
            )
            elapsed = time.monotonic() - start
        assert result.exit_code == 0
        assert result.timed_out is False
        assert elapsed >= 40

    def test_keepalive_off_survives_idle_exec(self, daemon: Daemon) -> None:
        with TazClient("127.0.0.1", daemon.port, keepalive=Keepalive.OFF) as client:
            result = client.command.exec(
                PY, args=_py("import time; time.sleep(40)"), timeout_ms=0
            )
        assert result.exit_code == 0
        assert result.timed_out is False


class TestFileOps:
    """``FILE_CREATE``/``DELETE``/``STAT``/``CHMOD`` through the typed client API."""

    _posix = sys.platform != "win32"

    @pytest.mark.valgrind
    def test_create_then_stat_reports_metadata(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        path = tmp_path / "created.txt"
        taz_client.file.create(str(path), content=b"Hello from TAZ")
        info = taz_client.file.stat(str(path))
        assert info.size == 14
        assert info.kind == Kind.FILE
        assert info.link_target == ""
        assert abs(info.modified - time.time()) < 60
        if self._posix:
            assert info.owner != ""

    @pytest.mark.valgrind
    def test_chmod_changes_permissions(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        path = tmp_path / "chmod.txt"
        taz_client.file.create(str(path))
        if self._posix:
            taz_client.file.chmod(str(path), 0o644)
            assert taz_client.file.stat(str(path)).permissions == 0o644
            taz_client.file.chmod(str(path), 0o600)
            assert taz_client.file.stat(str(path)).permissions == 0o600
        else:
            taz_client.file.chmod(str(path), 0o444)
            assert taz_client.file.stat(str(path)).permissions & 0o222 == 0
            assert os.access(path, os.W_OK) is False

    @pytest.mark.valgrind
    def test_delete_removes_file_and_stat_then_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        path = tmp_path / "deleteme.txt"
        taz_client.file.create(str(path))
        taz_client.file.delete(str(path))
        assert not path.exists()
        with pytest.raises(TazError) as exc_info:
            taz_client.file.stat(str(path))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_delete_nonexistent_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.file.delete(str(tmp_path / "no-such-file"))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_create_existing_raises_already_exists(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        path = tmp_path / "exists.txt"
        taz_client.file.create(str(path))
        with pytest.raises(TazError) as exc_info:
            taz_client.file.create(str(path))
        assert exc_info.value.code == common_pb2.ERROR_CODE_ALREADY_EXISTS

    def test_stat_nonexistent_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.file.stat(str(tmp_path / "no-such-file"))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_create_under_missing_directory_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.file.create(str(tmp_path / "no-such-dir" / "created.txt"))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_delete_directory_raises_and_directory_survives(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        sub = tmp_path / "subdir"
        sub.mkdir()
        with pytest.raises(TazError) as exc_info:
            taz_client.file.delete(str(sub))
        assert exc_info.value.code != common_pb2.ERROR_CODE_NOT_FOUND
        assert sub.is_dir()

    def test_symlink_reports_kind_symlink_and_target(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        target = tmp_path / "target.txt"
        target.write_text("x")
        link = tmp_path / "link"
        try:
            os.symlink("target.txt", link)
        except OSError:
            if sys.platform != "win32":
                raise
            pytest.skip("symlink privilege")
        info = taz_client.file.stat(str(link))
        assert info.kind == Kind.SYMLINK
        assert info.link_target == "target.txt"
        assert taz_client.file.stat(str(target)).kind == Kind.FILE

    def test_create_oversized_content_raises_value_error_without_round_trip(
        self,
        taz_client: TazClient,
        tmp_path: Path,
        monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        def _fail_on_send(*args: object, **kwargs: object) -> int:
            pytest.fail("file.create must not round-trip oversized content")

        monkeypatch.setattr(taz_client._conn, "send_request", _fail_on_send)
        with pytest.raises(ValueError):
            taz_client.file.create(str(tmp_path / "big.txt"), content=b"x" * 100 * 1024)

    def test_capabilities_advertise_file_and_directory_opcodes(
        self, taz_client: TazClient
    ) -> None:
        cap = taz_client.capabilities()
        assert set(cap.operations) >= _FILE_OPCODES

    def test_demo_create_stat_list(self, taz_client: TazClient, tmp_path: Path) -> None:
        path = tmp_path / "hello.txt"
        taz_client.file.create(str(path), content=b"Hello from TAZ")
        info = taz_client.file.stat(str(path))
        print(f"Size: {info.size}, Kind: {info.kind}")
        listing = [
            f"  {e.name} ({e.kind.name})"
            for e in taz_client.directory.list(str(tmp_path))
        ]
        for line in listing:
            print(line)
        assert "  hello.txt (FILE)" in listing


class TestDirectoryOps:
    """``DIR_MAKE``/``LIST``/``REMOVE`` through the typed client API."""

    @pytest.mark.valgrind
    def test_make_then_stat_reports_kind_dir(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        path = tmp_path / "newdir"
        taz_client.directory.make(str(path))
        assert taz_client.file.stat(str(path)).kind == Kind.DIR

    def test_make_existing_raises_already_exists(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        path = tmp_path / "exists"
        taz_client.directory.make(str(path))
        with pytest.raises(TazError) as exc_info:
            taz_client.directory.make(str(path))
        assert exc_info.value.code == common_pb2.ERROR_CODE_ALREADY_EXISTS

    def test_make_nested_without_parents_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.directory.make(str(tmp_path / "a" / "b" / "c"))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_make_nested_with_parents_creates_every_level(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        leaf = tmp_path / "a" / "b" / "c"
        taz_client.directory.make(str(leaf), parents=True)
        for level in (tmp_path / "a", tmp_path / "a" / "b", leaf):
            assert taz_client.file.stat(str(level)).kind == Kind.DIR

    @pytest.mark.valgrind
    def test_list_reports_kind_and_size_per_entry(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        (tmp_path / "file.txt").write_bytes(b"abc")
        (tmp_path / "sub").mkdir()
        (tmp_path / ".hidden").write_bytes(b"")
        have_link = True
        try:
            os.symlink("file.txt", tmp_path / "link")
        except OSError:
            if sys.platform != "win32":
                raise
            have_link = False

        visible = {
            e.name: (e.kind, e.size) for e in taz_client.directory.list(str(tmp_path))
        }
        assert visible["file.txt"] == (Kind.FILE, 3)
        assert visible["sub"] == (Kind.DIR, 0)
        if have_link:
            assert visible["link"] == (Kind.SYMLINK, 0)
        assert ".hidden" not in visible

        hidden = {
            e.name
            for e in taz_client.directory.list(str(tmp_path), include_hidden=True)
        }
        assert ".hidden" in hidden

    def test_list_large_directory_returns_every_entry(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        names = _touch_files(tmp_path, 5000)
        entries = taz_client.directory.list(str(tmp_path))
        assert len(entries) == 5000
        assert {e.name for e in entries} == names

    def test_list_empty_directory_returns_empty_list(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        empty = tmp_path / "empty"
        empty.mkdir()
        assert taz_client.directory.list(str(empty)) == []

    def test_list_nonexistent_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.directory.list(str(tmp_path / "no-such-dir"))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    @pytest.mark.valgrind
    def test_remove_empty_directory(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        path = tmp_path / "gone"
        path.mkdir()
        taz_client.directory.remove(str(path))
        assert not path.exists()

    def test_remove_nonempty_without_recursive_raises_and_survives(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        path = tmp_path / "nonempty"
        path.mkdir()
        (path / "child.txt").write_bytes(b"x")
        with pytest.raises(TazError):
            taz_client.directory.remove(str(path))
        assert path.exists()

    def test_remove_recursive_deletes_tree(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        root = tmp_path / "tree"
        (root / "sub").mkdir(parents=True)
        (root / "sub" / "child.txt").write_bytes(b"x")
        readonly = root / "readonly.txt"
        readonly.write_bytes(b"x")
        readonly.chmod(0o444)
        taz_client.directory.remove(str(root), recursive=True)
        assert not root.exists()

    def test_remove_nonexistent_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.directory.remove(str(tmp_path / "no-such-dir"))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_closing_during_dir_list_work_stays_usable(
        self, daemon: Daemon, tmp_path: Path
    ) -> None:
        """A dangling DIR_LIST must not block a second connection or corrupt
        the daemon's connection state: work callbacks run off the loop
        thread (a data race there only shows under TSan), and a wrong
        ``closing`` check in the after-work callback only shows under ASan
        or Valgrind."""
        _touch_files(tmp_path, 5000)
        req = file_pb2.DirListRequest(path=str(tmp_path))

        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        # 32 in-flight requests, never read: with the default libuv thread
        # pool (4 workers) this guarantees some are queued and some running
        # when the socket closes underneath them.
        for _ in range(32):
            victim._conn.send_request(
                common_pb2.OPCODE_DIR_LIST, req.SerializeToString()
            )
        victim.close()

        with TazClient("127.0.0.1", daemon.port) as other:
            start = time.monotonic()
            other.version()
            assert time.monotonic() - start < 2.0
            other.file.stat(str(tmp_path))


class TestFileTransfer:
    """``FILE_PUT``/``FILE_GET`` chunked transfers through the typed client API."""

    @staticmethod
    def _crc(data: bytes) -> bytes:
        return google_crc32c.value(data).to_bytes(4, "little")

    @staticmethod
    def _no_temps(directory: Path) -> None:
        leftover = [p.name for p in directory.iterdir() if ".taz-" in p.name]
        assert leftover == [], f"leftover temp files in {directory}: {leftover}"

    @pytest.mark.valgrind
    @pytest.mark.parametrize("size", [1024, 0, 4 * 1024 * 1024 + 1])
    def test_round_trip_bytes_and_checksum_match(
        self, taz_client: TazClient, tmp_path: Path, size: int
    ) -> None:
        data = os.urandom(size)
        src = tmp_path / "src.bin"
        src.write_bytes(data)
        remote_dir = tmp_path / "remote"
        remote_dir.mkdir()
        remote_path = remote_dir / "uploaded.bin"
        checksum = self._crc(data)

        put_result = taz_client.file.put(str(src), str(remote_path))
        assert put_result == FileTransfer(size=size, checksum=checksum)
        assert taz_client.file.stat(str(remote_path)).size == size
        self._no_temps(remote_dir)

        download = tmp_path / "downloaded.bin"
        get_result = taz_client.file.get(str(remote_path), str(download))
        assert get_result == put_result
        assert download.read_bytes() == data
        self._no_temps(tmp_path)

    def test_put_reports_custom_permissions_on_posix(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        if sys.platform == "win32":
            pytest.skip("permission bits are POSIX-only")
        data = os.urandom(4 * 1024 * 1024 + 1)
        src = tmp_path / "src.bin"
        src.write_bytes(data)
        remote_path = tmp_path / "uploaded.bin"
        taz_client.file.put(str(src), str(remote_path), permissions=0o600)
        assert taz_client.file.stat(str(remote_path)).permissions == 0o600

    def test_overwrite_false_onto_existing_raises_and_leaves_bytes_untouched(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        remote_path = tmp_path / "existing.bin"
        remote_path.write_bytes(b"old bytes")
        src = tmp_path / "src.bin"
        src.write_bytes(b"new bytes, longer than old")

        with pytest.raises(TazError) as exc_info:
            taz_client.file.put(str(src), str(remote_path))
        assert exc_info.value.code == common_pb2.ERROR_CODE_ALREADY_EXISTS
        assert remote_path.read_bytes() == b"old bytes"
        self._no_temps(tmp_path)

    def test_overwrite_true_replaces_existing_file(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        remote_path = tmp_path / "existing.bin"
        remote_path.write_bytes(b"old bytes")
        src = tmp_path / "src.bin"
        src.write_bytes(b"new bytes, longer than old")

        taz_client.file.put(str(src), str(remote_path), overwrite=True)
        assert remote_path.read_bytes() == b"new bytes, longer than old"

    def test_put_to_missing_parent_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        src = tmp_path / "src.bin"
        src.write_bytes(b"x")
        with pytest.raises(TazError) as exc_info:
            taz_client.file.put(str(src), str(tmp_path / "no-such-dir" / "dest.bin"))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_put_onto_directory_raises_invalid_request(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        src = tmp_path / "src.bin"
        src.write_bytes(b"x")
        target_dir = tmp_path / "subdir"
        target_dir.mkdir()
        with pytest.raises(TazError) as exc_info:
            taz_client.file.put(str(src), str(target_dir))
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST

    def test_get_nonexistent_raises_not_found(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.file.get(
                str(tmp_path / "no-such.bin"), str(tmp_path / "local.bin")
            )
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_get_directory_raises_invalid_request_and_leaves_no_local_temp(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        remote_dir = tmp_path / "remotedir"
        remote_dir.mkdir()
        with pytest.raises(TazError) as exc_info:
            taz_client.file.get(str(remote_dir), str(tmp_path / "local.bin"))
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        self._no_temps(tmp_path)

    def test_put_fewer_bytes_than_announced_size_raises_invalid_request(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        remote_path = tmp_path / "dest.bin"
        kv = taz_client._keepalive
        req = file_pb2.FilePutRequest(
            dest=str(remote_path), size=100, permissions=0o644
        )
        stream_id = taz_client._conn.send_request(
            common_pb2.OPCODE_FILE_PUT, req.SerializeToString()
        )
        ack = taz_client._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_PUT
        )
        ack_resp = file_pb2.FilePutResponse()
        ack_resp.ParseFromString(ack.payload)
        assert ack_resp.ack.ready is True

        taz_client._conn.send_file_chunk(stream_id, b"x" * 10, last=True)
        frame = taz_client._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_PUT
        )
        assert frame.type == common_pb2.FRAME_TYPE_ERROR
        info = common_pb2.ErrorInfo()
        info.ParseFromString(frame.payload)
        assert info.code == common_pb2.ERROR_CODE_INVALID_REQUEST

        assert not remote_path.exists()
        self._no_temps(tmp_path)
        taz_client.ping()

    def test_put_more_bytes_than_announced_size_raises_invalid_request(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        remote_path = tmp_path / "dest.bin"
        kv = taz_client._keepalive
        req = file_pb2.FilePutRequest(dest=str(remote_path), size=10, permissions=0o644)
        stream_id = taz_client._conn.send_request(
            common_pb2.OPCODE_FILE_PUT, req.SerializeToString()
        )
        ack = taz_client._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_PUT
        )
        ack_resp = file_pb2.FilePutResponse()
        ack_resp.ParseFromString(ack.payload)
        assert ack_resp.ack.ready is True

        taz_client._conn.send_file_chunk(stream_id, b"x" * 100, last=True)
        frame = taz_client._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_PUT
        )
        assert frame.type == common_pb2.FRAME_TYPE_ERROR
        info = common_pb2.ErrorInfo()
        info.ParseFromString(frame.payload)
        assert info.code == common_pb2.ERROR_CODE_INVALID_REQUEST

        assert not remote_path.exists()
        self._no_temps(tmp_path)
        taz_client.ping()

    def test_concurrent_uploads_from_two_clients_both_succeed(
        self, taz_client: TazClient, daemon: Daemon, tmp_path: Path
    ) -> None:
        data_a = os.urandom(512 * 1024)
        data_b = os.urandom(512 * 1024)
        src_a = tmp_path / "a.bin"
        src_b = tmp_path / "b.bin"
        src_a.write_bytes(data_a)
        src_b.write_bytes(data_b)
        dest_a = tmp_path / "dest_a.bin"
        dest_b = tmp_path / "dest_b.bin"

        results: dict[str, FileTransfer | BaseException] = {}

        def upload(name: str, client: TazClient, src: Path, dest: Path) -> None:
            try:
                results[name] = client.file.put(str(src), str(dest))
            except BaseException as exc:  # re-raised on the main thread below
                results[name] = exc

        with TazClient("127.0.0.1", daemon.port) as other:
            t_a = threading.Thread(target=upload, args=("a", taz_client, src_a, dest_a))
            t_b = threading.Thread(target=upload, args=("b", other, src_b, dest_b))
            t_a.start()
            t_b.start()
            t_a.join(timeout=10)
            t_b.join(timeout=10)

            assert not t_a.is_alive()
            assert not t_b.is_alive()

        for name in ("a", "b"):
            result = results[name]
            if isinstance(result, BaseException):
                raise result
        assert results["a"] == FileTransfer(
            size=len(data_a), checksum=self._crc(data_a)
        )
        assert results["b"] == FileTransfer(
            size=len(data_b), checksum=self._crc(data_b)
        )
        assert dest_a.read_bytes() == data_a
        assert dest_b.read_bytes() == data_b

    def test_capabilities_advertise_put_get_and_cancel(
        self, taz_client: TazClient
    ) -> None:
        cap = taz_client.capabilities()
        ops = set(cap.operations)
        assert common_pb2.OPCODE_FILE_PUT in ops
        assert common_pb2.OPCODE_FILE_GET in ops
        assert common_pb2.OPCODE_CANCEL in ops

    def test_demo_put_then_get_large_file(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        data = os.urandom(1024 * 1024)
        large_file = tmp_path / "large_file.bin"
        large_file.write_bytes(data)
        remote_path = tmp_path / "remote_large_file.bin"
        downloaded = tmp_path / "downloaded.bin"

        taz_client.file.put(str(large_file), str(remote_path))
        taz_client.file.get(str(remote_path), str(downloaded))

        assert downloaded.read_bytes() == data

    @pytest.mark.valgrind
    def test_cancel_unknown_stream_returns_false(self, taz_client: TazClient) -> None:
        assert taz_client.cancel(0xDEAD) is False

    def test_cancel_upload_after_two_chunks_leaves_no_dest_or_temp(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        remote_dir = tmp_path / "remote"
        remote_dir.mkdir()
        dest = remote_dir / "dest.bin"
        kv = taz_client._keepalive
        req = file_pb2.FilePutRequest(dest=str(dest), size=1000, permissions=0o644)
        stream_id = taz_client._conn.send_request(
            common_pb2.OPCODE_FILE_PUT, req.SerializeToString()
        )
        ack = taz_client._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_PUT
        )
        ack_resp = file_pb2.FilePutResponse()
        ack_resp.ParseFromString(ack.payload)
        assert ack_resp.ack.ready is True

        taz_client._conn.send_file_chunk(stream_id, b"x" * 10, last=False)
        taz_client._conn.send_file_chunk(stream_id, b"y" * 10, last=False)

        assert taz_client.cancel(stream_id) is True

        assert not dest.exists()
        self._no_temps(remote_dir)
        taz_client.ping()

        # The stream_id and dest are free again: a fresh put() succeeds.
        src = tmp_path / "src.bin"
        src.write_bytes(b"after cancel")
        result = taz_client.file.put(str(src), str(dest))
        assert result == FileTransfer(
            size=len(b"after cancel"), checksum=self._crc(b"after cancel")
        )
        assert dest.read_bytes() == b"after cancel"

    def test_cancel_download_after_two_chunks_connection_stays_usable(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        # Large enough that the daemon's SENDING_CHUNKS stream genuinely
        # blocks on its 256 KiB write-queue backpressure mark once we stop
        # reading after 2 chunks (we never drain the socket further below) -
        # a small file lets the daemon race ahead and finish the whole
        # transfer before cancel() is sent, making "target not active"
        # (cancelled=false) a real, observed flake instead of this test's
        # intended "still SENDING_CHUNKS" case.
        data = os.urandom(16 * 1024 * 1024)
        src = tmp_path / "src.bin"
        src.write_bytes(data)
        remote_path = tmp_path / "remote.bin"
        taz_client.file.put(str(src), str(remote_path))

        kv = taz_client._keepalive
        req = file_pb2.FileGetRequest(src=str(remote_path))
        stream_id = taz_client._conn.send_request(
            common_pb2.OPCODE_FILE_GET, req.SerializeToString()
        )
        meta_frame = taz_client._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_GET
        )
        meta = file_pb2.FileGetResponse()
        meta.ParseFromString(meta_frame.payload)
        assert meta.size == len(data)

        for _ in range(2):
            frame = taz_client._dispatcher.recv_response(
                stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_GET
            )
            assert frame.type == common_pb2.FRAME_TYPE_FILE_CHUNK
            assert frame.flags & common_pb2.FRAME_FLAG_CONTINUATION

        assert taz_client.cancel(stream_id) is True

        taz_client.ping()
        download = tmp_path / "downloaded.bin"
        get_result = taz_client.file.get(str(remote_path), str(download))
        assert get_result == FileTransfer(size=len(data), checksum=self._crc(data))
        assert download.read_bytes() == data

    def test_closing_socket_mid_upload_leaves_no_dest_and_connection_stays_usable(
        self, daemon: Daemon, tmp_path: Path
    ) -> None:
        remote_dir = tmp_path / "remote"
        remote_dir.mkdir()
        dest = remote_dir / "dest.bin"

        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        kv = victim._keepalive
        req = file_pb2.FilePutRequest(dest=str(dest), size=100, permissions=0o644)
        stream_id = victim._conn.send_request(
            common_pb2.OPCODE_FILE_PUT, req.SerializeToString()
        )
        ack = victim._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_PUT
        )
        ack_resp = file_pb2.FilePutResponse()
        ack_resp.ParseFromString(ack.payload)
        assert ack_resp.ack.ready is True
        victim._conn.send_file_chunk(stream_id, b"x" * 10, last=False)

        # Abrupt teardown: close the socket mid-transfer, no CANCEL sent, no
        # final chunk - mirrors TestConnectionLifetime's exec-side variant.
        victim.close()

        with TazClient("127.0.0.1", daemon.port) as other:
            start = time.monotonic()
            other.version()
            assert time.monotonic() - start < 2.0

        assert not dest.exists()
        # The daemon closes and unlinks the temp on its thread pool after it
        # sees the socket close, and nothing answers for that, so the
        # version() round trip above does not order it: wait for the unlink.
        deadline = time.monotonic() + 10
        while (
            any(".taz-" in p.name for p in remote_dir.iterdir())
            and time.monotonic() < deadline
        ):
            time.sleep(0.01)
        self._no_temps(remote_dir)

    def test_sigterm_mid_upload_raises_connection_lost_and_leaves_no_dest(
        self, taz_client: TazClient, daemon: Daemon, tmp_path: Path
    ) -> None:
        data = os.urandom(32 * 1024 * 1024)
        src = tmp_path / "src.bin"
        src.write_bytes(data)
        remote_dir = tmp_path / "remote"
        remote_dir.mkdir()
        dest = remote_dir / "dest.bin"

        errors: list[BaseException] = []

        def upload() -> None:
            try:
                taz_client.file.put(str(src), str(dest))
            except BaseException as exc:  # re-raised on the main thread below
                errors.append(exc)

        t = threading.Thread(target=upload)
        t.start()
        deadline = time.monotonic() + 10
        while (
            not any(".taz-" in p.name for p in remote_dir.iterdir())
            and time.monotonic() < deadline
        ):
            time.sleep(0.01)
        assert any(".taz-" in p.name for p in remote_dir.iterdir()), (
            "temp file never appeared"
        )

        daemon.stop()
        t.join(timeout=10)
        assert not t.is_alive()

        assert len(errors) == 1
        assert isinstance(errors[0], TazConnectionLost)
        assert not dest.exists()

    def test_sigterm_mid_download_raises_connection_lost_and_leaves_no_local_temp(
        self, taz_client: TazClient, daemon: Daemon, tmp_path: Path
    ) -> None:
        data = os.urandom(32 * 1024 * 1024)
        src = tmp_path / "src.bin"
        src.write_bytes(data)
        remote_path = tmp_path / "remote.bin"
        taz_client.file.put(str(src), str(remote_path))

        download = tmp_path / "downloaded.bin"
        errors: list[BaseException] = []

        def fetch() -> None:
            try:
                taz_client.file.get(str(remote_path), str(download))
            except BaseException as exc:  # re-raised on the main thread below
                errors.append(exc)

        t = threading.Thread(target=fetch)
        t.start()
        deadline = time.monotonic() + 10
        while (
            not any(".taz-" in p.name for p in tmp_path.iterdir() if p.is_file())
            and time.monotonic() < deadline
        ):
            time.sleep(0.01)
        assert any(".taz-" in p.name for p in tmp_path.iterdir() if p.is_file()), (
            "local temp file never appeared"
        )

        daemon.stop()
        t.join(timeout=10)
        assert not t.is_alive()

        assert len(errors) == 1
        assert isinstance(errors[0], TazConnectionLost)
        assert not download.exists()
        self._no_temps(tmp_path)

    @pytest.mark.skipif(
        sys.platform == "win32",
        reason="no way to cap tazd.exe's memory from the fixture on Windows",
    )
    def test_large_upload_succeeds_under_a_tight_memory_cap(
        self,
        taz_client: TazClient,
        daemon: Daemon,
        daemon_binary: Path,
        tmp_path: Path,
    ) -> None:
        if sys.platform == "win32":
            return  # unreachable at runtime; gives mypy the platform narrowing
        import resource

        # One failed FILE_STAT warms the daemon's thread pool before the cap
        # is set, matching the "already warmed" vms baseline this test relies
        # on (a cold pool's first allocation would blow the cap instead).
        with pytest.raises(TazError):
            taz_client.file.stat(str(tmp_path / "does-not-exist"))
        vms = psutil.Process(daemon.proc.pid).memory_info().vms
        # ASan's own redzones/quarantine add real overhead across the
        # ~1000 FILE_CHUNK allocations a 64 MiB/64 KiB-chunk upload makes,
        # on top of whatever margin the daemon itself needs; a plain build
        # only needs headroom for its own bounded chunk buffer. Both are
        # still far tighter than "big enough to hold the whole 64 MiB file".
        margin = (
            256 * 1024 * 1024
            if "asan" in str(daemon_binary).lower()
            else 24 * 1024 * 1024
        )
        cap = vms + margin
        resource.prlimit(daemon.proc.pid, resource.RLIMIT_AS, (cap, cap))

        data = os.urandom(64 * 1024 * 1024)
        src = tmp_path / "big.bin"
        src.write_bytes(data)
        dest = tmp_path / "remote_big.bin"

        result = taz_client.file.put(str(src), str(dest))
        assert result == FileTransfer(size=len(data), checksum=self._crc(data))
        assert dest.read_bytes() == data


class TestProcessOps:
    """``PROCESS_LIST``/``KILL``/``INFO`` through the typed client API."""

    @pytest.mark.valgrind
    def test_list_includes_daemon_pid_with_psutil_cross_checks(
        self, taz_client: TazClient, daemon: Daemon
    ) -> None:
        entries = {p.pid: p for p in taz_client.process.list()}
        assert daemon.proc.pid in entries
        entry = entries[daemon.proc.pid]
        ref = psutil.Process(daemon.proc.pid)
        assert entry.name == ref.name()
        expected_users = {ref.username()}
        if sys.platform != "win32":
            expected_users.add(str(os.getuid()))
        assert entry.user in expected_users
        assert entry.state != ""
        assert entry.memory_bytes > 0
        assert entry.cpu_percent >= 0

    def test_list_filter_includes_helper(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        pids = {p.pid for p in taz_client.process.list(filter=_python_name_filter())}
        assert pid in pids

    def test_list_nonsense_filter_returns_empty(self, taz_client: TazClient) -> None:
        assert taz_client.process.list(filter="no-such-process-name-7f3a") == []

    def test_info_cross_checks_with_psutil_and_open_files(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
        tmp_path: Path,
    ) -> None:
        scratch = tmp_path / "held-open.bin"
        scratch.write_bytes(b"")
        snippet = (
            "import os, time\n"
            "print(os.getpid(), flush=True)\n"
            f"f = open({str(scratch)!r}, 'rb')\n"
            "time.sleep(60)\n"
        )
        _, pid = spawn_helper(snippet)
        ref = psutil.Process(pid)

        info = taz_client.process.info(pid)
        assert info.pid == pid
        assert "time.sleep" in info.command_line
        assert info.name == ref.name()
        assert abs(info.start_time - int(ref.create_time())) <= 1
        expected_users = {ref.username()}
        if sys.platform != "win32":
            expected_users.add(str(os.getuid()))
        assert info.user in expected_users
        if sys.platform != "win32":
            assert str(scratch) in info.open_files
        else:
            assert info.open_files == []

    @pytest.mark.valgrind
    def test_kill_then_info_and_kill_again_not_found(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        proc, pid = spawn_helper(_SLEEP_SNIPPET)
        assert taz_client.process.kill(pid) is True
        proc.wait(timeout=10)
        deadline = time.monotonic() + 5
        while not _process_gone(pid) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert _process_gone(pid)

        with pytest.raises(TazError) as exc_info:
            taz_client.process.info(pid)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

        # The pid is free now and could be reused. On POSIX, SIGCONT does
        # nothing to a process that took it over. Windows has no harmless
        # signal, so there the second kill runs only while proc's handle
        # keeps the pid from being reused (no venv launcher in between).
        if sys.platform != "win32":
            with pytest.raises(TazError) as exc_info:
                taz_client.process.kill(pid, signal=signal.SIGCONT)
            assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        elif pid == proc.pid:
            with pytest.raises(TazError) as exc_info:
                taz_client.process.kill(pid)
            assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_unused_pid_not_found_for_kill_and_info(
        self, taz_client: TazClient
    ) -> None:
        pid = _unused_pid()
        with pytest.raises(TazError) as exc_info:
            taz_client.process.kill(pid)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        with pytest.raises(TazError) as exc_info:
            taz_client.process.info(pid)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_kill_and_info_reject_pid_zero(self, taz_client: TazClient) -> None:
        # SIGCONT, not the default SIGTERM: the daemon shares pytest's
        # process group, which kill(0, ...) signals, so a regressed guard
        # must not be able to stop the test run.
        with pytest.raises(TazError) as exc_info:
            if sys.platform != "win32":
                taz_client.process.kill(0, signal=signal.SIGCONT)
            else:
                taz_client.process.kill(0)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        with pytest.raises(TazError) as exc_info:
            taz_client.process.info(0)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST

    def test_kill_and_info_reject_pid_above_int32_max(
        self, taz_client: TazClient
    ) -> None:
        pid = 2**31
        with pytest.raises(TazError) as exc_info:
            taz_client.process.kill(pid)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        with pytest.raises(TazError) as exc_info:
            taz_client.process.info(pid)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST

    def test_kill_negative_signal_rejected_helper_stays_alive(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        proc, pid = spawn_helper(_SLEEP_SNIPPET)
        with pytest.raises(TazError) as exc_info:
            taz_client.process.kill(pid, signal=-1)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        assert proc.poll() is None

    def test_capabilities_advertise_process_opcodes_including_monitor(
        self, taz_client: TazClient
    ) -> None:
        ops = set(taz_client.capabilities().operations)
        assert ops >= _PROCESS_OPCODES

    def test_demo_list_and_info(self, taz_client: TazClient, daemon: Daemon) -> None:
        entries = taz_client.process.list()
        lines = [f"PID {p.pid}: {p.name} ({p.state})" for p in entries]
        for line in lines:
            print(line)
        assert len({p.pid for p in entries}) == len(entries)

        command_line = taz_client.process.info(pid=daemon.proc.pid).command_line
        print(command_line)
        assert "tazd" in command_line

    @pytest.mark.skipif(
        sys.platform == "win32", reason="SIGUSR1 does not exist on Windows"
    )
    def test_kill_with_sigusr1_runs_helper_handler(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        proc, pid = spawn_helper(_SIGUSR1_SNIPPET)
        assert proc.stdout is not None
        assert proc.stdout.readline().strip() == b"ready"
        if sys.platform != "win32":
            assert taz_client.process.kill(pid, signal=signal.SIGUSR1) is True
        assert proc.stdout.readline().strip() == b"usr1"

    @pytest.mark.skipif(
        sys.platform == "win32",
        reason="process names are arbitrary bytes only on Linux (/proc/self/comm)",
    )
    def test_invalid_utf8_comm_sanitized_in_list_and_info(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        proc, pid = spawn_helper(_INVALID_UTF8_COMM_SNIPPET)
        assert proc.stdout is not None
        assert proc.stdout.readline().strip() == b"ready"

        entries = {p.pid: p for p in taz_client.process.list()}
        assert entries[pid].name == "ab��cd"
        assert taz_client.process.info(pid).name == "ab��cd"

    def test_closing_socket_mid_process_list_stays_usable(self, daemon: Daemon) -> None:
        req = process_pb2.ProcessListRequest(filter="")

        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        # 32 in-flight requests, never read: with the default libuv thread
        # pool (4 workers) this guarantees some are queued and some running
        # when the socket closes underneath them.
        for _ in range(32):
            victim._conn.send_request(
                common_pb2.OPCODE_PROCESS_LIST, req.SerializeToString()
            )
        victim.close()

        with TazClient("127.0.0.1", daemon.port) as other:
            start = time.monotonic()
            other.version()
            assert time.monotonic() - start < 2.0
            other.process.list()

    def test_sigterm_mid_process_list_exit_audit_passes(
        self, taz_client: TazClient, daemon: Daemon
    ) -> None:
        req = process_pb2.ProcessListRequest(filter="")
        for _ in range(32):
            taz_client._conn.send_request(
                common_pb2.OPCODE_PROCESS_LIST, req.SerializeToString()
            )
        daemon.stop()
        # Nothing further to assert here: the daemon fixture's teardown
        # re-stops (idempotent) and audits the exit for sanitizer reports
        # and a non-zero code.


class TestProcessMonitor:
    """``PROCESS_MONITOR`` through the typed ``client.process.monitor()``
    API (``StreamIterator[ProcessMonitorUpdate]``)."""

    def test_updates_report_live_process_fields(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        ref = psutil.Process(pid)
        m = taz_client.process.monitor(pid, _MONITOR_INTERVAL)
        first, second, _third = _drain(m, 3)

        assert first.exited is False
        assert first.reason == ""
        assert first.info.pid == pid
        assert first.info.name == ref.name()
        assert first.info.memory_bytes > 0
        assert first.info.state != ""
        expected_users = {ref.username()}
        if sys.platform != "win32":
            expected_users.add(str(os.getuid()))
        assert first.info.user in expected_users
        assert first.info.cpu_percent == 0
        assert second.info.cpu_percent >= 0

        m.close()
        assert m.ended
        taz_client.version()

    @pytest.mark.filterwarnings("error::ResourceWarning")
    def test_exit_detected_via_pidfd(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        m = taz_client.process.monitor(pid, _MONITOR_INTERVAL)
        next(m)
        psutil.Process(pid).kill()

        final = _wait_final(m, 5.0)
        assert final.exited is True
        assert final.reason == "exited"
        assert final.exit_code_known is (sys.platform == "win32")
        with pytest.raises(StopIteration):
            next(m)
        assert m.ended
        assert taz_client.cancel(m.stream_id) is False

        del m
        gc.collect()
        taz_client.ping()

    @pytest.mark.filterwarnings("error::ResourceWarning")
    def test_exit_detected_via_timer_fallback(
        self,
        taz_client_no_pidfd: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        m = taz_client_no_pidfd.process.monitor(pid, _MONITOR_INTERVAL)
        next(m)
        psutil.Process(pid).kill()

        final = _wait_final(m, 5.0)
        assert final.exited is True
        assert final.reason == "exited"
        assert final.exit_code_known is (sys.platform == "win32")
        with pytest.raises(StopIteration):
            next(m)
        assert m.ended
        assert taz_client_no_pidfd.cancel(m.stream_id) is False

        del m
        gc.collect()
        taz_client_no_pidfd.ping()

    def test_detached_task_exit_code_is_authoritative_via_monitor(
        self, taz_client: TazClient
    ) -> None:
        # A short sleep first so the exit (and its registry record) races
        # neither the DETACH response nor this test's own monitor() call.
        task = taz_client.task.detach(
            taz_client.command.exec,
            PY,
            args=_py("import sys, time; time.sleep(1); sys.exit(7)"),
        )
        m = taz_client.process.monitor(task.pid, _MONITOR_INTERVAL)
        final = _wait_final(m, 5.0)
        assert final.exited is True
        assert final.exit_code_known is True
        assert final.exit_code == 7
        taz_client.ping()

    def test_monitor_false_detached_task_exit_code_is_authoritative(
        self, taz_client: TazClient
    ) -> None:
        # The task itself is freed as soon as it finishes; its registry
        # entry is not.
        task = taz_client.task.detach(
            taz_client.command.exec,
            PY,
            args=_py("import sys, time; time.sleep(1); sys.exit(7)"),
            monitor=False,
        )
        m = taz_client.process.monitor(task.pid, _MONITOR_INTERVAL)
        final = _wait_final(m, 5.0)
        assert final.exit_code_known is True
        assert final.exit_code == 7

    def test_cancelled_detached_task_reports_its_kill_not_exit_zero(
        self, taz_client: TazClient
    ) -> None:
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import time; time.sleep(30)")
        )
        m = taz_client.process.monitor(task.pid, _MONITOR_INTERVAL)
        assert next(m).exited is False
        assert taz_client.task.cancel(task.task_id) is True
        final = _wait_final(m, 5.0)
        assert final.exited is True
        assert final.exit_code_known is True
        # -signal on POSIX, as COMMAND_EXEC reports it; TerminateJobObject's
        # exit code on Windows.
        if sys.platform == "win32":
            assert final.exit_code == 1
        else:
            assert final.exit_code == -signal.SIGKILL

    def test_cancel_via_break_leaves_no_buffered_or_closed_state(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        with taz_client.process.monitor(pid, _MONITOR_INTERVAL) as m:
            for i, _update in enumerate(m):
                if i == 1:
                    break
        assert m.cancelled is True

        time.sleep(3 * _MONITOR_INTERVAL / 1000)
        for _ in range(5):
            taz_client.ping()
        assert m.stream_id not in taz_client._dispatcher._buffer
        assert m.stream_id not in taz_client._dispatcher._closed
        taz_client.version()
        assert taz_client.cancel(m.stream_id) is False

    def test_nonexistent_pid_raises_not_found_and_connection_stays_usable(
        self, taz_client: TazClient
    ) -> None:
        pid = _unused_pid()
        with pytest.raises(TazError) as exc_info:
            next(iter(taz_client.process.monitor(pid, _MONITOR_INTERVAL)))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        taz_client.ping()

        with (
            pytest.raises(TazError) as exc_info,
            taz_client.process.monitor(pid, _MONITOR_INTERVAL) as m,
        ):
            next(m)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        taz_client.ping()

    def test_pid_zero_raises_invalid_request(self, taz_client: TazClient) -> None:
        with pytest.raises(TazError) as exc_info:
            next(iter(taz_client.process.monitor(0, _MONITOR_INTERVAL)))
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        taz_client.ping()

    def test_interleaved_with_other_calls_no_deadlock(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
        tmp_path: Path,
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        scratch = tmp_path / "scratch.txt"
        scratch.write_bytes(b"x")

        start = time.monotonic()
        m = taz_client.process.monitor(pid, _MONITOR_INTERVAL)
        next(m)
        taz_client.file.stat(str(scratch))
        next(m)
        taz_client.process.info(pid)
        next(m)
        assert time.monotonic() - start < 10
        m.close()

    def test_late_frames_are_not_buffered_after_close(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        m = taz_client.process.monitor(pid, _MONITOR_INTERVAL)
        next(m)
        m.close()

        for _ in range(5):
            taz_client.ping()
        assert m.stream_id not in taz_client._dispatcher._buffer
        taz_client.version()

    @pytest.mark.filterwarnings("error::ResourceWarning")
    def test_abandoning_without_close_warns_and_connection_stays_usable(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        it = taz_client.process.monitor(pid, _MONITOR_INTERVAL)
        next(it)
        with pytest.warns(ResourceWarning):
            del it
            gc.collect()

        taz_client.ping()
        taz_client.version()

    @pytest.mark.valgrind
    def test_demo_monitor_cpu_and_memory(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
        capsys: pytest.CaptureFixture[str],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        with taz_client.process.monitor(pid, interval_ms=1000) as updates:
            for update in updates:
                print(
                    f"CPU: {update.info.cpu_percent}%, Mem: {update.info.memory_bytes}"
                )
                if update.info.memory_bytes > 0:
                    break
        out = capsys.readouterr().out
        assert len(out.splitlines()) == 1
        assert updates.cancelled is True

    def test_closing_socket_with_open_monitor_stays_usable(
        self,
        daemon: Daemon,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)

        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        m = victim.process.monitor(pid, _MONITOR_INTERVAL)
        next(m)
        # Abrupt teardown: close the socket with the stream still open,
        # mirroring TestProcessOps's 32-request mid-enumeration close test.
        victim.close()

        with TazClient("127.0.0.1", daemon.port) as other:
            start = time.monotonic()
            other.version()
            assert time.monotonic() - start < 2.0
            with other.process.monitor(pid, _MONITOR_INTERVAL) as again:
                next(again)

    def test_closing_socket_mid_sample_eight_victims_exit_audit_passes(
        self,
        daemon: Daemon,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        sleeps_ms = (0, 25, 50)

        for i in range(8):
            victim = TazClient("127.0.0.1", daemon.port)
            victim.connect()
            # Kept until the client closes: dropped any earlier, the open
            # stream would be abandoned.
            m = victim.process.monitor(pid, _MONITOR_INTERVAL)
            time.sleep(sleeps_ms[i % len(sleeps_ms)] / 1000)
            victim.close()
            del m
        # Nothing further to assert here: the daemon fixture's teardown
        # audits the exit for sanitizer reports and a non-zero code - the
        # ASan/TSan oracle for a socket close racing a sample in flight.

    def test_sigterm_with_idle_monitor_raises_connection_lost_promptly(
        self,
        taz_client: TazClient,
        daemon: Daemon,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        m = taz_client.process.monitor(pid, _MONITOR_INTERVAL)
        next(m)

        t0 = time.monotonic()
        daemon.stop()
        # The fixture's teardown re-stops (idempotent) and audits the exit
        # for sanitizer reports and a non-zero code.
        assert time.monotonic() - t0 < 3.0

        with pytest.raises(TazConnectionLost):
            next(m)

    def test_sigterm_from_thread_while_next_is_blocked_raises_connection_lost(
        self,
        taz_client: TazClient,
        daemon: Daemon,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        # A slow interval so the second next() call is genuinely blocked
        # waiting on the socket for a frame that is not due for seconds,
        # instead of racing the daemon's own 100 ms tick.
        m = taz_client.process.monitor(pid, 10_000)
        next(m)

        def stop_soon() -> None:
            time.sleep(0.2)
            daemon.stop()

        t = threading.Thread(target=stop_soon)
        t.start()
        with pytest.raises(TazConnectionLost):
            next(m)
        t.join(timeout=10)
        assert not t.is_alive()

    def test_cancel_storm_while_sample_may_be_in_flight(
        self,
        taz_client: TazClient,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        _, pid = spawn_helper(_SLEEP_SNIPPET)
        for _ in range(10):
            m = taz_client.process.monitor(pid, _MONITOR_INTERVAL)
            next(m)
            m.close()
            assert m.cancelled is True
            taz_client.ping()


class TestDetach:
    """``DETACH`` through the typed ``client.task.detach()`` API."""

    def test_short_sleep_returns_task_id_and_pid_promptly(
        self, taz_client: TazClient
    ) -> None:
        sleep_s = 2
        start = time.monotonic()
        task = taz_client.task.detach(
            taz_client.command.exec,
            PY,
            args=_py(f"import time; time.sleep({sleep_s})"),
        )
        # Nonzero pid and a prompt reply: the DETACH response is sent as
        # soon as the daemon has spawned the process, well before the
        # process itself has finished sleeping.
        assert task.task_id != 0
        assert task.pid > 0
        assert time.monotonic() - start < 1.0

        assert taz_client.task.cancel(task.task_id) is True
        deadline = time.monotonic() + 5
        while not _process_gone(task.pid) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert _process_gone(task.pid)
        taz_client.ping()

    def test_file_put_raw_detach_request_is_not_supported(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        req = advanced_pb2.DetachRequest(
            file_put=file_pb2.FilePutRequest(
                dest=str(tmp_path / "dest.bin"), size=0, permissions=0o644
            )
        )
        with pytest.raises(TazError) as exc_info:
            taz_client._call(
                common_pb2.OPCODE_DETACH,
                req.SerializeToString(),
                advanced_pb2.DetachResponse,
                None,
            )
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        assert not (tmp_path / "dest.bin").exists()
        taz_client.ping()

    def test_capabilities_advertise_detach(self, taz_client: TazClient) -> None:
        ops = set(taz_client.capabilities().operations)
        assert common_pb2.OPCODE_DETACH in ops


class TestTaskStatus:
    """``TASK_STATUS`` through the typed ``client.task.status()`` API."""

    def test_poll_reports_running_then_completed_with_decoded_result(
        self, taz_client: TazClient
    ) -> None:
        sleep_s = 2
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py(f"import time; time.sleep({sleep_s})")
        )
        running = taz_client.task.status(task.task_id)
        assert running.state == TaskState.RUNNING
        assert running.result is None
        assert running.error is None

        deadline = time.monotonic() + sleep_s + 5
        status = running
        while status.state == TaskState.RUNNING:
            assert time.monotonic() < deadline, "task never left RUNNING"
            time.sleep(0.05)
            status = taz_client.task.status(task.task_id)

        assert status.state == TaskState.COMPLETED
        assert status.error is None
        assert status.result is not None
        assert status.result.exit_code == 0
        taz_client.ping()

    def test_subscribe_streams_updates_until_completed_with_no_result_on_final_frame(
        self, taz_client: TazClient
    ) -> None:
        sleep_s = 1
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py(f"import time; time.sleep({sleep_s})")
        )
        m = taz_client.task.status(
            task.task_id, subscribe=True, interval_ms=_MONITOR_INTERVAL
        )
        assert isinstance(m, StreamIterator)

        first = next(m)
        assert first.state == TaskState.RUNNING
        # Streamed frames never carry `result`, even once the task is done -
        # only a non-subscribed poll does.
        assert first.result is None

        deadline = time.monotonic() + sleep_s + 5
        final: TaskStatus | None = None
        while not m.ended:
            assert time.monotonic() < deadline, "stream never reached its final frame"
            final = next(m)
        assert final is not None
        assert final.state == TaskState.COMPLETED
        assert final.result is None
        with pytest.raises(StopIteration):
            next(m)

        # The final frame ended the stream without a result: a separate
        # non-subscribed poll is how a caller actually retrieves it.
        status = taz_client.task.status(task.task_id)
        assert status.state == TaskState.COMPLETED
        assert status.result is not None
        assert status.result.exit_code == 0
        taz_client.ping()

    def test_subscription_on_monitor_false_task_ends_with_terminal_frame(
        self, taz_client: TazClient
    ) -> None:
        # A monitor=false task is freed when it finishes, but only after an
        # open subscription has had its final frame, which goes out as soon
        # as the task finishes rather than at the next 60 s interval.
        task = taz_client.task.detach(
            taz_client.command.exec,
            PY,
            args=_py("import time; time.sleep(2)"),
            monitor=False,
        )
        m = taz_client.task.status(
            task.task_id, subscribe=True, interval_ms=60_000, max_wait=15
        )
        states = [update.state for update in m]
        assert states[-1] == TaskState.COMPLETED
        assert m.ended
        with pytest.raises(TazError) as exc_info:
            taz_client.task.status(task.task_id)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_cancelling_a_subscription_ends_it_with_a_final_frame(
        self, taz_client: TazClient
    ) -> None:
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import time; time.sleep(30)")
        )
        with taz_client.task.status(
            task.task_id, subscribe=True, interval_ms=_MONITOR_INTERVAL
        ) as m:
            assert next(m).state == TaskState.RUNNING
        assert m.cancelled is True

        # The daemon's final frame on the cancelled stream lets the client
        # forget the stream instead of tracking it as closed forever.
        for _ in range(5):
            taz_client.ping()
        assert m.stream_id not in taz_client._dispatcher._closed
        assert m.stream_id not in taz_client._dispatcher._buffer
        assert taz_client.task.cancel(task.task_id) is True

    def test_failed_task_status_carries_error(self, taz_client: TazClient) -> None:
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import sys; sys.exit(3)")
        )
        deadline = time.monotonic() + 5
        status = taz_client.task.status(task.task_id)
        while status.state == TaskState.RUNNING:
            assert time.monotonic() < deadline, "task never left RUNNING"
            time.sleep(0.05)
            status = taz_client.task.status(task.task_id)

        assert status.state == TaskState.FAILED
        assert status.error is not None
        assert status.error.code == common_pb2.ERROR_CODE_INTERNAL
        assert status.error.message == "exit code 3"
        assert status.result is not None
        assert status.result.exit_code == 3
        taz_client.ping()

    def test_capabilities_advertise_task_status(self, taz_client: TazClient) -> None:
        ops = set(taz_client.capabilities().operations)
        assert common_pb2.OPCODE_TASK_STATUS in ops


class TestTaskCancel:
    """``TASK_CANCEL`` through the typed ``client.task.cancel()`` API."""

    def test_cancel_running_detached_sleep_returns_accepted_true(
        self, taz_client: TazClient
    ) -> None:
        sleep_s = 10
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py(f"import time; time.sleep({sleep_s})")
        )
        pid = task.pid

        accepted = taz_client.task.cancel(task.task_id)
        assert accepted is True

        deadline = time.monotonic() + 5
        status = taz_client.task.status(task.task_id)
        while status.state == TaskState.RUNNING:
            assert time.monotonic() < deadline, "task never left RUNNING"
            time.sleep(0.05)
            status = taz_client.task.status(task.task_id)

        assert status.state == TaskState.CANCELLED
        # Process should be gone shortly after cancellation.
        deadline = time.monotonic() + 5
        while psutil.pid_exists(pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        assert not psutil.pid_exists(pid), f"pid {pid} still exists"
        taz_client.ping()

    def test_cancel_signal_zero_kills_process_tree(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        grandchild_pid_file = tmp_path / "grandchild.pid"
        # repr() (not a hand-rolled quoted/raw string) escapes the path
        # safely for embedding as a Python literal one level down - needed
        # on Windows, where tmp_path contains backslashes (e.g. "\Users")
        # that a careless embedding mangles into bogus \U.. escapes.
        grandchild_code = (
            "import os, pathlib, time; "
            f"pathlib.Path({str(grandchild_pid_file)!r}).write_text(str(os.getpid())); "
            "time.sleep(60)"
        )
        snippet = (
            "import subprocess, sys, time; "
            f"child = subprocess.Popen([sys.executable, '-c', {grandchild_code!r}]); "
            "print(child.pid); sys.stdout.flush(); "
            "time.sleep(60)"
        )
        task = taz_client.task.detach(taz_client.command.exec, PY, args=_py(snippet))
        parent_pid = task.pid

        deadline = time.monotonic() + 5
        while not grandchild_pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert grandchild_pid_file.exists(), "grandchild never started"
        grandchild_pid = int(grandchild_pid_file.read_text())

        accepted = taz_client.task.cancel(task.task_id, signal=0)
        assert accepted is True

        # Both parent and grandchild should be gone.
        deadline = time.monotonic() + 5
        while (
            psutil.pid_exists(parent_pid) or psutil.pid_exists(grandchild_pid)
        ) and time.monotonic() < deadline:
            time.sleep(0.1)
        assert not psutil.pid_exists(parent_pid), (
            f"parent pid {parent_pid} still exists"
        )
        assert not psutil.pid_exists(grandchild_pid), (
            f"grandchild pid {grandchild_pid} still exists"
        )
        taz_client.ping()

    def test_cancel_with_nonzero_signal_kills_task(self, taz_client: TazClient) -> None:
        sleep_s = 10
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py(f"import time; time.sleep({sleep_s})")
        )

        # Use SIGTERM (signal 15) to request termination.
        accepted = taz_client.task.cancel(task.task_id, signal=15)
        assert accepted is True

        deadline = time.monotonic() + 5
        status = taz_client.task.status(task.task_id)
        while status.state == TaskState.RUNNING:
            assert time.monotonic() < deadline, "task never left RUNNING"
            time.sleep(0.05)
            status = taz_client.task.status(task.task_id)

        assert status.state == TaskState.CANCELLED
        taz_client.ping()

    def test_cancel_finished_task_returns_accepted_false(
        self, taz_client: TazClient
    ) -> None:
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import sys; sys.exit(0)")
        )

        deadline = time.monotonic() + 5
        status = taz_client.task.status(task.task_id)
        while status.state == TaskState.RUNNING:
            assert time.monotonic() < deadline, "task never left RUNNING"
            time.sleep(0.05)
            status = taz_client.task.status(task.task_id)

        assert status.state == TaskState.COMPLETED

        accepted = taz_client.task.cancel(task.task_id)
        assert accepted is False
        taz_client.ping()

    def test_cancel_with_invalid_signal_raises_invalid_request(
        self, taz_client: TazClient
    ) -> None:
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import time; time.sleep(10)")
        )

        # Use an obviously invalid signal number (far beyond typical range).
        with pytest.raises(TazError) as exc_info:
            taz_client.task.cancel(task.task_id, signal=999)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST

        # Clean up: cancel the task properly to avoid leaving it running.
        taz_client.task.cancel(task.task_id, signal=0)
        taz_client.ping()

    def test_cancel_unknown_task_raises_not_found(self, taz_client: TazClient) -> None:
        with pytest.raises(TazError) as exc_info:
            taz_client.task.cancel(999999)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_capabilities_advertise_task_cancel(self, taz_client: TazClient) -> None:
        ops = set(taz_client.capabilities().operations)
        assert common_pb2.OPCODE_TASK_CANCEL in ops


class TestPriority:
    """A CRITICAL-priority detached task's FAILED escalation (protocol.md
    §10.2) delivered out of band on the ``DETACH`` stream."""

    @pytest.mark.valgrind
    def test_raise_policy_interrupts_discrete_call_and_connection_stays_usable(
        self, taz_client: TazClient
    ) -> None:
        task = taz_client.task.detach(
            taz_client.command.exec,
            PY,
            args=_py("import sys; sys.exit(1)"),
            error_priority="critical",
        )

        with pytest.raises(TazPriorityError) as exc_info:
            taz_client.command.exec(PY, args=_py("import time; time.sleep(2)"))

        err = exc_info.value
        assert err.task_id == task.task_id
        assert err.error.code == common_pb2.ERROR_CODE_INTERNAL
        assert err.error.message == "exit code 1"

        # The connection stays usable, and the interrupted 2 s exec's late
        # RESPONSE, which arrives while this 3 s call is in flight, is
        # discarded rather than buffered or delivered here.
        taz_client.ping()
        result = taz_client.command.exec(
            PY, args=_py("import sys, time; time.sleep(3); sys.exit(42)")
        )
        assert result.exit_code == 42
        assert not taz_client._dispatcher._buffer
        assert not taz_client._dispatcher._closed

    def test_queue_policy_appends_to_priority_errors_without_raising(
        self, daemon: Daemon
    ) -> None:
        with TazClient("127.0.0.1", daemon.port, priority_policy="queue") as client:
            task = client.task.detach(
                client.command.exec,
                PY,
                args=_py("import sys; sys.exit(1)"),
                error_priority="critical",
            )

            result = client.command.exec(PY, args=_py("import time; time.sleep(2)"))
            assert result.exit_code == 0

            assert len(client.priority_errors) == 1
            err = client.priority_errors[0]
            assert err.task_id == task.task_id
            assert err.error.code == common_pb2.ERROR_CODE_INTERNAL
            assert err.error.message == "exit code 1"
            client.ping()


class TestTaskLifetime:
    """A detached task's process tree is scoped to the connection that owns
    it, running tasks don't starve other requests on the same connection,
    and the plan's demo snippet works end to end."""

    @pytest.mark.valgrind
    def test_sigterm_before_any_client_connects_exits_cleanly(
        self, daemon: Daemon
    ) -> None:
        # The SIGTERM tree kill used to mistake the listening socket for a
        # connection until the first client connected, and crash.
        daemon.stop()

    @pytest.mark.valgrind
    def test_sigterm_with_infly_exec_stops_gracefully(
        self, taz_client: TazClient, daemon: Daemon, tmp_path: Path
    ) -> None:
        # SIGTERM with an in-flight 30s exec terminates the child and the
        # daemon within 2s * timing_scale. The child pid is
        # captured before stop(), and we verify it's gone after.
        pid_file = tmp_path / "pid.txt"
        snippet = (
            "import os, pathlib, time; "
            f"pathlib.Path(r'{pid_file}').write_text(str(os.getpid())); "
            "time.sleep(30)"
        )
        req = command_pb2.CommandExecRequest(command=PY, args=_py(snippet))

        # Send the COMMAND_EXEC and wait for the child to start
        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        victim._conn.send_request(
            common_pb2.OPCODE_COMMAND_EXEC, req.SerializeToString()
        )

        deadline = time.monotonic() + 10
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert pid_file.exists(), "child never started"
        child_pid = int(pid_file.read_text())
        assert psutil.pid_exists(child_pid)

        # Measure time from SIGTERM to stop completion
        start = time.monotonic()
        daemon.stop()
        elapsed = time.monotonic() - start

        # Must complete within 2s * timing_scale
        max_elapsed = 2.0 * daemon.timing_scale
        assert elapsed < max_elapsed, (
            f"daemon.stop() took {elapsed:.2f}s, expected < {max_elapsed:.2f}s"
        )

        # Child must be gone
        assert _wait_process_gone(child_pid), f"child pid {child_pid} still exists"
        victim.close()

    @pytest.mark.valgrind
    def test_sigterm_with_detached_task_stops_gracefully(
        self, taz_client: TazClient, daemon: Daemon
    ) -> None:
        # SIGTERM with a detached 30s task terminates the child and the
        # daemon within 2s * timing_scale.
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import time; time.sleep(30)")
        )
        child_pid = task.pid
        assert psutil.pid_exists(child_pid)

        # Measure time from SIGTERM to stop completion
        start = time.monotonic()
        daemon.stop()
        elapsed = time.monotonic() - start

        # Must complete within 2s * timing_scale
        max_elapsed = 2.0 * daemon.timing_scale
        assert elapsed < max_elapsed, (
            f"daemon.stop() took {elapsed:.2f}s, expected < {max_elapsed:.2f}s"
        )

        # Child must be gone
        assert _wait_process_gone(child_pid), f"child pid {child_pid} still exists"

    @pytest.mark.skipif(
        sys.platform == "win32",
        reason="SIGINT is POSIX-only; Windows exercises the same walk via"
        " CTRL_BREAK_EVENT in the other SIGTERM-equivalent rows",
    )
    def test_sigint_with_detached_task_stops_gracefully(
        self, taz_client: TazClient, daemon: Daemon
    ) -> None:
        # SIGINT takes the same shutdown walk as SIGTERM: a detached
        # 30s task's child and the daemon both stop within 2s * timing_scale.
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import time; time.sleep(30)")
        )
        child_pid = task.pid
        assert psutil.pid_exists(child_pid)

        start = time.monotonic()
        os.kill(daemon.pid, signal.SIGINT)
        daemon.proc.wait(timeout=10)
        elapsed = time.monotonic() - start

        max_elapsed = 2.0 * daemon.timing_scale
        assert elapsed < max_elapsed, (
            f"daemon took {elapsed:.2f}s to stop after SIGINT, expected"
            f" < {max_elapsed:.2f}s"
        )
        assert _wait_process_gone(child_pid), f"child pid {child_pid} still exists"

    @pytest.mark.skipif(
        sys.platform == "win32",
        reason="a repeat POSIX signal mid-drain; on Windows the same walk"
        " keeps its console-control handler installed until the loop has"
        " drained, and a second CTRL_BREAK cannot be aimed at a single"
        " process from here",
    )
    def test_repeat_signal_during_shutdown_is_a_no_op(
        self, daemon: Daemon, tmp_path: Path
    ) -> None:
        # A peer that never reads holds the walk's drain open until its 1 s
        # deadline; a second signal inside that window must reach the
        # daemon's own (no-op) handler, not the default action, which
        # would kill it with the walk half done.
        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        _flood_unread_file_gets(victim, tmp_path)

        os.kill(daemon.pid, signal.SIGTERM)
        time.sleep(0.3)
        assert daemon.proc.poll() is None, "the drain ended before the repeat"
        os.kill(daemon.pid, signal.SIGINT)
        daemon.proc.wait(timeout=10 * daemon.timing_scale)
        assert daemon.proc.returncode == 0
        victim.close()

    def test_sigterm_with_an_unread_peer_force_closes_at_the_drain_deadline(
        self, daemon: Daemon, tmp_path: Path
    ) -> None:
        # A peer that never reads keeps the daemon's writes to it queued, so
        # the walk's graceful uv_shutdown cannot complete: the connection
        # is force-closed at the 1 s drain deadline and the exit is still 0.
        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        _flood_unread_file_gets(victim, tmp_path)

        start = time.monotonic()
        daemon.stop()
        elapsed = time.monotonic() - start

        assert elapsed >= 0.8, (
            f"stop took {elapsed:.2f}s: the drain never waited, so the"
            " deadline path did not run"
        )
        max_elapsed = 5.0 * daemon.timing_scale
        assert elapsed < max_elapsed, (
            f"daemon.stop() with an unread peer took {elapsed:.2f}s, "
            f"expected < {max_elapsed:.2f}s"
        )
        victim.close()

    def test_sigterm_with_monitor_and_status_streams_closes_cleanly(
        self,
        daemon: Daemon,
        spawn_helper: Callable[..., tuple[subprocess.Popen[bytes], int]],
    ) -> None:
        # Open a PROCESS_MONITOR stream and a subscribed TASK_STATUS stream.
        # SIGTERM closes the daemon and all open streams cleanly (without crashes).
        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()

        # Start a process to monitor
        _, pid = spawn_helper(_SLEEP_SNIPPET)

        # Open monitor stream and status stream (don't wait for updates)
        monitor = victim.process.monitor(pid, _MONITOR_INTERVAL)
        task = victim.task.detach(
            victim.command.exec, PY, args=_py("import time; time.sleep(30)")
        )
        status_stream = victim.task.status(
            task.task_id, subscribe=True, interval_ms=_MONITOR_INTERVAL
        )

        # Both streams share one TazClient connection/socket, so a single
        # reader must multiplex them: the dispatcher buffers frames for
        # whichever stream isn't the one currently awaited (see
        # Dispatcher._next_frame), which only holds if one thread at a time
        # calls recv_response() on this connection. Two threads each
        # blocked in their own next() would race Connection.recv_frame()
        # on the same socket and could corrupt frames.
        errors: list[BaseException] = []
        lost: list[str] = []
        # Local completion flags rather than `.ended`, so a stream that
        # fails in some unexpected way is still read only once more.
        monitor_done = False
        status_done = False

        def _drain_one(stream: StreamIterator[Any]) -> bool:
            # A caught exception here must not stop read_both() from still
            # draining the other stream. Returns True once this stream is
            # done (cleanly ended or failed) and should not be read again.
            try:
                next(stream)
            except TazConnectionLost:
                # The only way either stream may end: the daemon closed the
                # connection under it.
                lost.append(type(stream).__name__)
                return True
            except BaseException as e:
                errors.append(e)
                return True
            return False

        def read_both() -> None:
            nonlocal monitor_done, status_done
            while not monitor_done or not status_done:
                if not monitor_done:
                    monitor_done = _drain_one(monitor)
                if not status_done:
                    status_done = _drain_one(status_stream)

        reader = threading.Thread(target=read_both)
        reader.start()

        # Let the reader start reading
        time.sleep(0.1)

        # SIGTERM closes the daemon and both streams
        daemon.stop()

        # Wait for the reader to finish
        reader.join(timeout=5)
        assert not reader.is_alive(), "reader thread hung"
        assert not errors, f"unexpected errors: {errors}"
        assert len(lost) == 2
        victim.close()

    def test_interleaved_synchronous_calls_succeed_while_a_task_runs(
        self, taz_client: TazClient
    ) -> None:
        sleep_s = 3
        task = taz_client.task.detach(
            taz_client.command.exec,
            PY,
            args=_py(f"import time; time.sleep({sleep_s})"),
        )
        for i in range(5):
            result = taz_client.command.exec(PY, args=_py(f"import sys; sys.exit({i})"))
            assert result.exit_code == i

        deadline = time.monotonic() + sleep_s + 5
        status = taz_client.task.status(task.task_id)
        while status.state == TaskState.RUNNING:
            assert time.monotonic() < deadline, "task never left RUNNING"
            time.sleep(0.05)
            status = taz_client.task.status(task.task_id)
        assert status.state == TaskState.COMPLETED
        taz_client.ping()

    def test_closing_connection_kills_detached_task_tree(self, daemon: Daemon) -> None:
        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        task = victim.task.detach(
            victim.command.exec, PY, args=_py("import time; time.sleep(60)")
        )
        pid = task.pid
        assert psutil.pid_exists(pid)

        # Abrupt teardown: close the socket with the task still running.
        victim.close()

        with TazClient("127.0.0.1", daemon.port) as other:
            start = time.monotonic()
            other.version()
            assert time.monotonic() - start < 2.0

            # conn_close kills every task's tree; a wide margin, not a tight
            # bound, per the lifetime hazard notes.
            deadline = time.monotonic() + 2.0
            while not _process_gone(pid) and time.monotonic() < deadline:
                time.sleep(0.05)
            assert _process_gone(pid), f"pid {pid} still exists"

            # The daemon itself agrees, from a second connection, that the
            # pid is gone - not just the OS-level check above.
            with pytest.raises(TazError) as exc_info:
                other.process.info(pid)
            assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_concurrency_ten_detached_sleeps_do_not_starve_other_requests(
        self, taz_client: TazClient, tmp_path: Path
    ) -> None:
        sleep_s = 3
        tasks = [
            taz_client.task.detach(
                taz_client.command.exec,
                PY,
                args=_py(f"import time; time.sleep({sleep_s})"),
            )
            for _ in range(10)
        ]

        start = time.monotonic()
        taz_client.file.stat(str(tmp_path))
        assert time.monotonic() - start < 2.0

        deadline = time.monotonic() + sleep_s + 5
        for task in tasks:
            status = taz_client.task.status(task.task_id)
            while status.state == TaskState.RUNNING:
                assert time.monotonic() < deadline, "task never left RUNNING"
                time.sleep(0.05)
                status = taz_client.task.status(task.task_id)
            assert status.state == TaskState.COMPLETED
        taz_client.ping()

    @pytest.mark.valgrind
    def test_demo_detach_status_cancel(self, taz_client: TazClient) -> None:
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import time; time.sleep(10)")
        )
        print(f"Task {task.task_id} running (PID {task.pid})")
        assert task.task_id != 0
        assert task.pid > 0
        assert taz_client.task.status(task.task_id).state == TaskState.RUNNING

        accepted = taz_client.task.cancel(task.task_id)
        assert accepted is True

        deadline = time.monotonic() + 5
        status = taz_client.task.status(task.task_id)
        while status.state == TaskState.RUNNING:
            assert time.monotonic() < deadline, "task never left RUNNING"
            time.sleep(0.05)
            status = taz_client.task.status(task.task_id)
        assert status.state == TaskState.CANCELLED
        taz_client.ping()


class TestRestart:
    """RESTART (0x0005) through ``client.restart()``: the daemon runs its
    graceful shutdown walk, then re-execs on the same port."""

    def test_capabilities_advertise_restart(self, taz_client: TazClient) -> None:
        ops = set(taz_client.capabilities().operations)
        assert common_pb2.OPCODE_RESTART in ops

    @pytest.mark.valgrind
    def test_restart_500ms_reconnects_on_same_port_with_matching_version(
        self, daemon: Daemon
    ) -> None:
        # effective_delay_ms echoes the request, the client
        # closes on the daemon's own EOF, and a fresh connection on the
        # same port sees the same version once the successor is up.
        client = TazClient("127.0.0.1", daemon.port)
        client.connect()
        effective = client.restart(delay_ms=500)
        assert effective == 500
        assert client.closed

        daemon.await_restart(timeout=10 * daemon.timing_scale)

        with TazClient("127.0.0.1", daemon.port) as reconnected:
            assert reconnected.version().version == _project_version()

    def test_restart_delay_zero_is_clamped_to_the_minimum(self, daemon: Daemon) -> None:
        with TazClient("127.0.0.1", daemon.port) as client:
            effective = client.restart(delay_ms=0)
        assert effective == 100
        daemon.await_restart(timeout=10 * daemon.timing_scale)

    @pytest.mark.valgrind
    def test_restart_terminates_a_detached_task_and_drops_its_state(
        self, taz_client: TazClient, daemon: Daemon
    ) -> None:
        # A detached 60s task's child is gone and its state
        # does not survive the restart (NOT_FOUND from a fresh connection).
        task = taz_client.task.detach(
            taz_client.command.exec, PY, args=_py("import time; time.sleep(60)")
        )
        child_pid = task.pid
        assert psutil.pid_exists(child_pid)

        effective = taz_client.restart(delay_ms=500)
        assert effective == 500

        daemon.await_restart(timeout=10 * daemon.timing_scale)
        assert _wait_process_gone(child_pid), f"child pid {child_pid} still exists"

        with TazClient("127.0.0.1", daemon.port) as reconnected:
            with pytest.raises(TazError) as exc_info:
                reconnected.task.status(task.task_id)
            assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_restart_twice_then_sigterm_every_exit_is_clean(
        self, daemon: Daemon
    ) -> None:
        # execv inherits the signal mask and dispositions: a
        # twice-restarted daemon must still stop cleanly on SIGTERM.
        with TazClient("127.0.0.1", daemon.port) as client:
            client.restart(delay_ms=100)
        daemon.await_restart(timeout=10 * daemon.timing_scale)

        with TazClient("127.0.0.1", daemon.port) as client:
            client.restart(delay_ms=100)
        daemon.await_restart(timeout=10 * daemon.timing_scale)

        daemon.stop()

    def test_restart_closes_another_connections_inflight_exec(
        self, daemon: Daemon, tmp_path: Path
    ) -> None:
        # RESTART's shutdown walk kills every other connection's exec tree
        # too, same as a plain SIGTERM (TestTaskLifetime's rows).
        pid_file = tmp_path / "pid.txt"
        snippet = (
            "import os, pathlib, time; "
            f"pathlib.Path(r'{pid_file}').write_text(str(os.getpid())); "
            "time.sleep(30)"
        )
        req = command_pb2.CommandExecRequest(command=PY, args=_py(snippet))

        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        victim._conn.send_request(
            common_pb2.OPCODE_COMMAND_EXEC, req.SerializeToString()
        )
        deadline = time.monotonic() + 10
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert pid_file.exists(), "child never started"
        child_pid = int(pid_file.read_text())
        assert psutil.pid_exists(child_pid)

        restarter = TazClient("127.0.0.1", daemon.port)
        restarter.connect()
        effective = restarter.restart(delay_ms=200)
        assert effective == 200

        lost_deadline = time.monotonic() + 5 * daemon.timing_scale
        lost = False
        while time.monotonic() < lost_deadline:
            try:
                victim.ping()
            except TazConnectionLost:
                lost = True
                break
            time.sleep(0.05)
        assert lost, "victim connection survived the restart's shutdown walk"
        victim.close()

        daemon.await_restart(timeout=10 * daemon.timing_scale)
        assert _wait_process_gone(child_pid), f"child pid {child_pid} still exists"

    def test_restart_survives_the_requester_disconnecting(self, daemon: Daemon) -> None:
        # An acknowledged restart belongs to the daemon: the client closing
        # its connection right after the response does not cancel it.
        client = TazClient("127.0.0.1", daemon.port)
        client.connect()
        assert _send_restart_and_read_ack(client, 300) == 300
        client.close()

        daemon.await_restart(timeout=10 * daemon.timing_scale)
        with TazClient("127.0.0.1", daemon.port) as reconnected:
            assert reconnected.version().version == _project_version()

    def test_sigterm_during_the_restart_delay_stops_instead(
        self, daemon: Daemon
    ) -> None:
        # A STOP that arrives while an acknowledged restart is still waiting
        # out its delay wins: the pending restart is dropped and the daemon
        # exits 0 at once rather than after the delay.
        with TazClient("127.0.0.1", daemon.port) as client:
            assert _send_restart_and_read_ack(client, 60000) == 60000

            start = time.monotonic()
            daemon.stop()
            elapsed = time.monotonic() - start

        max_elapsed = 5.0 * daemon.timing_scale
        assert elapsed < max_elapsed, (
            f"stop during a 60 s restart delay took {elapsed:.2f}s,"
            f" expected < {max_elapsed:.2f}s"
        )

    def test_stop_while_a_restart_drains_stops_for_good(
        self, daemon: Daemon, tmp_path: Path
    ) -> None:
        # A peer that never reads holds the restart's own walk open until
        # its 1 s drain deadline; a stop signal inside that window must
        # make the daemon exit rather than come back.
        victim = TazClient("127.0.0.1", daemon.port)
        victim.connect()
        _flood_unread_file_gets(victim, tmp_path)
        with TazClient("127.0.0.1", daemon.port) as restarter:
            assert _send_restart_and_read_ack(restarter, 100) == 100
            time.sleep(0.4)
            assert daemon.proc.poll() is None
            daemon.stop()
        assert daemon.proc.returncode == 0
        if sys.platform == "win32":
            # A successor would be a new process; stop() alone cannot tell.
            assert not daemon._adopt_windows_successor(timeout=2.0), (
                "the daemon restarted despite the stop"
            )
        victim.close()

    @pytest.mark.skipif(
        sys.platform != "linux",
        reason="replacing a running executable on disk (and /proc/self/exe"
        " naming it '<path> (deleted)') is Linux behaviour",
    )
    def test_restart_after_the_binary_is_replaced_on_disk(
        self, daemon_binary: Path, tmp_path: Path
    ) -> None:
        # An install or upgrade renames a new binary over the running one;
        # a restart must then run what is at the path now.
        exe = tmp_path / "tazd"
        shutil.copy2(daemon_binary, exe)
        d = _launch(exe)
        try:
            replacement = tmp_path / "tazd.new"
            shutil.copy2(daemon_binary, replacement)
            os.replace(replacement, exe)

            with TazClient("127.0.0.1", d.port) as client:
                client.restart(delay_ms=100)
            d.await_restart(timeout=10 * d.timing_scale)

            with TazClient("127.0.0.1", d.port) as reconnected:
                assert reconnected.version().version == _project_version()
        finally:
            d.stop()
        _check_exit(d)

    def test_demo_restart_then_reconnect(self, daemon: Daemon) -> None:
        # The documented flow: restart(delay_ms=1000), sleep, reconnect, version().
        with TazClient("127.0.0.1", daemon.port) as client:
            effective = client.restart(delay_ms=1000)
            print(f"restarting in {effective}ms")
        time.sleep(effective / 1000 + 0.5)
        daemon.await_restart(timeout=10 * daemon.timing_scale)
        with TazClient("127.0.0.1", daemon.port) as reconnected:
            assert reconnected.version().version == _project_version()


class TestConfigGet:
    @pytest.mark.valgrind
    def test_config_get_all_returns_defaults(self, taz_client: TazClient) -> None:
        cfg = taz_client.config_get()
        assert cfg == {
            "log.level": "INFO",
            "compression": "NONE",
            "exec.max_output_bytes": "1048576",
        }

    def test_config_get_subset_returns_only_requested_key(
        self, taz_client: TazClient
    ) -> None:
        cfg = taz_client.config_get(keys=["log.level"])
        assert list(cfg.keys()) == ["log.level"]
        assert cfg["log.level"] == "INFO"

    def test_config_get_unknown_key_omitted(self, taz_client: TazClient) -> None:
        cfg = taz_client.config_get(keys=["no.such.key"])
        assert cfg == {}


class TestConfigUpdate:
    @pytest.mark.valgrind
    def test_config_update_then_get_reflects_change(
        self, taz_client: TazClient
    ) -> None:
        result = taz_client.config_update({"log.level": "DEBUG"})
        assert "log.level" in result.applied
        assert len(result.rejected) == 0
        cfg = taz_client.config_get(keys=["log.level"])
        assert cfg["log.level"] == "DEBUG"

    def test_config_update_invalid_key_in_rejected(self, taz_client: TazClient) -> None:
        result = taz_client.config_update({"invalid.key": "x"})
        assert len(result.applied) == 0
        rejected_keys = [rk.key for rk in result.rejected]
        assert "invalid.key" in rejected_keys

    def test_config_update_bad_log_level_rejected(self, taz_client: TazClient) -> None:
        result = taz_client.config_update({"log.level": "TRACE"})
        assert len(result.applied) == 0
        assert any(rk.key == "log.level" for rk in result.rejected)

    def test_config_capabilities_advertised(self, taz_client: TazClient) -> None:
        cap = taz_client.capabilities()
        ops = set(cap.operations)
        assert common_pb2.OPCODE_CONFIGURATION_GET in ops
        assert common_pb2.OPCODE_CONFIGURATION_UPDATE in ops


class TestLog:
    """``LOG`` (0x0042) returns ring-buffered diagnostic entries."""

    @pytest.mark.valgrind
    def test_log_returns_entries_after_several_execs(
        self, taz_client: TazClient
    ) -> None:
        for i in range(3):
            result = taz_client.command.exec(PY, args=_py(f"print({i})"))
            assert result.exit_code == 0
        entries = taz_client.log(lines=10)
        assert len(entries) >= 3
        for entry in entries:
            assert entry.timestamp > 0
            assert entry.level
            assert entry.message
        assert sum(1 for e in entries if "executed command" in e.message) >= 3

    def test_log_level_filter_excludes_info_exec_entries(
        self, taz_client: TazClient
    ) -> None:
        taz_client.command.exec(PY, args=_py("print('hi')"))
        entries = taz_client.log(level="ERROR")
        assert not any("executed command" in e.message for e in entries)

    @pytest.mark.slow
    def test_log_wrap_does_not_deadlock_the_drained_stderr_fixture(
        self, taz_client: TazClient
    ) -> None:
        # The fixture used to read stderr only at stop(), so a test that
        # wraps the log ring would write well past the 64 KiB pipe buffer
        # and deadlock the daemon mid-test. Pings log at DEBUG
        # (dispatch.c), so enough of them exceed both the ring capacity and
        # the pipe buffer cheaply, with no process spawn per entry.
        taz_client.config_update({"log.level": "DEBUG"})
        target = 4200  # > the 4096-entry ring, each line far exceeds 64 KiB total
        deadline = time.monotonic() + 30
        sent = 0
        while sent < target and time.monotonic() < deadline:
            taz_client.ping()
            sent += 1
        assert sent == target, f"only sent {sent}/{target} pings before the deadline"
        # The connection, and the daemon behind it, must still be responsive.
        taz_client.ping()
        entries = taz_client.log(lines=5)
        assert len(entries) == 5


class TestErrorFrames:
    """The daemon's ERROR frames raise TazError; the connection stays usable."""

    def test_pipeline_request_raises_not_supported(self, taz_client: TazClient) -> None:
        # The client refuses an unadvertised opcode before sending it, so
        # advertise PIPELINE locally to put the raw REQUEST on the wire.
        taz_client._conn.capabilities.operations.append(common_pb2.OPCODE_PIPELINE)
        with pytest.raises(TazError) as exc_info:
            taz_client._call(common_pb2.OPCODE_PIPELINE, b"", empty_pb2.Empty, None)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        assert exc_info.value.message == "unknown opcode"
        assert taz_client.version().version == _project_version()

    def test_malformed_config_get_raises_invalid_request(
        self, taz_client: TazClient
    ) -> None:
        # keys (field 1) claims five bytes but only two follow.
        with pytest.raises(TazError) as exc_info:
            taz_client._call(
                common_pb2.OPCODE_CONFIGURATION_GET,
                b"\x0a\x05ab",
                daemon_control_pb2.ConfigurationGetResponse,
                None,
            )
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        assert taz_client.config_get(keys=["log.level"]) == {"log.level": "INFO"}
