"""Integration tests: TazClient against a real daemon subprocess."""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

import psutil
import pytest
from google.protobuf import empty_pb2
from taz.c3 import TazClient, TazConnectionLost, TazError
from taz.v1 import command_pb2, common_pb2, daemon_control_pb2

from tests.conftest import Daemon

_DAEMON_CMAKELISTS = Path(__file__).resolve().parents[1] / "daemon" / "CMakeLists.txt"


def _project_version() -> str:
    """The ``VERSION`` of the daemon's ``project()`` call."""
    m = re.search(
        r"^\s*VERSION\s+(\d+\.\d+\.\d+)", _DAEMON_CMAKELISTS.read_text(), re.MULTILINE
    )
    assert m is not None, f"no project VERSION in {_DAEMON_CMAKELISTS}"
    return m.group(1)


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
