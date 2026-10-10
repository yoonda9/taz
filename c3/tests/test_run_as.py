"""Tests for TazClient.run_as()."""

from __future__ import annotations

import socket
from unittest.mock import MagicMock, patch

import pytest
from taz.c3.client import TazClient
from taz.c3.errors import TazError
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header, unpack_header
from taz.c3.settings import Keepalive
from taz.v1 import advanced_pb2, common_pb2, daemon_control_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_cancel.py / test_timeout.py)
# ---------------------------------------------------------------------------

_RUN_AS_OPS = [common_pb2.OPCODE_PING, common_pb2.OPCODE_RUN_AS]


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    ops = operations if operations is not None else _RUN_AS_OPS
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


def _run_as_response_bytes(effective_user: str, stream_id: int = 1) -> bytes:
    resp = advanced_pb2.RunAsResponse(success=True, effective_user=effective_user)
    return _response_bytes(
        common_pb2.OPCODE_RUN_AS, resp.SerializeToString(), stream_id=stream_id
    )


_SOCKET_SPEC = sorted({*dir(socket.socket), "sendmsg"})


def _mock_sock(*chunks: bytes) -> MagicMock:
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


# ---------------------------------------------------------------------------
# client.run_as()
# ---------------------------------------------------------------------------


class TestRunAs:
    def test_sends_run_as_request_with_given_user(self) -> None:
        client, mock_sock = _connected_client_with_sock(_run_as_response_bytes("alice"))
        client.run_as("alice")
        sent = b"".join(mock_sock.sent)
        frame = unpack_header(sent[:HEADER_SIZE])
        assert frame.type == common_pb2.FRAME_TYPE_REQUEST
        assert frame.opcode == common_pb2.OPCODE_RUN_AS
        assert frame.opcode == 0x0040
        req = advanced_pb2.RunAsRequest()
        req.ParseFromString(sent[HEADER_SIZE : HEADER_SIZE + frame.length])
        assert req.user == "alice"

    def test_returns_effective_user(self) -> None:
        client = _connected_client(_run_as_response_bytes("alice"))
        assert client.run_as("alice") == "alice"

    def test_empty_user_sends_empty_user(self) -> None:
        client, mock_sock = _connected_client_with_sock(_run_as_response_bytes("root"))
        # A reset reports the daemon's own identity, now in effect again.
        assert client.run_as("") == "root"
        sent = b"".join(mock_sock.sent)
        frame = unpack_header(sent[:HEADER_SIZE])
        req = advanced_pb2.RunAsRequest()
        req.ParseFromString(sent[HEADER_SIZE : HEADER_SIZE + frame.length])
        assert req.user == ""

    def test_not_advertised_raises_not_supported(self) -> None:
        client = _connected_client(operations=[common_pb2.OPCODE_PING])
        with pytest.raises(TazError) as exc_info:
            client.run_as("alice")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED

    def test_not_advertised_sends_nothing(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING]
        )
        with pytest.raises(TazError):
            client.run_as("alice")
        mock_sock.sendmsg.assert_not_called()
        mock_sock.sendall.assert_not_called()

    def test_error_response_propagates_without_closing_connection(self) -> None:
        info = common_pb2.ErrorInfo(code=common_pb2.ERROR_CODE_NOT_FOUND, message="x")
        header = pack_header(
            Frame(
                type=common_pb2.FRAME_TYPE_ERROR,
                flags=0,
                opcode=common_pb2.OPCODE_RUN_AS,
                length=len(info.SerializeToString()),
                stream_id=1,
            )
        )
        client = _connected_client(header + info.SerializeToString())
        with pytest.raises(TazError) as exc_info:
            client.run_as("ghost")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        assert not client._conn.closed
