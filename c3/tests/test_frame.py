"""Tests for the Python framing layer: header pack/unpack, validation, send/recv."""

from __future__ import annotations

import socket
import struct
from unittest.mock import MagicMock

import pytest
from taz.c3.errors import TazError, TazProtocolError
from taz.c3.protocol import frame
from taz.v1 import common_pb2

# ---------------------------------------------------------------------------
# Pack / unpack
# ---------------------------------------------------------------------------


class TestPackUnpack:
    """Test header pack and unpack operations."""

    def test_pack_unpack_roundtrip(self) -> None:
        """Verify a frame round-trips through pack/unpack unchanged."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_REQUEST,
            flags=0xAB,
            opcode=0x1234,
            length=0x00ABCDEF,
            stream_id=0xDEADBEEF,
        )
        packed = frame.pack_header(f)
        unpacked = frame.unpack_header(packed)

        assert unpacked.type == f.type
        assert unpacked.flags == f.flags
        assert unpacked.opcode == f.opcode
        assert unpacked.length == f.length
        assert unpacked.stream_id == f.stream_id
        assert unpacked.payload == b""

    def test_wire_layout_little_endian(self) -> None:
        """Verify each multi-byte field is stored little-endian."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_RESPONSE,
            flags=0x01,
            opcode=0x1234,  # lo=0x34, hi=0x12
            length=0x01020304,  # bytes: 04 03 02 01
            stream_id=0xAABBCCDD,  # bytes: DD CC BB AA
        )
        packed = frame.pack_header(f)

        assert packed[0] == common_pb2.FRAME_TYPE_RESPONSE
        assert packed[1] == 0x01

        # opcode LE
        assert packed[2] == 0x34
        assert packed[3] == 0x12

        # length LE
        assert packed[4] == 0x04
        assert packed[5] == 0x03
        assert packed[6] == 0x02
        assert packed[7] == 0x01

        # stream_id LE
        assert packed[8] == 0xDD
        assert packed[9] == 0xCC
        assert packed[10] == 0xBB
        assert packed[11] == 0xAA

    def test_header_size_is_12_bytes(self) -> None:
        """Verify HEADER_SIZE is exactly 12 bytes."""
        assert frame.HEADER_SIZE == 12

    def test_opcode_edge_values_roundtrip(self) -> None:
        """Verify opcode boundary values survive pack/unpack."""
        # Test 0xFFFF
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_REQUEST,
            flags=0,
            opcode=0xFFFF,
            length=0,
            stream_id=0,
        )
        packed = frame.pack_header(f)
        unpacked = frame.unpack_header(packed)
        assert unpacked.opcode == 0xFFFF

        # Test 0x0000
        f.opcode = 0x0000
        packed = frame.pack_header(f)
        unpacked = frame.unpack_header(packed)
        assert unpacked.opcode == 0x0000

        # Test mid-range value 0x0134
        f.opcode = 0x0134
        packed = frame.pack_header(f)
        unpacked = frame.unpack_header(packed)
        assert unpacked.opcode == 0x0134

    def test_stream_id_boundary_values_roundtrip(self) -> None:
        """Verify stream_id boundary values survive pack/unpack."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_REQUEST,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
        )

        # Test 0
        packed = frame.pack_header(f)
        unpacked = frame.unpack_header(packed)
        assert unpacked.stream_id == 0

        # Test 0xFFFFFFFF
        f.stream_id = 0xFFFFFFFF
        packed = frame.pack_header(f)
        unpacked = frame.unpack_header(packed)
        assert unpacked.stream_id == 0xFFFFFFFF

        # Test mid-range value
        f.stream_id = 0x12345678
        packed = frame.pack_header(f)
        unpacked = frame.unpack_header(packed)
        assert unpacked.stream_id == 0x12345678

    def test_unpack_preserves_payload(self) -> None:
        """Verify unpacking returns the Frame with empty payload initially."""
        data = b"\x01\x00\x34\x12\x04\x03\x02\x01\xdd\xcc\xbb\xaa"
        f = frame.unpack_header(data)
        assert f.payload == b""


# ---------------------------------------------------------------------------
# Max-payload table (§6)
# ---------------------------------------------------------------------------


class TestMaxPayloadTable:
    """Test the default max payload limits."""

    def test_ping_pong_max_payload_is_zero(self) -> None:
        """PING and PONG have 0 max payload."""
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_PING] == 0
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_PONG] == 0

    def test_capability_max_payload_is_1024(self) -> None:
        """CAPABILITY has 1024 max payload."""
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_CAPABILITY] == 1024

    def test_error_max_payload_is_4096(self) -> None:
        """ERROR has 4096 max payload."""
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_ERROR] == 4096

    def test_request_response_file_chunk_max_payload_is_65536(self) -> None:
        """REQUEST, RESPONSE, FILE_CHUNK have 65536 max payload."""
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST] == 65536
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_RESPONSE] == 65536
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_FILE_CHUNK] == 65536

    def test_all_known_types_in_default_table(self) -> None:
        """All known frame types are in the default max payload table."""
        known_types = [
            common_pb2.FRAME_TYPE_PING,
            common_pb2.FRAME_TYPE_PONG,
            common_pb2.FRAME_TYPE_CAPABILITY,
            common_pb2.FRAME_TYPE_ERROR,
            common_pb2.FRAME_TYPE_REQUEST,
            common_pb2.FRAME_TYPE_RESPONSE,
            common_pb2.FRAME_TYPE_FILE_CHUNK,
        ]
        for ftype in known_types:
            assert ftype in frame.DEFAULT_MAX_PAYLOAD


# ---------------------------------------------------------------------------
# Validate header
# ---------------------------------------------------------------------------


class TestValidateHeader:
    """Test header validation logic."""

    def test_validate_known_types_at_limit_is_ok(self) -> None:
        """Known types at their limit pass validation."""
        known_types = [
            common_pb2.FRAME_TYPE_REQUEST,
            common_pb2.FRAME_TYPE_RESPONSE,
            common_pb2.FRAME_TYPE_FILE_CHUNK,
            common_pb2.FRAME_TYPE_ERROR,
            common_pb2.FRAME_TYPE_CAPABILITY,
        ]
        for ftype in known_types:
            f = frame.Frame(
                type=ftype,
                flags=0,
                opcode=0,
                length=frame.DEFAULT_MAX_PAYLOAD[ftype],
                stream_id=0,
            )
            verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
            assert verdict == frame.Verdict.OK, f"type={ftype} failed"

    def test_validate_ping_pong_zero_length_is_ok(self) -> None:
        """PING and PONG with zero length pass validation."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_PING,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OK

        f.type = common_pb2.FRAME_TYPE_PONG
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OK

    def test_validate_ping_with_payload_is_oversized(self) -> None:
        """PING with any payload fails validation."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_PING,
            flags=0,
            opcode=0,
            length=1,
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OVERSIZED

    def test_validate_capability_oversized(self) -> None:
        """CAPABILITY exceeding its limit fails validation."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_CAPABILITY,
            flags=0,
            opcode=0,
            length=frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_CAPABILITY] + 1,
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OVERSIZED

    def test_validate_request_oversized(self) -> None:
        """REQUEST exceeding its limit fails validation."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_REQUEST,
            flags=0,
            opcode=0,
            length=frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST] + 1,
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OVERSIZED

    def test_validate_unknown_type_within_bound_is_unknown_type(self) -> None:
        """Unknown type within the default bound returns UNKNOWN_TYPE."""
        # Type 0x00 (UNSPECIFIED) within bound
        f = frame.Frame(
            type=0x00,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.UNKNOWN_TYPE

        # Type 0x08 (reserved) within bound
        f.type = 0x08
        f.length = 100
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.UNKNOWN_TYPE

    def test_validate_unknown_type_oversized(self) -> None:
        """Unknown type exceeding the max bound fails as OVERSIZED."""
        f = frame.Frame(
            type=0x08,
            flags=0,
            opcode=0,
            length=max(frame.DEFAULT_MAX_PAYLOAD.values()) + 1,
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OVERSIZED

    def test_validate_unknown_type_at_exact_bound_is_unknown_type(self) -> None:
        """Unknown type at exactly the max bound returns UNKNOWN_TYPE."""
        f = frame.Frame(
            type=0x09,
            flags=0,
            opcode=0,
            length=max(frame.DEFAULT_MAX_PAYLOAD.values()),
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.UNKNOWN_TYPE

    def test_validate_ping_nonzero_opcode_is_ok(self) -> None:
        """PING validation ignores the opcode field."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_PING,
            flags=0,
            opcode=0xFFFF,
            length=0,
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OK

    def test_validate_all_flag_bits_set_is_ok(self) -> None:
        """All flag bits set is valid."""
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_REQUEST,
            flags=0xFF,
            opcode=0,
            length=0,
            stream_id=0,
        )
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
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_REQUEST,
            flags=0,
            opcode=1,
            length=0,  # Will be set by send_frame
            stream_id=1,
            payload=b"test_payload",
        )

        frame.send_frame(mock_sock, f)

        # Verify length was set
        assert f.length == len(b"test_payload")

        # Verify sendall was called with header + payload
        assert mock_sock.sendall.called
        sent_data = mock_sock.sendall.call_args[0][0]
        assert len(sent_data) == frame.HEADER_SIZE + len(b"test_payload")
        assert sent_data.endswith(b"test_payload")

    def test_send_frame_with_empty_payload(self) -> None:
        """send_frame handles empty payload."""
        mock_sock = MagicMock(spec=socket.socket)
        f = frame.Frame(
            type=common_pb2.FRAME_TYPE_PING,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
            payload=b"",
        )

        frame.send_frame(mock_sock, f)

        sent_data = mock_sock.sendall.call_args[0][0]
        assert len(sent_data) == frame.HEADER_SIZE
        assert f.length == 0


class TestRecvFrame:
    """Test recv_frame operation."""

    def test_recv_frame_basic(self) -> None:
        """recv_frame reads header and payload correctly."""
        # Create a valid frame: PING (type=5), zero length
        header = struct.pack(
            "<BBHII",
            common_pb2.FRAME_TYPE_PING,
            0,  # flags
            0,  # opcode
            0,  # length
            0,  # stream_id
        )

        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.recv_into.side_effect = [
            frame.HEADER_SIZE,  # recv_into writes HEADER_SIZE bytes into view
        ]

        # Simulate recv_into filling the buffer
        def recv_into_impl(buf: bytearray, n: int) -> int:
            buf[: len(header)] = header
            return len(header)

        mock_sock.recv_into.side_effect = recv_into_impl

        f = frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert f.type == common_pb2.FRAME_TYPE_PING
        assert f.length == 0
        assert f.payload == b""

    def test_recv_frame_with_payload(self) -> None:
        """recv_frame receives header and payload correctly."""
        payload = b"hello"
        header = struct.pack(
            "<BBHII",
            common_pb2.FRAME_TYPE_REQUEST,
            0,  # flags
            1,  # opcode
            len(payload),
            42,  # stream_id
        )

        mock_sock = MagicMock(spec=socket.socket)

        # First recv_into gets the header, second gets the payload
        def recv_into_impl(buf: bytearray, n: int) -> int:
            if len(buf) == frame.HEADER_SIZE:
                buf[: len(header)] = header
                return len(header)
            else:
                buf[: len(payload)] = payload
                return len(payload)

        mock_sock.recv_into.side_effect = recv_into_impl

        f = frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert f.type == common_pb2.FRAME_TYPE_REQUEST
        assert f.opcode == 1
        assert f.stream_id == 42
        assert f.length == len(payload)
        assert f.payload == payload

    def test_recv_frame_oversized_raises_protocol_error(self) -> None:
        """recv_frame raises TazProtocolError for oversized frame."""
        # REQUEST with length > 65536
        header = struct.pack(
            "<BBHII",
            common_pb2.FRAME_TYPE_REQUEST,
            0,  # flags
            0,  # opcode
            65537,  # length > limit
            0,  # stream_id
        )

        mock_sock = MagicMock(spec=socket.socket)

        def recv_into_impl(buf: bytearray, n: int) -> int:
            buf[: len(header)] = header
            return len(header)

        mock_sock.recv_into.side_effect = recv_into_impl

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
        mock_sock = MagicMock(spec=socket.socket)
        # recv_into returns 0 to indicate EOF
        mock_sock.recv_into.return_value = 0

        with pytest.raises(TazError) as exc_info:
            frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert "closed by peer" in str(exc_info.value)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CONNECTION_LOST

    def test_recv_frame_custom_limits(self) -> None:
        """recv_frame respects custom max payload limits."""
        # Custom limit: REQUEST max is 1024 instead of 65536
        limits = {
            common_pb2.FRAME_TYPE_REQUEST: 1024,
        }

        header = struct.pack(
            "<BBHII",
            common_pb2.FRAME_TYPE_REQUEST,
            0,  # flags
            0,  # opcode
            2048,  # length > custom limit
            0,  # stream_id
        )

        mock_sock = MagicMock(spec=socket.socket)

        def recv_into_impl(buf: bytearray, n: int) -> int:
            buf[: len(header)] = header
            return len(header)

        mock_sock.recv_into.side_effect = recv_into_impl

        with pytest.raises(TazProtocolError) as exc_info:
            frame.recv_frame(mock_sock, limits)  # type: ignore[arg-type]

        assert "oversized" in str(exc_info.value)

    def test_recv_frame_partial_header_reassembly(self) -> None:
        """recv_frame reassembles a header that arrives in multiple partial reads."""
        payload = b"partial_test"
        header = struct.pack(
            "<BBHII",
            common_pb2.FRAME_TYPE_REQUEST,
            0,  # flags
            5,  # opcode
            len(payload),
            99,  # stream_id
        )

        # Wire data arrives in 3 chunks: first 4 bytes, last 8 bytes of header, payload.
        chunks = [header[:4], header[4:], payload]
        chunk_iter = iter(chunks)

        def recv_into_partial(buf: bytearray, n: int) -> int:
            data = next(chunk_iter)
            to_write = min(len(data), n)
            buf[:to_write] = data[:to_write]
            return to_write

        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.recv_into.side_effect = recv_into_partial

        f = frame.recv_frame(mock_sock, frame.DEFAULT_MAX_PAYLOAD)

        assert f.type == common_pb2.FRAME_TYPE_REQUEST
        assert f.opcode == 5
        assert f.stream_id == 99
        assert f.payload == payload

    def test_recv_frame_concatenated_frames_boundary(self) -> None:
        """recv_frame reads exactly one frame when two frames arrive back-to-back."""
        payload1 = b"first_frame_payload"
        payload2 = b"second_frame_payload"

        header1 = struct.pack(
            "<BBHII",
            common_pb2.FRAME_TYPE_REQUEST,
            0,
            1,
            len(payload1),
            1,
        )
        header2 = struct.pack(
            "<BBHII",
            common_pb2.FRAME_TYPE_RESPONSE,
            0,
            2,
            len(payload2),
            2,
        )

        wire_data = header1 + payload1 + header2 + payload2
        pos = 0

        def recv_into_stream(buf: bytearray, n: int) -> int:
            nonlocal pos
            to_read = min(n, len(wire_data) - pos)
            if to_read == 0:
                return 0
            buf[:to_read] = wire_data[pos : pos + to_read]
            pos += to_read
            return to_read

        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.recv_into.side_effect = recv_into_stream

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

        response_frame = frame.Frame(
            type=common_pb2.FRAME_TYPE_RESPONSE,
            flags=0,
            opcode=1,
            length=payload_size,
            stream_id=1,
        )
        # Confirm default limits reject this size.
        assert (
            frame.validate_header(response_frame, frame.DEFAULT_MAX_PAYLOAD)
            == frame.Verdict.OVERSIZED
        )

        raised_limits = dict(frame.DEFAULT_MAX_PAYLOAD)
        raised_limits[common_pb2.FRAME_TYPE_RESPONSE] = 128 * 1024  # 128 KiB

        header = struct.pack(
            "<BBHII",
            common_pb2.FRAME_TYPE_RESPONSE,
            0,
            1,
            payload_size,
            1,
        )
        wire_data = header + payload
        pos = 0

        def recv_into_impl(buf: bytearray, n: int) -> int:
            nonlocal pos
            to_read = min(n, len(wire_data) - pos)
            buf[:to_read] = wire_data[pos : pos + to_read]
            pos += to_read
            return to_read

        mock_sock = MagicMock(spec=socket.socket)
        mock_sock.recv_into.side_effect = recv_into_impl

        f = frame.recv_frame(mock_sock, raised_limits)

        assert f.type == common_pb2.FRAME_TYPE_RESPONSE
        assert f.length == payload_size
        assert f.payload == payload


# ---------------------------------------------------------------------------
# Verdict enum
# ---------------------------------------------------------------------------


class TestVerdict:
    """Test Verdict enum."""

    def test_verdict_has_required_values(self) -> None:
        """Verdict enum has required values."""
        assert hasattr(frame.Verdict, "OK")
        assert hasattr(frame.Verdict, "UNKNOWN_TYPE")
        assert hasattr(frame.Verdict, "OVERSIZED")

    def test_verdict_values_are_strings(self) -> None:
        """Verdict enum values match expected strings."""
        assert frame.Verdict.OK.value == "ok"
        assert frame.Verdict.UNKNOWN_TYPE.value == "unknown_type"
        assert frame.Verdict.OVERSIZED.value == "oversized"


# ---------------------------------------------------------------------------
# Frame dataclass
# ---------------------------------------------------------------------------


class TestFrameDataclass:
    """Test Frame dataclass."""

    def test_frame_construction_minimal(self) -> None:
        """Frame can be constructed with required fields."""
        f = frame.Frame(
            type=1,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
        )
        assert f.type == 1
        assert f.flags == 0
        assert f.opcode == 0
        assert f.length == 0
        assert f.stream_id == 0
        assert f.payload == b""

    def test_frame_construction_with_payload(self) -> None:
        """Frame can be constructed with payload."""
        payload = b"data"
        f = frame.Frame(
            type=1,
            flags=0,
            opcode=0,
            length=4,
            stream_id=0,
            payload=payload,
        )
        assert f.payload == payload

    def test_frame_payload_defaults_to_empty_bytes(self) -> None:
        """Frame.payload defaults to empty bytes."""
        f = frame.Frame(
            type=1,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
        )
        assert f.payload == b""
        assert isinstance(f.payload, bytes)


# ---------------------------------------------------------------------------
# Integration tests (spec compliance)
# ---------------------------------------------------------------------------


class TestSpecCompliance:
    """Test compliance with protocol.md specifications."""

    def test_roundtrip_preserves_all_header_fields(self) -> None:
        """Spec §4: roundtrip preserves all 12 header bytes."""
        test_cases = [
            frame.Frame(
                type=common_pb2.FRAME_TYPE_PING,
                flags=0x00,
                opcode=0x0000,
                length=0,
                stream_id=0,
            ),
            frame.Frame(
                type=common_pb2.FRAME_TYPE_REQUEST,
                flags=0xFF,
                opcode=0xFFFF,
                length=0xFFFFFFFF,
                stream_id=0xFFFFFFFF,
            ),
            frame.Frame(
                type=0x08,  # reserved
                flags=0x55,
                opcode=0x1234,
                length=0x56789ABC,
                stream_id=0xDEADBEEF,
            ),
        ]

        for original in test_cases:
            packed = frame.pack_header(original)
            unpacked = frame.unpack_header(packed)

            assert unpacked.type == original.type
            assert unpacked.flags == original.flags
            assert unpacked.opcode == original.opcode
            assert unpacked.length == original.length
            assert unpacked.stream_id == original.stream_id

    def test_spec_section_6_payload_limits(self) -> None:
        """Spec §6: payload limits match defaults."""
        # These are the recommended defaults from protocol.md §6
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_PING] == 0
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_PONG] == 0
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_CAPABILITY] == 1024
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST] == 65536
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_RESPONSE] == 65536
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_ERROR] == 4096
        assert frame.DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_FILE_CHUNK] == 65536

    def test_spec_section_10_1_unknown_type_handling(self) -> None:
        """Spec §10.1: unknown types apply the 64 KiB bound."""
        # Unknown type at 64 KiB is UNKNOWN_TYPE
        f = frame.Frame(
            type=0xFF,  # unknown
            flags=0,
            opcode=0,
            length=65536,
            stream_id=0,
        )
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.UNKNOWN_TYPE

        # Unknown type exceeding 64 KiB is OVERSIZED
        f.length = 65537
        verdict = frame.validate_header(f, frame.DEFAULT_MAX_PAYLOAD)
        assert verdict == frame.Verdict.OVERSIZED
