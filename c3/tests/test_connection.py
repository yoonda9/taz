"""Tests for Connection: CAPABILITY parse, protocol_major gate, merged limits."""

from __future__ import annotations

import socket
from unittest.mock import MagicMock, patch

import pytest
from taz.c3.connection import Connection
from taz.c3.errors import TazConnectionLost, TazError, TazProtocolError
from taz.c3.protocol.frame import DEFAULT_MAX_PAYLOAD, Frame, pack_header
from taz.v1 import common_pb2, daemon_control_pb2


def _capability_bytes(
    protocol_major: int = 1,
    protocol_minor: int = 0,
    operations: list[int] | None = None,
    max_payload_sizes: dict[int, int] | None = None,
) -> bytes:
    cap = daemon_control_pb2.CapabilityPayload(
        protocol_major=protocol_major,
        protocol_minor=protocol_minor,
        operations=operations or [],
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


def _frame_bytes(
    type_: int,
    payload: bytes = b"",
    *,
    opcode: int = 0,
    stream_id: int = 0,
    flags: int = 0,
) -> bytes:
    header = pack_header(
        Frame(
            type=type_,
            flags=flags,
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


def _connect(conn: Connection, *chunks: bytes) -> None:
    """Connect ``conn`` using a mock socket serving ``chunks``."""
    mock_sock = _mock_sock(*chunks)
    with patch("taz.c3.connection.socket.create_connection", return_value=mock_sock):
        conn.connect("127.0.0.1", 5555)


class TestCapabilityParsed:
    def test_operations_stored(self) -> None:
        ops: list[int] = [common_pb2.OPCODE_PING, common_pb2.OPCODE_VERSION]
        conn = Connection()
        _connect(conn, _capability_bytes(operations=ops))
        assert list(conn.capabilities.operations) == ops
        assert not conn.closed

    def test_minor_version_kept(self) -> None:
        conn = Connection()
        _connect(conn, _capability_bytes(protocol_minor=7))
        assert conn.capabilities.protocol_minor == 7

    def test_default_limits_intact_without_override(self) -> None:
        conn = Connection()
        _connect(conn, _capability_bytes())
        default = DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_RESPONSE]
        assert conn.limits[common_pb2.FRAME_TYPE_RESPONSE] == default


class TestProtocolMajorGate:
    def test_wrong_major_raises_protocol_error(self) -> None:
        conn = Connection()
        with pytest.raises(TazProtocolError):
            _connect(conn, _capability_bytes(protocol_major=99))

    def test_wrong_major_closes_connection(self) -> None:
        conn = Connection()
        with pytest.raises(TazProtocolError):
            _connect(conn, _capability_bytes(protocol_major=0))
        assert conn.closed

    def test_correct_major_succeeds(self) -> None:
        conn = Connection()
        _connect(conn, _capability_bytes(protocol_major=1))
        assert not conn.closed


class TestMaxPayloadSizesOverride:
    def test_raised_response_limit_allows_larger_recv(self) -> None:
        large_limit = DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_RESPONSE] * 2
        cap_data = _capability_bytes(
            max_payload_sizes={common_pb2.FRAME_TYPE_RESPONSE: large_limit}
        )
        large_payload = b"x" * (DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_RESPONSE] + 1)
        response_data = _frame_bytes(
            common_pb2.FRAME_TYPE_RESPONSE,
            large_payload,
            opcode=common_pb2.OPCODE_PING,
            stream_id=1,
        )
        mock_sock = _mock_sock(cap_data, response_data)
        conn = Connection()
        with patch(
            "taz.c3.connection.socket.create_connection", return_value=mock_sock
        ):
            conn.connect("127.0.0.1", 5555)
        frame = conn.recv_frame()
        assert frame.payload == large_payload

    def test_override_is_reflected_in_limits(self) -> None:
        new_limit = 8192
        conn = Connection()
        _connect(
            conn,
            _capability_bytes(
                max_payload_sizes={common_pb2.FRAME_TYPE_REQUEST: new_limit}
            ),
        )
        assert conn.limits[common_pb2.FRAME_TYPE_REQUEST] == new_limit

    def test_unknown_frame_type_in_max_payload_sizes_is_ignored(self) -> None:
        conn = Connection()
        _connect(conn, _capability_bytes(max_payload_sizes={0xFF: 9999}))
        assert 0xFF not in conn.limits


class TestNonCapabilityFirstFrame:
    def test_ping_first_raises_protocol_error(self) -> None:
        ping = _frame_bytes(common_pb2.FRAME_TYPE_PING)
        conn = Connection()
        with pytest.raises(TazProtocolError):
            _connect(conn, ping)

    def test_request_first_raises_protocol_error(self) -> None:
        req = _frame_bytes(common_pb2.FRAME_TYPE_REQUEST, b"\x00" * 4, stream_id=1)
        conn = Connection()
        with pytest.raises(TazProtocolError):
            _connect(conn, req)

    def test_non_capability_closes_connection(self) -> None:
        ping = _frame_bytes(common_pb2.FRAME_TYPE_PING)
        conn = Connection()
        with pytest.raises(TazProtocolError):
            _connect(conn, ping)
        assert conn.closed


class TestConnectionLifecycle:
    def test_closed_before_connect(self) -> None:
        assert Connection().closed

    def test_open_after_connect(self) -> None:
        conn = Connection()
        _connect(conn, _capability_bytes())
        assert not conn.closed

    def test_closed_after_close(self) -> None:
        conn = Connection()
        _connect(conn, _capability_bytes())
        conn.close()
        assert conn.closed

    def test_close_is_idempotent(self) -> None:
        conn = Connection()
        _connect(conn, _capability_bytes())
        conn.close()
        conn.close()
        assert conn.closed

    def test_timeout_passed_to_create_connection(self) -> None:
        mock_sock = _mock_sock(_capability_bytes())
        with patch(
            "taz.c3.connection.socket.create_connection", return_value=mock_sock
        ) as mock_create:
            conn = Connection(timeout=5.0)
            conn.connect("127.0.0.1", 5555)
        mock_create.assert_called_once_with(("127.0.0.1", 5555), timeout=5.0)


def _mock_raising_sock(cap_bytes: bytes, exc: BaseException) -> MagicMock:
    """Mock socket that serves cap_bytes then raises exc on the next recv."""
    cap_data = bytearray(cap_bytes)
    raised = False

    def recv(bufsize: int, flags: int = 0) -> bytes:
        nonlocal raised
        if cap_data:
            if flags & socket.MSG_PEEK:
                return bytes(cap_data[:1])
            take = min(bufsize, len(cap_data))
            data = bytes(cap_data[:take])
            del cap_data[:take]
            return data
        if not raised:
            raised = True
            raise exc
        return b""

    sock = MagicMock(spec=_SOCKET_SPEC)
    sock.recv.side_effect = recv
    sock.gettimeout.return_value = 1.0
    return sock


def _mock_stall_after_n(cap_bytes: bytes, n: int, exc: BaseException) -> MagicMock:
    """Mock socket: serves cap_bytes, then serves n bytes, then raises exc."""
    all_data = bytearray(cap_bytes)
    # This mock serves cap_bytes fully, then serves the first `n` bytes of the next
    # frame (header fragment), then raises `exc`.
    state = {"served_extra": 0}

    def recv(bufsize: int, flags: int = 0) -> bytes:
        if all_data:
            if flags & socket.MSG_PEEK:
                return bytes(all_data[:1])
            take = min(bufsize, len(all_data))
            data = bytes(all_data[:take])
            del all_data[:take]
            return data
        if state["served_extra"] < n:
            remaining = n - state["served_extra"]
            take = min(bufsize, remaining)
            state["served_extra"] += take
            return b"\x00" * take
        raise exc

    sock = MagicMock(spec=_SOCKET_SPEC)
    sock.recv.side_effect = recv
    sock.gettimeout.return_value = 1.0
    return sock


class TestFailureGuard:
    def test_mid_header_stall_closes(self) -> None:
        """OSError mid-header read closes the connection."""
        exc = TimeoutError("stall")
        sock = _mock_stall_after_n(_capability_bytes(), 4, exc)
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazConnectionLost):
            conn.recv_frame()
        assert conn.closed

    def test_mid_payload_stall_closes(self) -> None:
        """OSError mid-payload read closes the connection."""
        # Build a valid PING frame header with no payload; then a RESPONSE header
        # that claims 10 bytes but the socket raises mid-payload.
        ping_header = pack_header(
            Frame(
                type=common_pb2.FRAME_TYPE_RESPONSE,
                flags=0,
                opcode=0,
                length=10,
                stream_id=1,
            )
        )
        cap = _capability_bytes()

        state = {"phase": "cap", "header_sent": False, "payload_bytes": 0}
        cap_buf = bytearray(cap)

        def recv(bufsize: int, flags: int = 0) -> bytes:
            if cap_buf:
                if flags & socket.MSG_PEEK:
                    return bytes(cap_buf[:1])
                take = min(bufsize, len(cap_buf))
                data = bytes(cap_buf[:take])
                del cap_buf[:take]
                return data
            if not state["header_sent"]:
                if flags & socket.MSG_PEEK:
                    return bytes(ping_header[:1])
                state["header_sent"] = True
                return ping_header
            # mid-payload stall
            raise TimeoutError("stall")

        sock = MagicMock(spec=_SOCKET_SPEC)
        sock.recv.side_effect = recv
        sock.gettimeout.return_value = 1.0

        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazConnectionLost):
            conn.recv_frame()
        assert conn.closed

    def test_oversized_frame_closes(self) -> None:
        """OVERSIZED verdict raises TazProtocolError and closes the connection."""
        cap = _capability_bytes()
        oversized_limit = DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_RESPONSE] + 1
        oversized_header = pack_header(
            Frame(
                type=common_pb2.FRAME_TYPE_RESPONSE,
                flags=0,
                opcode=0,
                length=oversized_limit,
                stream_id=1,
            )
        )
        conn = Connection()
        mock = _mock_sock(cap, oversized_header)
        with patch("taz.c3.connection.socket.create_connection", return_value=mock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazProtocolError):
            conn.recv_frame()
        assert conn.closed

    def test_keyboard_interrupt_closes(self) -> None:
        """KeyboardInterrupt during recv closes the connection and propagates."""
        exc = KeyboardInterrupt()
        sock = _mock_raising_sock(_capability_bytes(), exc)
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(KeyboardInterrupt):
            conn.recv_frame()
        assert conn.closed

    def test_next_call_raises_chained_after_connection_lost(self) -> None:
        """After TazConnectionLost, next recv_frame raises chained TazConnectionLost."""
        exc = TimeoutError("stall")
        sock = _mock_stall_after_n(_capability_bytes(), 4, exc)
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazConnectionLost):
            conn.recv_frame()
        with pytest.raises(TazConnectionLost) as exc_info:
            conn.recv_frame()
        assert exc_info.value.__cause__ is not None
        assert "connection closed after an earlier failure" in str(exc_info.value)

    def test_next_call_raises_chained_after_protocol_error(self) -> None:
        """After a TazProtocolError, next recv_frame raises chained TazProtocolError."""
        cap = _capability_bytes()
        oversized_header = pack_header(
            Frame(
                type=common_pb2.FRAME_TYPE_RESPONSE,
                flags=0,
                opcode=0,
                length=DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_RESPONSE] + 1,
                stream_id=1,
            )
        )
        conn = Connection()
        with patch(
            "taz.c3.connection.socket.create_connection",
            return_value=_mock_sock(cap, oversized_header),
        ):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazProtocolError):
            conn.recv_frame()
        with pytest.raises(TazProtocolError) as exc_info:
            conn.recv_frame()
        assert exc_info.value.__cause__ is not None
        assert "connection closed after an earlier failure" in str(exc_info.value)

    def test_next_call_raises_chained_after_keyboard_interrupt(self) -> None:
        """After KeyboardInterrupt, next recv_frame raises TazConnectionLost."""
        ki = KeyboardInterrupt()
        sock = _mock_raising_sock(_capability_bytes(), ki)
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(KeyboardInterrupt):
            conn.recv_frame()
        with pytest.raises(TazConnectionLost) as exc_info:
            conn.recv_frame()
        assert exc_info.value.__cause__ is ki
        assert "connection closed after an earlier failure" in str(exc_info.value)

    def test_next_call_does_not_touch_socket(self) -> None:
        """After a failure, subsequent calls do not access the (closed) socket."""
        exc = TimeoutError("stall")
        sock = _mock_stall_after_n(_capability_bytes(), 4, exc)
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazConnectionLost):
            conn.recv_frame()
        recv_count_after_failure = sock.recv.call_count
        # Subsequent call must not invoke recv again (socket is already closed).
        with pytest.raises(TazConnectionLost):
            conn.recv_frame()
        assert sock.recv.call_count == recv_count_after_failure

    def test_evil_bytes_not_reread(self) -> None:
        """Garbage left in the socket after a failure is never re-read.

        Simulates the review scenario: a TazProtocolError (OVERSIZED) occurs,
        leaving unread b"EVIL" in the socket.  The next call must not attempt
        to read that data -- it was never consumed and would corrupt the stream
        -- because the socket is already closed and _failure is set.
        """
        cap = _capability_bytes()
        oversized_header = pack_header(
            Frame(
                type=common_pb2.FRAME_TYPE_RESPONSE,
                flags=0,
                opcode=0,
                length=DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_RESPONSE] + 1,
                stream_id=1,
            )
        )
        # b"EVIL" would follow if recv were called again after the OVERSIZED header
        conn = Connection()
        with patch(
            "taz.c3.connection.socket.create_connection",
            return_value=_mock_sock(cap, oversized_header, b"EVIL"),
        ):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazProtocolError):
            conn.recv_frame()
        # Socket is closed; b"EVIL" must never have been read.
        assert conn.closed
        with pytest.raises(TazProtocolError):
            conn.recv_frame()

    def test_partial_send_timeout_closes(self) -> None:
        """A timeout during send_request closes the connection."""
        cap = _capability_bytes(operations=[common_pb2.OPCODE_PING])
        sock = _mock_sock(cap)
        sock.sendmsg.side_effect = TimeoutError("send stall")
        sock.sendall.side_effect = TimeoutError("send stall")
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazConnectionLost):
            conn.send_request(common_pb2.OPCODE_PING, b"")
        assert conn.closed

    def test_next_call_raises_chained_after_send_failure(self) -> None:
        """After a send failure, subsequent calls raise TazConnectionLost chained."""
        cap = _capability_bytes(operations=[common_pb2.OPCODE_PING])
        sock = _mock_sock(cap)
        sock.sendmsg.side_effect = TimeoutError("send stall")
        sock.sendall.side_effect = TimeoutError("send stall")
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazConnectionLost):
            conn.send_request(common_pb2.OPCODE_PING, b"")
        with pytest.raises(TazConnectionLost) as exc_info:
            conn.recv_frame()
        assert exc_info.value.__cause__ is not None
        assert "connection closed after an earlier failure" in str(exc_info.value)


class TestSendRequest:
    def test_unadvertised_opcode_raises_not_supported(self) -> None:
        """Opcode absent from capabilities.operations raises NOT_SUPPORTED locally."""
        # Connect with only PING advertised; attempt OPCODE_VERSION.
        conn = Connection()
        _connect(conn, _capability_bytes(operations=[common_pb2.OPCODE_PING]))
        with pytest.raises(TazError) as exc_info:
            conn.send_request(common_pb2.OPCODE_VERSION, b"")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED

    def test_unadvertised_opcode_nothing_written(self) -> None:
        """NOT_SUPPORTED does not touch the socket."""
        cap = _capability_bytes(operations=[common_pb2.OPCODE_PING])
        mock = _mock_sock(cap)
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=mock):
            conn.connect("127.0.0.1", 5555)
        with pytest.raises(TazError):
            conn.send_request(common_pb2.OPCODE_VERSION, b"")
        mock.sendmsg.assert_not_called()
        mock.sendall.assert_not_called()

    def test_request_over_limit_raises_invalid_request(self) -> None:
        """Payload one byte over the REQUEST limit raises INVALID_REQUEST."""
        conn = Connection()
        _connect(conn, _capability_bytes(operations=[common_pb2.OPCODE_PING]))
        request_limit = DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST]
        oversized = b"x" * (request_limit + 1)
        with pytest.raises(TazError) as exc_info:
            conn.send_request(common_pb2.OPCODE_PING, oversized)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        assert str(request_limit) in exc_info.value.message
        assert "not sent" in exc_info.value.message

    def test_request_over_limit_nothing_written(self) -> None:
        """INVALID_REQUEST from size check does not touch the socket."""
        cap = _capability_bytes(operations=[common_pb2.OPCODE_PING])
        mock = _mock_sock(cap)
        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=mock):
            conn.connect("127.0.0.1", 5555)
        request_limit = DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST]
        with pytest.raises(TazError):
            conn.send_request(common_pb2.OPCODE_PING, b"x" * (request_limit + 1))
        mock.sendmsg.assert_not_called()
        mock.sendall.assert_not_called()

    def test_over_limit_connection_not_poisoned(self) -> None:
        """After an INVALID_REQUEST, a following in-limit request succeeds."""
        sent: list[bytes] = []

        cap_bytes = _capability_bytes(operations=[common_pb2.OPCODE_PING])
        cap_buf = bytearray(cap_bytes)

        def recv(bufsize: int, flags: int = 0) -> bytes:
            if cap_buf:
                if flags & socket.MSG_PEEK:
                    return bytes(cap_buf[:1])
                take = min(bufsize, len(cap_buf))
                data = bytes(cap_buf[:take])
                del cap_buf[:take]
                return data
            return b""

        def sendmsg(views: list[memoryview]) -> int:
            total = 0
            for v in views:
                sent.append(bytes(v))
                total += len(v)
            return total

        def sendall(data: bytes) -> None:
            sent.append(data)

        sock = MagicMock(spec=_SOCKET_SPEC)
        sock.recv.side_effect = recv
        sock.gettimeout.return_value = None
        sock.sendmsg.side_effect = sendmsg
        sock.sendall.side_effect = sendall

        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)

        request_limit = DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST]
        with pytest.raises(TazError):
            conn.send_request(common_pb2.OPCODE_PING, b"x" * (request_limit + 1))

        assert not conn.closed
        stream_id = conn.send_request(common_pb2.OPCODE_PING, b"")
        assert stream_id >= 1
        assert len(sent) > 0

    def test_stream_id_starts_at_1(self) -> None:
        """First send_request assigns stream_id 1."""
        sent_frames: list[bytes] = []

        cap_bytes = _capability_bytes(operations=[common_pb2.OPCODE_PING])
        cap_buf = bytearray(cap_bytes)

        def recv(bufsize: int, flags: int = 0) -> bytes:
            if cap_buf:
                if flags & socket.MSG_PEEK:
                    return bytes(cap_buf[:1])
                take = min(bufsize, len(cap_buf))
                data = bytes(cap_buf[:take])
                del cap_buf[:take]
                return data
            return b""

        def capture_send(views: list[memoryview]) -> int:
            total = 0
            for v in views:
                sent_frames.append(bytes(v))
                total += len(v)
            return total

        def capture_sendall(data: bytes) -> None:
            sent_frames.append(data)

        sock = MagicMock(spec=_SOCKET_SPEC)
        sock.recv.side_effect = recv
        sock.gettimeout.return_value = None
        sock.sendmsg.side_effect = capture_send
        sock.sendall.side_effect = capture_sendall

        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)

        stream_id = conn.send_request(common_pb2.OPCODE_PING, b"")
        assert stream_id == 1

    def test_stream_id_increments(self) -> None:
        """Successive send_request calls assign monotonically increasing stream_ids."""
        cap_bytes = _capability_bytes(operations=[common_pb2.OPCODE_PING])
        cap_buf = bytearray(cap_bytes)

        def recv(bufsize: int, flags: int = 0) -> bytes:
            if cap_buf:
                if flags & socket.MSG_PEEK:
                    return bytes(cap_buf[:1])
                take = min(bufsize, len(cap_buf))
                data = bytes(cap_buf[:take])
                del cap_buf[:take]
                return data
            return b""

        sock = MagicMock(spec=_SOCKET_SPEC)
        sock.recv.side_effect = recv
        sock.gettimeout.return_value = None
        sock.sendmsg.side_effect = lambda views: sum(len(v) for v in views)

        conn = Connection()
        with patch("taz.c3.connection.socket.create_connection", return_value=sock):
            conn.connect("127.0.0.1", 5555)

        ids = [conn.send_request(common_pb2.OPCODE_PING, b"") for _ in range(5)]
        assert ids == [1, 2, 3, 4, 5]

    def test_stream_id_skips_zero_on_wrap(self) -> None:
        """stream_id wraps from 0xFFFFFFFF to 1, never 0."""
        conn = Connection()
        _connect(conn, _capability_bytes(operations=[common_pb2.OPCODE_PING]))
        conn._next_stream_id = 0xFFFFFFFF
        stream_id = conn.send_request(common_pb2.OPCODE_PING, b"")
        assert stream_id == 0xFFFFFFFF
        assert conn._next_stream_id == 1
