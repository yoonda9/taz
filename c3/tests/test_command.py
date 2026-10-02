"""Tests for CommandNamespace.exec() / CommandResult."""

from __future__ import annotations

import socket
from unittest.mock import MagicMock, patch

import pytest
from taz.c3.client import TazClient
from taz.c3.command import CommandResult
from taz.c3.errors import TazError
from taz.c3.protocol import frame
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header
from taz.c3.settings import Keepalive
from taz.v1 import command_pb2, common_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_client.py)
# ---------------------------------------------------------------------------


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    ops = (
        operations
        if operations is not None
        else [common_pb2.OPCODE_PING, common_pb2.OPCODE_COMMAND_EXEC]
    )
    from taz.v1 import daemon_control_pb2

    cap = daemon_control_pb2.CapabilityPayload(
        protocol_major=1,
        protocol_minor=0,
        operations=ops,
    )
    payload = cap.SerializeToString()
    header = pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_CAPABILITY,
            flags=0,
            opcode=0,
            length=len(payload),
            stream_id=0,
        )
    )
    return header + payload


def _response_bytes(
    opcode: int,
    payload: bytes = b"",
    stream_id: int = 1,
    flags: int = 0,
) -> bytes:
    header = pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_RESPONSE,
            flags=flags,
            opcode=opcode,
            length=len(payload),
            stream_id=stream_id,
        )
    )
    return header + payload


def _error_bytes(
    opcode: int,
    code: common_pb2.ErrorCode,
    message: str,
    stream_id: int = 1,
) -> bytes:
    info = common_pb2.ErrorInfo(code=code, message=message)
    payload = info.SerializeToString()
    header = pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_ERROR,
            flags=0,
            opcode=opcode,
            length=len(payload),
            stream_id=stream_id,
        )
    )
    return header + payload


_SOCKET_SPEC = sorted({*dir(socket.socket), "sendmsg"})


def _mock_sock(*chunks: bytes) -> MagicMock:
    """Mock socket that serves ``chunks`` sequentially then EOF.

    Records every ``sendmsg`` or ``sendall`` call's bytes on ``sock.sent``,
    copied eagerly: ``send_frame``'s retry loop pops consumed buffers off
    the same list it passed in, so reading ``call_args_list`` after the fact
    would see it already drained to empty.
    """
    pending = [bytearray(c) for c in chunks]
    sent: list[bytes] = []

    def recv(bufsize: int, flags: int = 0) -> bytes:
        while pending:
            chunk = pending[0]
            if not chunk:
                pending.pop(0)
                continue
            if flags & socket.MSG_PEEK:
                return bytes(chunk[:1])
            take = min(bufsize, len(chunk))
            data = bytes(chunk[:take])
            del chunk[:take]
            if not chunk:
                pending.pop(0)
            return data
        return b""

    def sendmsg(views: list[memoryview]) -> int:
        sent.append(b"".join(bytes(view) for view in views))
        return sum(len(view) for view in views)

    def sendall(data: bytes) -> None:
        sent.append(data)

    sock = MagicMock(spec=_SOCKET_SPEC)
    sock.recv.side_effect = recv
    sock.gettimeout.return_value = None
    sock.sendmsg.side_effect = sendmsg
    sock.sendall.side_effect = sendall
    sock.sent = sent
    return sock


_PATCH_CC = "taz.c3.connection.socket.create_connection"


def _connected_client(
    *extra_chunks: bytes, operations: list[int] | None = None
) -> TazClient:
    client, _ = _connected_client_with_sock(*extra_chunks, operations=operations)
    return client


def _connected_client_with_sock(
    *extra_chunks: bytes, operations: list[int] | None = None
) -> tuple[TazClient, MagicMock]:
    mock_sock = _mock_sock(_capability_bytes(operations=operations), *extra_chunks)
    client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
    with patch(_PATCH_CC, return_value=mock_sock):
        client.connect()
    return client, mock_sock


def _exec_response(
    exit_code: int = 0,
    stdout_data: bytes = b"",
    stderr_data: bytes = b"",
    timed_out: bool = False,
    truncated: bool = False,
) -> bytes:
    resp = command_pb2.CommandExecResponse(
        exit_code=exit_code,
        stdout_data=stdout_data,
        stderr_data=stderr_data,
        timed_out=timed_out,
        truncated=truncated,
    )
    return _response_bytes(common_pb2.OPCODE_COMMAND_EXEC, resp.SerializeToString())


# ---------------------------------------------------------------------------
# command.exec()
# ---------------------------------------------------------------------------


class TestCommandExec:
    def test_exec_returns_command_result(self) -> None:
        client = _connected_client(_exec_response(exit_code=0, stdout_data=b"hello\n"))
        result = client.command.exec("echo", args=["hello"])
        assert result == CommandResult(
            exit_code=0,
            stdout=b"hello\n",
            stderr=b"",
            timed_out=False,
            truncated=False,
        )

    def test_exec_maps_all_response_fields(self) -> None:
        client = _connected_client(
            _exec_response(
                exit_code=3,
                stdout_data=b"out",
                stderr_data=b"err",
                timed_out=True,
                truncated=True,
            )
        )
        result = client.command.exec("cmd")
        assert result.exit_code == 3
        assert result.stdout == b"out"
        assert result.stderr == b"err"
        assert result.timed_out is True
        assert result.truncated is True

    def test_exec_sends_args_env_working_dir_timeout(self) -> None:
        client, mock_sock = _connected_client_with_sock(_exec_response())
        client.command.exec(
            "cmd",
            args=["a", "b"],
            env={"FOO": "bar"},
            working_dir="/srv/app",
            timeout_ms=5000,
        )
        sent = b"".join(mock_sock.sent)
        req = command_pb2.CommandExecRequest()
        # Header is TAZ's frame header; the payload is everything after it.
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.command == "cmd"
        assert list(req.args) == ["a", "b"]
        assert [(kv.key, kv.value) for kv in req.env] == [("FOO", "bar")]
        assert req.working_dir == "/srv/app"
        assert req.timeout_ms == 5000

    def test_exec_sends_request_via_sendall_fallback(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """On platforms without sendmsg (e.g. Windows), send_frame falls back to
        sendall; the request must still round-trip correctly through that path."""
        monkeypatch.setattr(frame, "_HAS_SENDMSG", False)
        client, mock_sock = _connected_client_with_sock(_exec_response())
        client.command.exec("cmd", args=["a", "b"], working_dir="/srv/app")
        mock_sock.sendmsg.assert_not_called()
        sent = b"".join(mock_sock.sent)
        req = command_pb2.CommandExecRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.command == "cmd"
        assert list(req.args) == ["a", "b"]
        assert req.working_dir == "/srv/app"

    def test_exec_merges_chunked_response_across_three_frames(self) -> None:
        """stdout_data/stderr_data split across CONTINUATION frames concatenate."""
        first = _response_bytes(
            common_pb2.OPCODE_COMMAND_EXEC,
            command_pb2.CommandExecResponse(stdout_data=b"x" * 100).SerializeToString(),
            flags=common_pb2.FRAME_FLAG_CONTINUATION,
        )
        second = _response_bytes(
            common_pb2.OPCODE_COMMAND_EXEC,
            command_pb2.CommandExecResponse(stdout_data=b"y" * 100).SerializeToString(),
            flags=common_pb2.FRAME_FLAG_CONTINUATION,
        )
        last = _response_bytes(
            common_pb2.OPCODE_COMMAND_EXEC,
            command_pb2.CommandExecResponse(
                stdout_data=b"z" * 100, exit_code=0
            ).SerializeToString(),
        )
        client = _connected_client(first, second, last)
        result = client.command.exec("cmd")
        assert result.stdout == b"x" * 100 + b"y" * 100 + b"z" * 100
        assert result.exit_code == 0

    def test_exec_error_frame_raises_taz_error(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_COMMAND_EXEC,
            common_pb2.ERROR_CODE_NOT_FOUND,
            "spawn failed",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.command.exec("does-not-exist")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_exec_unadvertised_opcode_raises_not_supported_locally(self) -> None:
        client = _connected_client(operations=[common_pb2.OPCODE_PING])
        with pytest.raises(TazError) as exc_info:
            client.command.exec("cmd")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        # Local rejection: the connection is not torn down.
        assert not client._conn.closed

    def test_exec_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(_exec_response())
        # Should not raise even with an explicit per-call keepalive override.
        client.command.exec("cmd", keepalive=Keepalive(idle=60.0, timeout=10.0))
