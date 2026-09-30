"""Tests for TazClient: ping, version, capabilities."""

from __future__ import annotations

import socket
from unittest.mock import MagicMock, patch

import pytest
from taz.c3.client import TazClient
from taz.c3.errors import TazConnectionLost, TazError
from taz.c3.protocol.frame import Frame, pack_header
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, daemon_control_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_connection.py)
# ---------------------------------------------------------------------------


def _capability_bytes(
    protocol_major: int = 1,
    protocol_minor: int = 0,
    operations: list[int] | None = None,
    max_payload_sizes: dict[int, int] | None = None,
) -> bytes:
    ops = (
        operations
        if operations is not None
        else [common_pb2.OPCODE_PING, common_pb2.OPCODE_VERSION]
    )
    cap = daemon_control_pb2.CapabilityPayload(
        protocol_major=protocol_major,
        protocol_minor=protocol_minor,
        operations=ops,
    )
    if max_payload_sizes:
        for key, value in max_payload_sizes.items():
            pair = cap.max_payload_sizes.add()
            pair.key = key
            pair.value = value
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


def _pong_bytes() -> bytes:
    return pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_PONG,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
        )
    )


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


# socket.socket has no sendmsg on Windows, where send_frame uses sendall; a
# spec with both lets the same mock serve whichever path the platform takes.
_SOCKET_SPEC = sorted({*dir(socket.socket), "sendmsg"})


def _mock_sock(*chunks: bytes) -> MagicMock:
    """Mock socket that serves ``chunks`` sequentially then EOF."""
    pending = [bytearray(c) for c in chunks]

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

    sock = MagicMock(spec=_SOCKET_SPEC)
    sock.recv.side_effect = recv
    sock.gettimeout.return_value = None
    sock.sendmsg.side_effect = lambda views: sum(len(v) for v in views)
    return sock


_PATCH_CC = "taz.c3.connection.socket.create_connection"


def _connected_client(
    *extra_chunks: bytes, operations: list[int] | None = None
) -> TazClient:
    """Return a TazClient connected via a mock socket."""
    mock_sock = _mock_sock(_capability_bytes(operations=operations), *extra_chunks)
    client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
    with patch(_PATCH_CC, return_value=mock_sock):
        client.connect()
    return client


# ---------------------------------------------------------------------------
# Context manager
# ---------------------------------------------------------------------------


class TestContextManager:
    def test_enter_connects_and_exit_closes(self) -> None:
        mock_sock = _mock_sock(_capability_bytes())
        with (
            patch(_PATCH_CC, return_value=mock_sock),
            TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF) as client,
        ):
            assert not client._conn.closed
        assert client._conn.closed

    def test_exit_on_exception_still_closes(self) -> None:
        mock_sock = _mock_sock(_capability_bytes())
        with (
            patch(_PATCH_CC, return_value=mock_sock),
            pytest.raises(RuntimeError),
            TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF) as client,
        ):
            raise RuntimeError("test")
        assert client._conn.closed


# ---------------------------------------------------------------------------
# ping()
# ---------------------------------------------------------------------------


class TestPing:
    def test_ping_returns_none(self) -> None:
        client = _connected_client(_pong_bytes())
        client.ping()

    def test_ping_with_per_call_keepalive(self) -> None:
        client = _connected_client(_pong_bytes())
        # Should not raise even with an explicit keepalive when PONG is ready.
        client.ping(keepalive=Keepalive(idle=60.0, timeout=10.0))

    def test_ping_eof_raises_connection_lost(self) -> None:
        # EOF while waiting for PONG → TazConnectionLost, socket closed.
        client = _connected_client()  # no PONG queued
        with pytest.raises(TazConnectionLost):
            client.ping()
        assert client._conn.closed

    def test_ping_subsequent_call_after_failure_raises(self) -> None:
        client = _connected_client()  # no PONG queued
        with pytest.raises(TazConnectionLost):
            client.ping()
        with pytest.raises(TazConnectionLost):
            client.ping()


# ---------------------------------------------------------------------------
# version()
# ---------------------------------------------------------------------------


class TestVersion:
    def _version_response(self, stream_id: int = 1) -> bytes:
        resp = daemon_control_pb2.VersionResponse(
            version="1.2.3",
            build="abc123",
            platform="linux/amd64",
        )
        return _response_bytes(
            common_pb2.OPCODE_VERSION,
            resp.SerializeToString(),
            stream_id=stream_id,
        )

    def test_version_returns_version_response(self) -> None:
        client = _connected_client(self._version_response())
        info = client.version()
        assert info.version == "1.2.3"
        assert info.build == "abc123"
        assert info.platform == "linux/amd64"

    def test_version_merges_chunked_response(self) -> None:
        """String fields split across CONTINUATION frames are concatenated."""
        first = _response_bytes(
            common_pb2.OPCODE_VERSION,
            daemon_control_pb2.VersionResponse(version="1.2").SerializeToString(),
            flags=common_pb2.FRAME_FLAG_CONTINUATION,
        )
        last = _response_bytes(
            common_pb2.OPCODE_VERSION,
            daemon_control_pb2.VersionResponse(
                version=".3", build="abc123"
            ).SerializeToString(),
        )
        client = _connected_client(first, last)
        info = client.version()
        assert info.version == "1.2.3"
        assert info.build == "abc123"

    def test_version_error_raises(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_VERSION,
            common_pb2.ERROR_CODE_INTERNAL,
            "internal error",
            stream_id=1,
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.version()
        assert exc_info.value.code == common_pb2.ERROR_CODE_INTERNAL

    def test_version_unadvertised_raises_not_supported(self) -> None:
        cap = _capability_bytes(operations=[common_pb2.OPCODE_PING])
        mock_sock = _mock_sock(cap)
        client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
        with patch(_PATCH_CC, return_value=mock_sock):
            client.connect()
        with pytest.raises(TazError) as exc_info:
            client.version()
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        assert not client._conn.closed


# ---------------------------------------------------------------------------
# capabilities()
# ---------------------------------------------------------------------------


class TestCapabilities:
    def test_capabilities_returns_parsed_payload(self) -> None:
        client = _connected_client()
        cap = client.capabilities()
        assert cap.protocol_major == 1
        assert common_pb2.OPCODE_PING in cap.operations
        assert common_pb2.OPCODE_VERSION in cap.operations

    def test_capabilities_before_connect_raises(self) -> None:
        client = TazClient("127.0.0.1", 5555)
        with pytest.raises(RuntimeError):
            client.capabilities()


# ---------------------------------------------------------------------------
# config_get() / config_update()
# ---------------------------------------------------------------------------

_CONFIG_OPS: list[int] = [
    common_pb2.OPCODE_CONFIGURATION_GET,
    common_pb2.OPCODE_CONFIGURATION_UPDATE,
]


class TestConfig:
    def test_config_get_returns_dict(self) -> None:
        resp = daemon_control_pb2.ConfigurationGetResponse(
            config=[
                common_pb2.KeyValue(key="log.level", value="INFO"),
                common_pb2.KeyValue(key="compression", value="NONE"),
            ]
        )
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_CONFIGURATION_GET, resp.SerializeToString()
            ),
            operations=_CONFIG_OPS,
        )
        assert client.config_get() == {"log.level": "INFO", "compression": "NONE"}

    def test_config_get_error_raises(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_CONFIGURATION_GET,
            common_pb2.ERROR_CODE_INVALID_REQUEST,
            "decode ConfigurationGetRequest failed",
        )
        client = _connected_client(err, operations=_CONFIG_OPS)
        with pytest.raises(TazError) as exc_info:
            client.config_get()
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        assert not client._conn.closed

    def test_config_update_returns_applied_and_rejected(self) -> None:
        resp = daemon_control_pb2.ConfigurationUpdateResponse(
            applied=["log.level"],
            rejected=[
                daemon_control_pb2.RejectedKey(key="invalid.key", reason="unknown key")
            ],
        )
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_CONFIGURATION_UPDATE, resp.SerializeToString()
            ),
            operations=_CONFIG_OPS,
        )
        result = client.config_update({"log.level": "DEBUG", "invalid.key": "x"})
        assert list(result.applied) == ["log.level"]
        assert [(r.key, r.reason) for r in result.rejected] == [
            ("invalid.key", "unknown key")
        ]


# ---------------------------------------------------------------------------
# REQUEST limit (one byte over limit → INVALID_REQUEST, socket untouched)
# ---------------------------------------------------------------------------


class TestRequestLimit:
    def test_oversized_request_raises_invalid_request(self) -> None:
        # Advertise a very small REQUEST limit.
        cap = _capability_bytes(
            max_payload_sizes={common_pb2.FRAME_TYPE_REQUEST: 4},
        )
        mock_sock = _mock_sock(cap)
        client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
        with patch(_PATCH_CC, return_value=mock_sock):
            client.connect()

        # Sending 5 bytes (limit is 4) must raise before touching the socket.
        oversized = b"X" * 5
        with pytest.raises(TazError) as exc_info:
            client._conn.send_request(common_pb2.OPCODE_PING, oversized)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        assert not client._conn.closed

        # A subsequent ping still works (PONG expected, not RESPONSE).
        mock_sock2 = _mock_sock(_pong_bytes())
        client._conn._sock = mock_sock2
        client.ping()


# ---------------------------------------------------------------------------
# Connection-lost propagation
# ---------------------------------------------------------------------------


class TestConnectionLost:
    def test_version_eof_raises_connection_lost(self) -> None:
        # EOF on the RESPONSE → TazConnectionLost and connection closed.
        mock_sock = _mock_sock(_capability_bytes())
        client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
        with patch(_PATCH_CC, return_value=mock_sock):
            client.connect()
        # No response queued — recv returns b"" (EOF) → connection lost.
        with pytest.raises(TazConnectionLost):
            client.version()
        assert client._conn.closed

    def test_subsequent_version_after_failure_raises(self) -> None:
        mock_sock = _mock_sock(_capability_bytes())
        client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
        with patch(_PATCH_CC, return_value=mock_sock):
            client.connect()
        with pytest.raises(TazConnectionLost):
            client.version()
        # Second call should also raise (fail-fast path).
        with pytest.raises(TazConnectionLost):
            client.version()
