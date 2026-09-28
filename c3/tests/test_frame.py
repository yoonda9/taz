"""Tests for the Python framing layer: header pack/unpack, validation, send/recv."""

from __future__ import annotations

import socket
from unittest.mock import MagicMock

import pytest
from taz.c3.errors import TazError, TazProtocolError
from taz.c3.protocol import frame
from taz.v1 import common_pb2

_UNKNOWN_BOUND = max(frame.DEFAULT_MAX_PAYLOAD.values())


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
    """Mock socket whose recv_into serves ``chunks`` one read at a time, then EOF.

    An exception in ``chunks`` is raised by the read that reaches it. Arguments
    are checked as socket.recv_into checks them.
    """
    pending = list(chunks)

    def recv_into(buf: memoryview, nbytes: int = 0, flags: int = 0) -> int:
        if nbytes < 0:
            raise ValueError("negative buffersize in recv_into")
        if nbytes > len(buf):
            raise ValueError("buffer too small for requested bytes")
        if not pending:
            return 0
        chunk = pending.pop(0)
        if isinstance(chunk, OSError):
            raise chunk
        size = min(nbytes or len(buf), len(chunk))
        buf[:size] = chunk[:size]
        if size < len(chunk):
            pending.insert(0, chunk[size:])
        return size

    sock = MagicMock(spec=socket.socket)
    sock.recv_into.side_effect = recv_into
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

    @pytest.mark.parametrize("opcode", [0xFFFF, 0x0000, 0x0134])
    def test_opcode_edge_values_roundtrip(self, opcode: int) -> None:
        """Verify opcode boundary values survive pack/unpack."""
        f = _frame(common_pb2.FRAME_TYPE_REQUEST, opcode=opcode)
        assert frame.unpack_header(frame.pack_header(f)).opcode == opcode

    @pytest.mark.parametrize("stream_id", [0, 0xFFFFFFFF, 0x12345678])
    def test_stream_id_boundary_values_roundtrip(self, stream_id: int) -> None:
        """Verify stream_id boundary values survive pack/unpack."""
        f = _frame(common_pb2.FRAME_TYPE_REQUEST, stream_id=stream_id)
        assert frame.unpack_header(frame.pack_header(f)).stream_id == stream_id


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

    def test_send_frame_sets_length_and_sends_all_data(self) -> None:
        """send_frame sets length from payload and sends header+payload."""
        mock_sock = MagicMock(spec=socket.socket)
        f = _frame(common_pb2.FRAME_TYPE_REQUEST, opcode=1, stream_id=1)
        f.payload = b"test_payload"

        frame.send_frame(mock_sock, f)

        assert f.length == len(b"test_payload")
        mock_sock.sendall.assert_called_once_with(
            frame.pack_header(f) + b"test_payload"
        )

    def test_send_frame_with_empty_payload(self) -> None:
        """send_frame handles empty payload."""
        mock_sock = MagicMock(spec=socket.socket)
        f = _frame(common_pb2.FRAME_TYPE_PING)

        frame.send_frame(mock_sock, f)

        assert f.length == 0
        mock_sock.sendall.assert_called_once_with(frame.pack_header(f))

    @pytest.mark.parametrize(
        "error", [BrokenPipeError(32, "Broken pipe"), TimeoutError("timed out")]
    )
    def test_send_frame_socket_error_raises_connection_lost(
        self, error: OSError
    ) -> None:
        """A failed or timed-out sendall surfaces as TazError(CONNECTION_LOST)."""
        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.sendall.side_effect = error

        with pytest.raises(TazError) as exc_info:
            frame.send_frame(mock_sock, _frame(common_pb2.FRAME_TYPE_PING))

        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST
        assert exc_info.value.__cause__ is error


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

    def test_recv_frame_oversized_raises_protocol_error(self) -> None:
        """recv_frame raises TazProtocolError for oversized frame."""
        mock_sock = _sock(
            frame.pack_header(_frame(common_pb2.FRAME_TYPE_REQUEST, 65537))
        )

        with pytest.raises(TazProtocolError) as exc_info:
            frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert "oversized" in str(exc_info.value)

    def test_recv_frame_connection_timeout_raises_taz_error(self) -> None:
        """recv_frame raises TazError on socket timeout."""
        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.recv_into.side_effect = TimeoutError("socket timeout")

        with pytest.raises(TazError) as exc_info:
            frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert "timed out" in str(exc_info.value)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST

    def test_recv_frame_connection_closed_raises_taz_error(self) -> None:
        """recv_frame raises TazError when connection closes."""
        with pytest.raises(TazError) as exc_info:
            frame.recv_frame(_sock(), frame.DEFAULT_MAX_PAYLOAD)

        assert "closed by peer" in str(exc_info.value)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST

    @pytest.mark.parametrize(
        "received",
        [5, frame.HEADER_SIZE + 4],
        ids=["mid-header", "mid-payload"],
    )
    def test_recv_frame_eof_mid_frame_raises_taz_error(self, received: int) -> None:
        """EOF partway through a frame is CONNECTION_LOST, never a short frame."""
        payload = b"0123456789"
        header = frame.pack_header(_frame(common_pb2.FRAME_TYPE_REQUEST, len(payload)))
        mock_sock = _sock((header + payload)[:received])

        with pytest.raises(TazError) as exc_info:
            frame.recv_frame(mock_sock)

        assert "closed by peer" in str(exc_info.value)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST

    def test_recv_frame_connection_reset_raises_taz_error(self) -> None:
        """A reset mid-frame (e.g. the daemon closing after an ERROR, §10.1)
        surfaces as TazError(CONNECTION_LOST), not a raw OSError."""
        reset = ConnectionResetError(104, "Connection reset by peer")
        header = frame.pack_header(_frame(common_pb2.FRAME_TYPE_RESPONSE, 8))
        mock_sock = _sock(header, b"1234", reset)

        with pytest.raises(TazError) as exc_info:
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

        assert "oversized" in str(exc_info.value)

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
