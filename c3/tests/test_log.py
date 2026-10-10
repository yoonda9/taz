"""Tests for TazClient.log() and LogEntry."""

from __future__ import annotations

import socket
from unittest.mock import MagicMock, patch

import pytest
from taz.c3.client import TazClient
from taz.c3.errors import TazError
from taz.c3.logs import LogEntry
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header
from taz.c3.settings import Keepalive
from taz.v1 import advanced_pb2, common_pb2, daemon_control_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_process.py)
# ---------------------------------------------------------------------------

_LOG_OPS = [common_pb2.OPCODE_PING, common_pb2.OPCODE_LOG]


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    ops = operations if operations is not None else _LOG_OPS
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


def _entry(
    timestamp: int, level: str = "INFO", message: str = "m"
) -> advanced_pb2.LogEntry:
    return advanced_pb2.LogEntry(timestamp=timestamp, level=level, message=message)


# ---------------------------------------------------------------------------
# client.log()
# ---------------------------------------------------------------------------


class TestLog:
    def test_log_sends_defaults(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_LOG,
                advanced_pb2.LogResponse().SerializeToString(),
            )
        )
        client.log()
        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.LogRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.lines == 100
        assert req.since == 0
        assert req.level == "DEBUG"

    def test_log_sends_given_fields(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_LOG,
                advanced_pb2.LogResponse().SerializeToString(),
            )
        )
        client.log(lines=5, since=1_700_000_000, level="WARN")
        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.LogRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.lines == 5
        assert req.since == 1_700_000_000
        assert req.level == "WARN"

    def test_log_maps_every_field(self) -> None:
        resp = advanced_pb2.LogResponse(
            entries=[
                _entry(1, "INFO", "first"),
                _entry(2, "ERROR", "second"),
            ]
        )
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_LOG, resp.SerializeToString())
        )
        entries = client.log()
        assert entries == [
            LogEntry(timestamp=1, level="INFO", message="first"),
            LogEntry(timestamp=2, level="ERROR", message="second"),
        ]

    def test_log_empty_response_returns_empty_list(self) -> None:
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_LOG,
                advanced_pb2.LogResponse().SerializeToString(),
            )
        )
        assert client.log() == []

    def test_log_chunked_response_concatenates_entries_in_frame_order(self) -> None:
        # One RESPONSE frame of 64 entries + CONTINUATION, a second with 1;
        # the dispatcher merges them (§6.1) before this method sees them.
        frame1 = advanced_pb2.LogResponse(entries=[_entry(i) for i in range(1, 65)])
        frame2 = advanced_pb2.LogResponse(entries=[_entry(65)])
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_LOG,
                frame1.SerializeToString(),
                flags=common_pb2.FRAME_FLAG_CONTINUATION,
            ),
            _response_bytes(common_pb2.OPCODE_LOG, frame2.SerializeToString()),
        )
        entries = client.log()
        assert len(entries) == 65
        assert [entry.timestamp for entry in entries] == list(range(1, 66))

    def test_log_unknown_level_raises_invalid_request(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_LOG,
            common_pb2.ERROR_CODE_INVALID_REQUEST,
            "unknown log level",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.log(level="TRACE")
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST

    def test_log_unadvertised_raises_not_supported(self) -> None:
        client = _connected_client(operations=[common_pb2.OPCODE_PING])
        with pytest.raises(TazError) as exc_info:
            client.log()
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        assert not client._conn.closed


# ---------------------------------------------------------------------------
# LogEntry
# ---------------------------------------------------------------------------


class TestLogEntry:
    def test_slots(self) -> None:
        assert "__slots__" in LogEntry.__dict__

    def test_frozen(self) -> None:
        entry = LogEntry(timestamp=1, level="INFO", message="m")
        with pytest.raises(AttributeError):
            entry.level = "ERROR"  # type: ignore[misc]


class TestExports:
    def test_log_entry_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.LogEntry is LogEntry

    def test_log_entry_in_all(self) -> None:
        import taz.c3

        assert "LogEntry" in taz.c3.__all__
