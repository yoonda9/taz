"""Tests for the Python framing layer: header pack/unpack, validation, send/recv."""

from __future__ import annotations

import socket
import sys
from unittest.mock import MagicMock

import pytest
from taz.c3.errors import TazConnectionLost, TazProtocolError
from taz.c3.protocol import frame
from taz.v1 import common_pb2

_UNKNOWN_BOUND = frame.DEFAULT_UNKNOWN_TYPE_MAX_PAYLOAD


def _frame(
    type_: int,
    length: int = 0,
    *,
    flags: int = 0,
    opcode: int = 0,
    stream_id: int = 0,
) -> frame.Frame:
    return frame.Frame(
        type=type_, flags=flags, opcode=opcode, length=length, stream_id=stream_id
    )


def _verdict(type_: int, length: int) -> frame.Verdict:
    return frame.validate_header(_frame(type_, length), frame.DEFAULT_MAX_PAYLOAD)


def _sock(*chunks: bytes | OSError) -> MagicMock:
    """Mock socket whose recv serves ``chunks`` one read at a time, then EOF.

    An exception in ``chunks`` is raised by the read that reaches it.
    MSG_PEEK returns the first byte without consuming it from the queue.
    When a chunk is consumed in full, the original bytes object is returned
    (enabling identity checks in _recv_exact single-read tests).
    """
    pending = list(chunks)

    def recv(bufsize: int, flags: int = 0) -> bytes:
        if not pending:
            return b""
        chunk = pending[0]
        if isinstance(chunk, OSError):
            pending.pop(0)
            raise chunk
        if flags & socket.MSG_PEEK:
            return chunk[:1] if chunk else b""
        take = min(bufsize, len(chunk))
        if take == len(chunk):
            pending.pop(0)
            return chunk  # return original object for identity tests
        pending[0] = chunk[take:]
        return chunk[:take]

    sock = MagicMock(spec=socket.socket)
    sock.recv.side_effect = recv
    return sock


# ---------------------------------------------------------------------------
# Pack / unpack
# ---------------------------------------------------------------------------


class TestPackUnpack:
    """Test header pack and unpack operations."""

    @pytest.mark.parametrize(
        "f",
        [
            _frame(
                common_pb2.FRAME_TYPE_REQUEST,
                0x00ABCDEF,
                flags=0xAB,
                opcode=0x1234,
                stream_id=0xDEADBEEF,
            ),
            _frame(common_pb2.FRAME_TYPE_PING),
            _frame(
                common_pb2.FRAME_TYPE_REQUEST,
                0xFFFFFFFF,
                flags=0xFF,
                opcode=0xFFFF,
                stream_id=0xFFFFFFFF,
            ),
            _frame(
                0x08,  # reserved
                0x56789ABC,
                flags=0x55,
                opcode=0x1234,
                stream_id=0xDEADBEEF,
            ),
            _frame(
                common_pb2.FRAME_TYPE_REQUEST, 0, opcode=0x0134, stream_id=0x12345678
            ),
        ],
    )
    def test_pack_unpack_roundtrip(self, f: frame.Frame) -> None:
        """Verify a frame round-trips through pack/unpack unchanged."""
        assert frame.unpack_header(frame.pack_header(f)) == f

    def test_wire_layout_little_endian(self) -> None:
        """Verify each multi-byte field is stored little-endian."""
        f = _frame(
            common_pb2.FRAME_TYPE_RESPONSE,
            0x01020304,
            flags=0x01,
            opcode=0x1234,
            stream_id=0xAABBCCDD,
        )
        assert frame.pack_header(f) == bytes(
            [common_pb2.FRAME_TYPE_RESPONSE, 0x01]
        ) + bytes.fromhex(
            "3412"  # opcode
            "04030201"  # length
            "ddccbbaa"  # stream_id
        )

    def test_header_size_is_12_bytes(self) -> None:
        """Verify HEADER_SIZE is exactly 12 bytes."""
        assert frame.HEADER_SIZE == 12

    def test_repr_omits_payload(self) -> None:
        """Logging a frame does not dump its payload."""
        f = _frame(common_pb2.FRAME_TYPE_FILE_CHUNK, 65536)
        f.payload = b"\xab" * 65536
        assert "payload" not in repr(f)


# ---------------------------------------------------------------------------
# Max-payload table (§6)
# ---------------------------------------------------------------------------


class TestMaxPayloadTable:
    """Test the default max payload limits."""

    def test_default_limits_match_spec(self) -> None:
        """Protocol §6 default limits."""
        assert frame.DEFAULT_MAX_PAYLOAD == {
            common_pb2.FRAME_TYPE_PING: 0,
            common_pb2.FRAME_TYPE_PONG: 0,
            common_pb2.FRAME_TYPE_CAPABILITY: 1024,
            common_pb2.FRAME_TYPE_ERROR: 4096,
            common_pb2.FRAME_TYPE_REQUEST: 65536,
            common_pb2.FRAME_TYPE_RESPONSE: 65536,
            common_pb2.FRAME_TYPE_FILE_CHUNK: 65536,
        }

    def test_all_known_types_in_default_table(self) -> None:
        """Every FrameType in the proto schema has a default limit."""
        known_types = set(common_pb2.FrameType.values()) - {
            common_pb2.FRAME_TYPE_UNSPECIFIED
        }
        assert set(frame.DEFAULT_MAX_PAYLOAD) == known_types

    def test_default_table_is_read_only(self) -> None:
        """The defaults (recv_frame's default argument) cannot be mutated."""
        with pytest.raises(TypeError):
            frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_PING] = 1  # type: ignore[index]


# ---------------------------------------------------------------------------
# Validate header
# ---------------------------------------------------------------------------


class TestValidateHeader:
    """Test header validation logic."""

    @pytest.mark.parametrize(
        ("ftype", "limit"), sorted(frame.DEFAULT_MAX_PAYLOAD.items())
    )
    def test_validate_known_type_limit_boundary(self, ftype: int, limit: int) -> None:
        """A known type is OK at its limit and OVERSIZED one byte over (§6)."""
        assert _verdict(ftype, limit) == frame.Verdict.OK
        assert _verdict(ftype, limit + 1) == frame.Verdict.OVERSIZED

    def test_validate_limits_for_unassigned_types_are_ignored(self) -> None:
        """Table entries for unassigned types neither make them known nor raise
        the unknown-type bound (§4.2, §10.1)."""
        limits = {**frame.DEFAULT_MAX_PAYLOAD, 0x00: 1 << 20, 0x08: 1 << 20}

        def verdict(ftype: int, length: int) -> frame.Verdict:
            return frame.validate_header(_frame(ftype, length), limits)

        assert verdict(0x00, 100) == frame.Verdict.UNKNOWN_TYPE
        assert verdict(0x08, 100) == frame.Verdict.UNKNOWN_TYPE
        assert verdict(0x09, _UNKNOWN_BOUND + 1) == frame.Verdict.OVERSIZED

    def test_validate_partial_limits_fall_back_to_defaults(self) -> None:
        """Known types missing from the table keep their §6 default."""
        limits: dict[int, int] = {common_pb2.FRAME_TYPE_REQUEST: 1024}

        def verdict(ftype: int, length: int) -> frame.Verdict:
            return frame.validate_header(_frame(ftype, length), limits)

        assert verdict(common_pb2.FRAME_TYPE_PING, 0) == frame.Verdict.OK
        assert verdict(common_pb2.FRAME_TYPE_PING, 1) == frame.Verdict.OVERSIZED
        assert verdict(common_pb2.FRAME_TYPE_ERROR, 2048) == frame.Verdict.OK
        assert verdict(common_pb2.FRAME_TYPE_REQUEST, 1025) == frame.Verdict.OVERSIZED
        assert verdict(0x08, _UNKNOWN_BOUND) == frame.Verdict.UNKNOWN_TYPE
        assert frame.validate_header(_frame(0x08, 0), {}) == frame.Verdict.UNKNOWN_TYPE

    def test_validate_raised_limit_raises_unknown_type_bound(self) -> None:
        """The unknown-type bound follows the largest known-type limit (§10.1)."""
        limits = {**frame.DEFAULT_MAX_PAYLOAD, common_pb2.FRAME_TYPE_RESPONSE: 1 << 17}
        f = _frame(0x08, 1 << 17)
        assert frame.validate_header(f, limits) == frame.Verdict.UNKNOWN_TYPE

    @pytest.mark.parametrize("ftype", [0x00, 0x08, 0xFF])
    def test_validate_unknown_type_within_bound_is_unknown_type(
        self, ftype: int
    ) -> None:
        """Unknown types (incl. UNSPECIFIED) up to the 64 KiB bound (§10.1)."""
        assert _verdict(ftype, 0) == frame.Verdict.UNKNOWN_TYPE
        assert _verdict(ftype, _UNKNOWN_BOUND) == frame.Verdict.UNKNOWN_TYPE

    @pytest.mark.parametrize("ftype", [0x00, 0x08, 0xFF])
    def test_validate_unknown_type_oversized(self, ftype: int) -> None:
        """Unknown type exceeding the max bound fails as OVERSIZED."""
        assert _verdict(ftype, _UNKNOWN_BOUND + 1) == frame.Verdict.OVERSIZED

    def test_validate_ping_nonzero_opcode_is_ok(self) -> None:
        """PING validation ignores the opcode field."""
        f = _frame(common_pb2.FRAME_TYPE_PING, opcode=0xFFFF)
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OK

    def test_validate_all_flag_bits_set_is_ok(self) -> None:
        """All flag bits set is valid."""
        f = _frame(common_pb2.FRAME_TYPE_REQUEST, flags=0xFF)
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OK


# ---------------------------------------------------------------------------
# Send / receive frames (socket operations)
# ---------------------------------------------------------------------------


class TestSendFrame:
    """Test send_frame operation."""

    def test_send_frame_does_not_mutate_caller_frame(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """send_frame does not mutate the caller's frame.length."""
        monkeypatch.setattr(frame, "_HAS_SENDMSG", False)
        mock_sock = MagicMock(spec=socket.socket)
        f = _frame(common_pb2.FRAME_TYPE_REQUEST, 999, opcode=1, stream_id=1)
        f.payload = b"test_payload"

        frame.send_frame(mock_sock, f)

        assert f.length == 999  # unchanged; send_frame must not mutate the caller
        expected_header = frame.pack_header(
            frame.Frame(
                type=f.type,
                flags=f.flags,
                opcode=f.opcode,
                length=len(b"test_payload"),
                stream_id=f.stream_id,
            )
        )
        mock_sock.sendall.assert_called_once_with(expected_header + b"test_payload")

    def test_send_frame_with_empty_payload(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """send_frame handles empty payload; caller's length is not changed."""
        monkeypatch.setattr(frame, "_HAS_SENDMSG", False)
        mock_sock = MagicMock(spec=socket.socket)
        f = _frame(common_pb2.FRAME_TYPE_PING, 42)  # stale length

        frame.send_frame(mock_sock, f)

        assert f.length == 42  # not mutated
        expected_header = frame.pack_header(
            frame.Frame(f.type, f.flags, f.opcode, 0, f.stream_id)
        )
        mock_sock.sendall.assert_called_once_with(expected_header)

    @pytest.mark.parametrize(
        "error", [BrokenPipeError(32, "Broken pipe"), TimeoutError("timed out")]
    )
    def test_send_frame_sendall_error_raises_connection_lost(
        self, error: OSError, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """A failed sendall surfaces as TazConnectionLost."""
        monkeypatch.setattr(frame, "_HAS_SENDMSG", False)
        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.sendall.side_effect = error

        with pytest.raises(TazConnectionLost) as exc_info:
            frame.send_frame(mock_sock, _frame(common_pb2.FRAME_TYPE_PING))

        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST
        assert exc_info.value.__cause__ is error

    @pytest.mark.skipif(
        not frame._HAS_SENDMSG, reason="sendmsg not available on this platform"
    )
    @pytest.mark.parametrize(
        "error", [BrokenPipeError(32, "Broken pipe"), TimeoutError("timed out")]
    )
    def test_send_frame_sendmsg_error_raises_connection_lost(
        self, error: OSError, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """A failed sendmsg surfaces as TazConnectionLost."""
        monkeypatch.setattr(frame, "_HAS_SENDMSG", True)
        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.sendmsg.side_effect = error

        with pytest.raises(TazConnectionLost) as exc_info:
            frame.send_frame(mock_sock, _frame(common_pb2.FRAME_TYPE_PING))

        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST
        assert exc_info.value.__cause__ is error

    def test_send_frame_bytes_via_socketpair(self) -> None:
        """Bytes received by peer equal header+payload regardless of path taken."""
        a, b = socket.socketpair()
        try:
            payload = b"hello_socketpair"
            f = _frame(common_pb2.FRAME_TYPE_REQUEST, 0, opcode=7, stream_id=42)
            f.payload = payload
            frame.send_frame(a, f)
            a.shutdown(socket.SHUT_WR)
            received = b""
            while chunk := b.recv(4096):
                received += chunk
        finally:
            a.close()
            b.close()

        expected_header = frame.pack_header(
            frame.Frame(f.type, f.flags, f.opcode, len(payload), f.stream_id)
        )
        assert received == expected_header + payload

    @pytest.mark.skipif(
        not frame._HAS_SENDMSG, reason="sendmsg not available on this platform"
    )
    def test_send_frame_sendmsg_partial_sends_deliver_all_bytes(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """_sendmsg_all retries correctly when sendmsg returns short counts."""
        monkeypatch.setattr(frame, "_HAS_SENDMSG", True)

        payload = b"hello_world_0123456789"
        f = _frame(common_pb2.FRAME_TYPE_REQUEST, 0, opcode=1, stream_id=42)
        f.payload = payload

        expected_header = frame.pack_header(
            frame.Frame(f.type, f.flags, f.opcode, len(payload), f.stream_id)
        )
        all_bytes = expected_header + payload

        # sendmsg sends 5 bytes at a time to exercise partial-send retry
        chunk_size = 5
        collected = bytearray()

        def mock_sendmsg(views: list[memoryview]) -> int:
            to_send = min(chunk_size, sum(len(v) for v in views))
            remaining = to_send
            for v in views:
                take = min(remaining, len(v))
                collected.extend(bytes(v[:take]))
                remaining -= take
                if remaining == 0:
                    break
            return to_send

        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.sendmsg.side_effect = mock_sendmsg

        frame.send_frame(mock_sock, f)

        assert bytes(collected) == all_bytes

    @pytest.mark.skipif(
        not frame._HAS_SENDMSG, reason="sendmsg not available on this platform"
    )
    def test_send_frame_fallback_path_same_bytes(
        self, monkeypatch: pytest.MonkeyPatch
    ) -> None:
        """With _HAS_SENDMSG=False the concatenation path produces the same bytes."""
        payload = b"fallback_test"
        f = _frame(common_pb2.FRAME_TYPE_RESPONSE, 0, opcode=2, stream_id=7)
        f.payload = payload

        expected_header = frame.pack_header(
            frame.Frame(f.type, f.flags, f.opcode, len(payload), f.stream_id)
        )

        monkeypatch.setattr(frame, "_HAS_SENDMSG", True)
        mock_sendmsg = MagicMock(spec=socket.socket)
        collected_sendmsg = bytearray()

        def capture_sendmsg(views: list[memoryview]) -> int:
            total = sum(len(v) for v in views)
            for v in views:
                collected_sendmsg.extend(bytes(v))
            return total

        mock_sendmsg.sendmsg.side_effect = capture_sendmsg
        frame.send_frame(mock_sendmsg, f)

        monkeypatch.setattr(frame, "_HAS_SENDMSG", False)
        mock_sendall = MagicMock(spec=socket.socket)
        frame.send_frame(mock_sendall, f)
        collected_sendall = mock_sendall.sendall.call_args[0][0]

        assert bytes(collected_sendmsg) == collected_sendall
        assert collected_sendall == expected_header + payload


class TestRecvFrame:
    """Test recv_frame operation."""

    def test_recv_frame_basic(self) -> None:
        """recv_frame reads header and payload correctly."""
        mock_sock = _sock(frame.pack_header(_frame(common_pb2.FRAME_TYPE_PING)))

        f = frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert f.type == common_pb2.FRAME_TYPE_PING
        assert f.length == 0
        assert f.payload == b""

    def test_recv_frame_with_payload(self) -> None:
        """recv_frame receives header and payload correctly."""
        payload = b"hello"
        header = frame.pack_header(
            _frame(common_pb2.FRAME_TYPE_REQUEST, len(payload), opcode=1, stream_id=42)
        )
        mock_sock = _sock(header, payload)

        f = frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert f.type == common_pb2.FRAME_TYPE_REQUEST
        assert f.opcode == 1
        assert f.stream_id == 42
        assert f.length == len(payload)
        assert f.payload == payload

    def test_recv_frame_oversized_known_type_raises_protocol_error(self) -> None:
        """recv_frame raises TazProtocolError naming type, both sizes, and detail."""
        oversized_length = 65537
        limit = frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST]
        mock_sock = _sock(
            frame.pack_header(
                _frame(
                    common_pb2.FRAME_TYPE_REQUEST,
                    oversized_length,
                    opcode=2,
                    stream_id=42,
                )
            )
        )

        with pytest.raises(TazProtocolError) as exc_info:
            frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        err = exc_info.value
        assert f"REQUEST payload {oversized_length} bytes exceeds limit {limit}" in str(
            err
        )
        assert err.detail == (
            f"type=0x{common_pb2.FRAME_TYPE_REQUEST:02x} flags=0x00 "
            f"opcode=0x{2:04x} stream_id=42 "
            f"length={oversized_length} limit={limit}"
        )

    def test_recv_frame_oversized_unknown_type_raises_protocol_error(self) -> None:
        """recv_frame raises TazProtocolError with hex type label for unknown types."""
        unknown_type = 0x09
        oversized_length = _UNKNOWN_BOUND + 1
        mock_sock = _sock(frame.pack_header(_frame(unknown_type, oversized_length)))

        with pytest.raises(TazProtocolError) as exc_info:
            frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        err = exc_info.value
        assert f"type 0x{unknown_type:02x} payload {oversized_length}" in str(err)

    def test_recv_frame_oversized_payload_not_read(self) -> None:
        """recv_frame does not consume payload bytes after detecting OVERSIZED."""
        type_ = common_pb2.FRAME_TYPE_RESPONSE
        limit = frame.DEFAULT_MAX_PAYLOAD[type_]
        sentinel = b"not_consumed"
        mock_sock = _sock(frame.pack_header(_frame(type_, limit + 1)) + sentinel)

        with pytest.raises(TazProtocolError):
            frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        # sentinel bytes are still in the mock's queue
        data = mock_sock.recv(len(sentinel))
        assert data == sentinel

    def test_recv_frame_connection_timeout_raises_connection_lost(self) -> None:
        """recv_frame raises TazConnectionLost on socket timeout."""
        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.recv.side_effect = TimeoutError("socket timeout")

        with pytest.raises(TazConnectionLost) as exc_info:
            frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert "timed out" in str(exc_info.value)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST

    def test_recv_frame_connection_closed_raises_connection_lost(self) -> None:
        """recv_frame raises TazConnectionLost when connection closes."""
        with pytest.raises(TazConnectionLost) as exc_info:
            frame.recv_frame(_sock(), frame.DEFAULT_MAX_PAYLOAD)

        assert "closed by peer" in str(exc_info.value)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST

    @pytest.mark.parametrize(
        "received",
        [5, frame.HEADER_SIZE + 4],
        ids=["mid-header", "mid-payload"],
    )
    def test_recv_frame_eof_mid_frame_raises_connection_lost(
        self, received: int
    ) -> None:
        """EOF partway through a frame is TazConnectionLost, never a short frame."""
        payload = b"0123456789"
        header = frame.pack_header(_frame(common_pb2.FRAME_TYPE_REQUEST, len(payload)))
        mock_sock = _sock((header + payload)[:received])

        with pytest.raises(TazConnectionLost) as exc_info:
            frame.recv_frame(mock_sock)

        assert "closed by peer" in str(exc_info.value)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST

    def test_recv_frame_connection_reset_raises_connection_lost(self) -> None:
        """A reset mid-frame (e.g. the daemon closing after an ERROR, §10.1)
        surfaces as TazConnectionLost, not a raw OSError."""
        reset = ConnectionResetError(104, "Connection reset by peer")
        header = frame.pack_header(_frame(common_pb2.FRAME_TYPE_RESPONSE, 8))
        mock_sock = _sock(header, b"1234", reset)

        with pytest.raises(TazConnectionLost) as exc_info:
            frame.recv_frame(mock_sock)

        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST
        assert exc_info.value.__cause__ is reset

    def test_recv_frame_unknown_type_returned_with_payload(self) -> None:
        """An in-bounds unknown-type frame is returned whole, so the next frame
        still parses (§10.1 forward compatibility)."""
        unknown_payload = b"future"
        unknown = frame.pack_header(_frame(0x08, len(unknown_payload), stream_id=3))
        ping = frame.pack_header(_frame(common_pb2.FRAME_TYPE_PING))
        mock_sock = _sock(unknown + unknown_payload + ping)

        f1 = frame.recv_frame(mock_sock)
        f2 = frame.recv_frame(mock_sock)

        assert f1.type == 0x08
        assert f1.payload == unknown_payload
        assert frame.validate_header(f1, frame.DEFAULT_MAX_PAYLOAD) == (
            frame.Verdict.UNKNOWN_TYPE
        )
        assert f2.type == common_pb2.FRAME_TYPE_PING

    def test_recv_frame_custom_limits(self) -> None:
        """recv_frame respects custom max payload limits."""
        # Custom limit: REQUEST max is 1024 instead of 65536
        limits: dict[int, int] = {common_pb2.FRAME_TYPE_REQUEST: 1024}
        mock_sock = _sock(
            frame.pack_header(_frame(common_pb2.FRAME_TYPE_REQUEST, 2048))
        )

        with pytest.raises(TazProtocolError) as exc_info:
            frame.recv_frame(mock_sock, limits)

        assert "REQUEST payload 2048 bytes exceeds limit 1024" in str(exc_info.value)

    def test_recv_frame_partial_header_reassembly(self) -> None:
        """recv_frame reassembles a header that arrives in multiple partial reads."""
        payload = b"partial_test"
        header = frame.pack_header(
            _frame(common_pb2.FRAME_TYPE_REQUEST, len(payload), opcode=5, stream_id=99)
        )
        # Wire data arrives in 3 chunks: first 4 bytes, last 8 bytes of header, payload.
        mock_sock = _sock(header[:4], header[4:], payload)

        f = frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert f.type == common_pb2.FRAME_TYPE_REQUEST
        assert f.opcode == 5
        assert f.stream_id == 99
        assert f.payload == payload

    def test_recv_frame_concatenated_frames_boundary(self) -> None:
        """recv_frame reads exactly one frame when two frames arrive back-to-back."""
        payload1 = b"first_frame_payload"
        payload2 = b"second_frame_payload"
        header1 = frame.pack_header(
            _frame(common_pb2.FRAME_TYPE_REQUEST, len(payload1), opcode=1, stream_id=1)
        )
        header2 = frame.pack_header(
            _frame(common_pb2.FRAME_TYPE_RESPONSE, len(payload2), opcode=2, stream_id=2)
        )
        mock_sock = _sock(header1 + payload1 + header2 + payload2)

        f1 = frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)
        f2 = frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert f1.type == common_pb2.FRAME_TYPE_REQUEST
        assert f1.opcode == 1
        assert f1.stream_id == 1
        assert f1.payload == payload1

        assert f2.type == common_pb2.FRAME_TYPE_RESPONSE
        assert f2.opcode == 2
        assert f2.stream_id == 2
        assert f2.payload == payload2

    def test_recv_frame_raised_response_limit_accepts_large_response(self) -> None:
        """recv_frame with RESPONSE raised to 128 KiB accepts a 100 KiB response.

        The default limit (65536) rejects it; a raised limit (131072) accepts it.
        This verifies the limits parameter is actually used for validation.
        """
        payload_size = 100 * 1024  # 100 KiB — exceeds 64 KiB default
        payload = b"x" * payload_size

        response_frame = _frame(
            common_pb2.FRAME_TYPE_RESPONSE, payload_size, opcode=1, stream_id=1
        )
        # Confirm default limits reject this size.
        assert (
            frame.validate_header(response_frame, frame.DEFAULT_MAX_PAYLOAD)
            == frame.Verdict.OVERSIZED
        )

        raised_limits = dict(frame.DEFAULT_MAX_PAYLOAD)
        raised_limits[common_pb2.FRAME_TYPE_RESPONSE] = 128 * 1024  # 128 KiB
        mock_sock = _sock(frame.pack_header(response_frame) + payload)

        f = frame.recv_frame(mock_sock, raised_limits)

        assert f.type == common_pb2.FRAME_TYPE_RESPONSE
        assert f.length == payload_size
        assert f.payload == payload


# ---------------------------------------------------------------------------
# pack_header range validation
# ---------------------------------------------------------------------------


class TestPackHeaderRanges:
    """pack_header raises ValueError for any field value outside its wire range."""

    @pytest.mark.parametrize(
        ("overrides", "field_name"),
        [
            ({"type": -1}, "type"),
            ({"type": 0x100}, "type"),
            ({"flags": -1}, "flags"),
            ({"flags": 0x100}, "flags"),
            ({"opcode": -1}, "opcode"),
            ({"opcode": 0x10000}, "opcode"),
            ({"length": -1}, "length"),
            ({"length": 0x1_0000_0000}, "length"),
            ({"stream_id": -1}, "stream_id"),
            ({"stream_id": 0x1_0000_0000}, "stream_id"),
        ],
    )
    def test_field_out_of_range_raises_value_error(
        self, overrides: dict[str, int], field_name: str
    ) -> None:
        base = dict(type=1, flags=0, opcode=0, length=0, stream_id=0)
        f = frame.Frame(**{**base, **overrides})  # type: ignore[arg-type]
        with pytest.raises(ValueError, match=field_name):
            frame.pack_header(f)

    @pytest.mark.parametrize(
        "kwargs",
        [
            dict(type=0, flags=0, opcode=0, length=0, stream_id=0),
            dict(
                type=0xFF,
                flags=0xFF,
                opcode=0xFFFF,
                length=0xFFFF_FFFF,
                stream_id=0xFFFF_FFFF,
            ),
        ],
        ids=["min-bounds", "max-bounds"],
    )
    def test_exact_bounds_pack_without_error(self, kwargs: dict[str, int]) -> None:
        """Exact boundary values (0 and max for each field) must pack cleanly."""
        frame.pack_header(frame.Frame(**kwargs))  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# Frame and Verdict types
# ---------------------------------------------------------------------------


class TestFrameCleanups:
    """Frame uses __slots__ and Verdict uses enum.auto()."""

    def test_frame_has_slots(self) -> None:
        """Frame with slots=True rejects arbitrary attribute assignment."""
        f = _frame(common_pb2.FRAME_TYPE_PING)
        with pytest.raises(AttributeError):
            f.unknown_attribute = "x"  # type: ignore[attr-defined]

    def test_verdict_members_are_distinct(self) -> None:
        """All three Verdict members are unequal to each other."""
        members = list(frame.Verdict)
        assert len(members) == 3
        assert len(set(members)) == len(members)

    def test_verdict_ok_is_not_a_string(self) -> None:
        """Verdict.OK is not a string (migrated from string-valued enum)."""
        assert not isinstance(frame.Verdict.OK.value, str)


# ---------------------------------------------------------------------------
# _recv_exact
# ---------------------------------------------------------------------------


class TestRecvExact:
    """Unit tests for _recv_exact internals."""

    def test_identity_single_read(self) -> None:
        """Whole payload in one recv call: returned object is the same bytes object."""
        payload = b"hello_world"
        mock_sock = _sock(payload)
        result = frame._recv_exact(mock_sock, len(payload), "payload")
        assert result is payload

    def test_pieces_joined_correctly(self) -> None:
        """Data arriving in multiple pieces is assembled into a single bytes object."""
        data = b"hello_world_test"
        mock_sock = _sock(data[:5], data[5:10], data[10:])
        result = frame._recv_exact(mock_sock, len(data), "payload")
        assert result == data

    def test_no_recv_call_exceeds_cap(self) -> None:
        """Every recv call requests at most _RECV_CAP bytes."""
        n = frame._RECV_CAP + 100
        mock_sock = _sock(b"x" * n)
        frame._recv_exact(mock_sock, n, "payload")
        for call in mock_sock.recv.call_args_list:
            assert call.args[0] <= frame._RECV_CAP

    def test_loop_requests_remaining_not_total_bytes(self) -> None:
        """Each recv in the loop asks for n-got (remaining) bytes, not n."""
        chunk = 100
        data = b"a" * chunk + b"b" * chunk + b"c" * chunk + b"d" * chunk
        n = len(data)  # 400
        mock_sock = _sock(
            data[:chunk],
            data[chunk : chunk * 2],
            data[chunk * 2 : chunk * 3],
            data[chunk * 3 :],
        )
        result = frame._recv_exact(mock_sock, n, "payload")
        assert result == data
        calls = [call.args[0] for call in mock_sock.recv.call_args_list]
        expected = [min(n - i * chunk, frame._RECV_CAP) for i in range(4)]
        assert calls == expected, f"expected {expected}, got {calls}"

    def test_zero_bytes_makes_no_recv_call(self) -> None:
        """_recv_exact(n=0) returns b'' without calling recv."""
        mock_sock = MagicMock(spec=socket.socket)
        result = frame._recv_exact(mock_sock, 0, "header")
        assert result == b""
        mock_sock.recv.assert_not_called()

    def test_eof_mid_header_message_contains_mid_frame_and_counts(self) -> None:
        """EOF partway through the header: message says 'mid-frame' with byte counts."""
        mock_sock = _sock(b"x" * 5)  # 5 of 12 header bytes
        with pytest.raises(TazConnectionLost) as exc_info:
            frame._recv_exact(mock_sock, frame.HEADER_SIZE, "header")
        msg = str(exc_info.value)
        assert "mid-frame" in msg
        assert "5 of 12" in msg
        assert "header" in msg

    def test_eof_mid_payload_message_contains_mid_frame_and_counts(self) -> None:
        """EOF mid-payload: message says 'mid-frame' with byte counts."""
        mock_sock = _sock(b"x" * 4)  # 4 of 10 payload bytes
        with pytest.raises(TazConnectionLost) as exc_info:
            frame._recv_exact(mock_sock, 10, "payload")
        msg = str(exc_info.value)
        assert "mid-frame" in msg
        assert "4 of 10" in msg
        assert "payload" in msg

    def test_timeout_mid_payload_chains_timeout_error(self) -> None:
        """Timeout mid-payload raises TazConnectionLost chaining TimeoutError."""
        timeout_err = TimeoutError("timed out")
        mock_sock = _sock(b"x" * 4, timeout_err)  # 4 bytes then timeout
        with pytest.raises(TazConnectionLost) as exc_info:
            frame._recv_exact(mock_sock, 10, "payload")
        msg = str(exc_info.value)
        assert "mid-frame" in msg
        assert isinstance(exc_info.value.__cause__, TimeoutError)

    @pytest.mark.skipif(sys.platform != "linux", reason="RSS check is Linux-only")
    def test_no_prealloc_for_large_claimed_payload(self) -> None:
        """Claimed payload far exceeds what arrives: RSS growth stays small."""
        if sys.platform != "linux":
            return  # unreachable at runtime; gives mypy the platform narrowing
        import resource

        size = 64 * 1024 * 1024  # 64 MiB
        # Raise RESPONSE limit so validate_header passes; unknown type bound follows.
        limits = {**frame.DEFAULT_MAX_PAYLOAD, common_pb2.FRAME_TYPE_RESPONSE: size + 1}
        hdr = frame.pack_header(
            frame.Frame(type=0x09, flags=0, opcode=0, length=size, stream_id=0)
        )
        a, b = socket.socketpair()
        a_closed = False
        try:
            rss_before = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
            a.sendall(hdr + b"x" * 10)
            a.close()
            a_closed = True
            with pytest.raises(TazConnectionLost):
                frame.recv_frame(b, limits)
            rss_after = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
            # ru_maxrss is in KB on Linux; < 8 MiB means growth < 8192 KB
            assert rss_after - rss_before < 8 * 1024
        finally:
            if not a_closed:
                a.close()
            b.close()


# ---------------------------------------------------------------------------
# wait_readable
# ---------------------------------------------------------------------------


class TestWaitReadable:
    """Tests for wait_readable using real socketpairs."""

    def test_no_data_returns_false_and_restores_timeout(self) -> None:
        """No data within timeout: returns False; original socket timeout restored."""
        a, b = socket.socketpair()
        try:
            original = b.gettimeout()
            result = frame.wait_readable(b, 0.05)
            assert not result
            assert b.gettimeout() == original
        finally:
            a.close()
            b.close()

    def test_data_returns_true_and_byte_still_readable(self) -> None:
        """Data available: returns True and the byte is not consumed (MSG_PEEK)."""
        a, b = socket.socketpair()
        try:
            a.sendall(b"z")
            assert frame.wait_readable(b, 1.0)
            assert b.recv(1) == b"z"  # byte not consumed
        finally:
            a.close()
            b.close()

    def test_eof_raises_connection_lost(self) -> None:
        """Peer closes connection: wait_readable raises TazConnectionLost."""
        a, b = socket.socketpair()
        try:
            a.close()
            with pytest.raises(TazConnectionLost):
                frame.wait_readable(b, 1.0)
        finally:
            b.close()

    def test_none_timeout_with_data_returns_true(self) -> None:
        """timeout=None with data already present: returns True (blocks until data)."""
        a, b = socket.socketpair()
        try:
            a.sendall(b"q")
            assert frame.wait_readable(b, None)
        finally:
            a.close()
            b.close()


# ---------------------------------------------------------------------------
# Unknown-type bound
# ---------------------------------------------------------------------------


def test_default_unknown_type_max_payload_is_65536() -> None:
    """DEFAULT_UNKNOWN_TYPE_MAX_PAYLOAD equals 65536 (§10.1, pinned by spec)."""
    assert frame.DEFAULT_UNKNOWN_TYPE_MAX_PAYLOAD == 65536
