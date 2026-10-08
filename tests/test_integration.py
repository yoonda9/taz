"""Integration tests: TazClient against a real daemon subprocess."""

from __future__ import annotations

import os
import re
import sys
import threading
import time
from pathlib import Path

import google_crc32c
import psutil
import pytest
from google.protobuf import empty_pb2
from taz.c3 import (
    CommandResult,
    FileTransfer,
    Keepalive,
    TazClient,
    TazConnectionLost,
    TazError,
)
from taz.c3.file import Kind
from taz.v1 import command_pb2, common_pb2, daemon_control_pb2, file_pb2

from tests.conftest import Daemon

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


class TestPing:
    def test_ping_succeeds(self, taz_client: TazClient) -> None:
        taz_client.ping()

    def test_ping_after_idle(self, taz_client: TazClient) -> None:
        # Guards against the daemon ever adding idle disconnects; the client's
        # own keepalive (idle=60) stays quiet for the whole sleep.
        time.sleep(5)
        taz_client.ping()


class TestVersion:
    def test_version_matches_project_version(self, taz_client: TazClient) -> None:
        # build_info.c is generated from the CMake project version; a fallback
        # or stale value would differ.
        info = taz_client.version()
        assert info.version == _project_version()
        assert info.build
        assert info.platform


class TestCapabilities:
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
        # Bypass CommandNamespace (Step 04, not landed yet): send the raw
        # REQUEST and never read its response, simulating an abrupt client
        # disconnect while COMMAND_EXEC is in flight.
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


class TestConfigGet:
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
