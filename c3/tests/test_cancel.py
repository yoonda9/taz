"""Tests for TazClient.cancel()."""

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
# Helpers (same pattern as test_command.py)
# ---------------------------------------------------------------------------

_CANCEL_OPS = [common_pb2.OPCODE_PING, common_pb2.OPCODE_CANCEL]


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    ops = operations if operations is not None else _CANCEL_OPS
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


def _cancel_response_bytes(cancelled: bool, stream_id: int = 1) -> bytes:
    resp = advanced_pb2.CancelResponse(cancelled=cancelled)
    return _response_bytes(
        common_pb2.OPCODE_CANCEL, resp.SerializeToString(), stream_id=stream_id
    )


_SOCKET_SPEC = sorted({*dir(socket.socket), "sendmsg"})


def _mock_sock(*chunks: bytes) -> MagicMock:
    """Mock socket that serves ``chunks`` sequentially then EOF.

    Records every ``sendmsg``/``sendall`` call's bytes on ``sock.sent``,
    copied eagerly (send_frame's retry loop drains the list it was given).
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


def _connected_client_with_sock(
    *extra_chunks: bytes, operations: list[int] | None = None
) -> tuple[TazClient, MagicMock]:
    mock_sock = _mock_sock(_capability_bytes(operations=operations), *extra_chunks)
    client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
    with patch(_PATCH_CC, return_value=mock_sock):
        client.connect()
    return client, mock_sock


# ---------------------------------------------------------------------------
# cancel()
# ---------------------------------------------------------------------------


class TestCancel:
    def test_sends_cancel_request_with_target_stream_id(self) -> None:
        client, mock_sock = _connected_client_with_sock(_cancel_response_bytes(True))
        client.cancel(7)
        sent = b"".join(mock_sock.sent)
        frame = unpack_header(sent[:HEADER_SIZE])
        assert frame.type == common_pb2.FRAME_TYPE_REQUEST
        assert frame.opcode == common_pb2.OPCODE_CANCEL
        assert frame.opcode == 0x0046
        req = advanced_pb2.CancelRequest()
        req.ParseFromString(sent[HEADER_SIZE : HEADER_SIZE + frame.length])
        assert req.target_stream_id == 7

    @pytest.mark.parametrize("cancelled", [True, False])
    def test_returns_daemons_cancelled_value(self, cancelled: bool) -> None:
        client, _ = _connected_client_with_sock(_cancel_response_bytes(cancelled))
        assert client.cancel(7) is cancelled

    def test_closes_stream_so_late_frames_are_discarded(self) -> None:
        client, _ = _connected_client_with_sock(_cancel_response_bytes(True))
        client.cancel(7)
        assert 7 in client._dispatcher._closed

    def test_not_advertised_raises_not_supported(self) -> None:
        client, _ = _connected_client_with_sock(operations=[common_pb2.OPCODE_PING])
        with pytest.raises(TazError) as exc_info:
            client.cancel(7)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED

    def test_not_advertised_sends_nothing(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING]
        )
        with pytest.raises(TazError):
            client.cancel(7)
        mock_sock.sendmsg.assert_not_called()
        mock_sock.sendall.assert_not_called()

    def test_error_response_propagates_without_closing_stream(self) -> None:
        info = common_pb2.ErrorInfo(code=common_pb2.ERROR_CODE_INTERNAL, message="boom")
        header = pack_header(
            Frame(
                type=common_pb2.FRAME_TYPE_ERROR,
                flags=0,
                opcode=common_pb2.OPCODE_CANCEL,
                length=len(info.SerializeToString()),
                stream_id=1,
            )
        )
        client, _ = _connected_client_with_sock(header + info.SerializeToString())
        with pytest.raises(TazError) as exc_info:
            client.cancel(7)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INTERNAL
        assert 7 not in client._dispatcher._closed
