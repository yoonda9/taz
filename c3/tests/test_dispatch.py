"""Tests for the dispatch loop: buffering, backlog, keepalive."""

from __future__ import annotations

import socket
from unittest.mock import MagicMock, patch

import pytest
from google.protobuf.message import Message
from taz.c3.connection import Connection
from taz.c3.errors import TazConnectionLost, TazProtocolError
from taz.c3.protocol.dispatch import Dispatcher
from taz.c3.protocol.frame import Frame, pack_header
from taz.c3.settings import Backlog, Keepalive
from taz.v1 import command_pb2, common_pb2, daemon_control_pb2, process_pb2

# ---------------------------------------------------------------------------
# Test helpers
# ---------------------------------------------------------------------------


def _capability_bytes(
    protocol_major: int = 1,
    operations: list[int] | None = None,
) -> bytes:
    cap = daemon_control_pb2.CapabilityPayload(
        protocol_major=protocol_major,
        operations=operations or [],
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


def _frame_bytes(
    type_: int,
    payload: bytes = b"",
    *,
    opcode: int = 0,
    stream_id: int = 1,
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


def _response(
    opcode: int = common_pb2.OPCODE_PING,
    stream_id: int = 1,
    payload: bytes = b"",
) -> bytes:
    return _frame_bytes(
        common_pb2.FRAME_TYPE_RESPONSE,
        payload,
        opcode=opcode,
        stream_id=stream_id,
    )


def _pong() -> bytes:
    return _frame_bytes(common_pb2.FRAME_TYPE_PONG, stream_id=0)


# socket.socket has no sendmsg on Windows, where send_frame uses sendall; a
# spec with both lets the same mock serve whichever path the platform takes.
_SOCKET_SPEC = sorted({*dir(socket.socket), "sendmsg"})


def _send_count(sock: MagicMock) -> int:
    """Frames written through either send path."""
    return int(sock.sendmsg.call_count) + int(sock.sendall.call_count)


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


def _connected(
    *extra_chunks: bytes,
    operations: list[int] | None = None,
) -> tuple[Connection, MagicMock]:
    """Return a connected Connection plus its mock socket."""
    cap = _capability_bytes(operations=operations)
    mock_sock = _mock_sock(cap, *extra_chunks)
    conn = Connection()
    with patch("taz.c3.connection.socket.create_connection", return_value=mock_sock):
        conn.connect("127.0.0.1", 5555)
    return conn, mock_sock


def _dispatcher(
    *extra_chunks: bytes,
    backlog: Backlog | None = None,
    operations: list[int] | None = None,
    _wait_readable: object = None,
) -> tuple[Connection, MagicMock, Dispatcher]:
    conn, sock = _connected(*extra_chunks, operations=operations)
    d = Dispatcher(
        conn,
        backlog,
        _wait_readable=_wait_readable,  # type: ignore[arg-type]
    )
    return conn, sock, d


# ---------------------------------------------------------------------------
# TestCoreBuffering: pending-frame buffer and routing
# ---------------------------------------------------------------------------


class TestCoreBuffering:
    def test_frame_for_other_stream_buffered(self) -> None:
        """Frame for stream 2 is buffered while waiting for stream 1."""
        resp2 = _response(opcode=common_pb2.OPCODE_PING, stream_id=2)
        resp1 = _response(opcode=common_pb2.OPCODE_PING, stream_id=1)
        _, _, d = _dispatcher(resp2, resp1)

        frame = d.recv_response(1, Keepalive.OFF)
        assert frame.stream_id == 1

    def test_buffered_frame_returned_next(self) -> None:
        """Buffered frame for stream 2 is returned on the subsequent call."""
        resp2 = _response(opcode=common_pb2.OPCODE_PING, stream_id=2)
        resp1 = _response(opcode=common_pb2.OPCODE_PING, stream_id=1)
        _, _, d = _dispatcher(resp2, resp1)

        d.recv_response(1, Keepalive.OFF)
        frame2 = d.recv_response(2, Keepalive.OFF)
        assert frame2.stream_id == 2

    def test_buffer_checked_first_no_socket_read(self) -> None:
        """If the buffer already holds the expected frame, no new socket read occurs."""
        resp2 = _response(opcode=common_pb2.OPCODE_PING, stream_id=2)
        resp1 = _response(opcode=common_pb2.OPCODE_PING, stream_id=1)
        _, sock, d = _dispatcher(resp2, resp1)

        d.recv_response(1, Keepalive.OFF)
        recv_count = sock.recv.call_count

        d.recv_response(2, Keepalive.OFF)
        assert sock.recv.call_count == recv_count


# ---------------------------------------------------------------------------
# TestClosedStreamDiscard
# ---------------------------------------------------------------------------


class TestClosedStreamDiscard:
    def test_closed_stream_frame_discarded(self) -> None:
        """Frames for a closed stream are silently discarded."""
        resp2 = _response(stream_id=2)
        resp1 = _response(stream_id=1)
        _, _, d = _dispatcher(resp2, resp1)
        d.close_stream(2)

        frame = d.recv_response(1, Keepalive.OFF)
        assert frame.stream_id == 1

    def test_close_stream_frees_buffered(self) -> None:
        """Closing a stream removes its buffered frames from the total counts."""
        resp2 = _response(stream_id=2)
        resp1 = _response(stream_id=1)
        _, _, d = _dispatcher(resp2, resp1)

        d.recv_response(1, Keepalive.OFF)
        assert d._total_frames == 1

        d.close_stream(2)
        assert d._total_frames == 0
        assert 2 not in d._buffer

    def test_close_stream_already_closed_noop(self) -> None:
        """Calling close_stream twice on the same stream_id is a no-op."""
        _, _, d = _dispatcher()
        d.close_stream(42)
        d.close_stream(42)


# ---------------------------------------------------------------------------
# TestUnknownFrameDrop
# ---------------------------------------------------------------------------


class TestUnknownFrameDrop:
    def test_unknown_type_0x08_dropped(self) -> None:
        """An unknown frame type (0x08) on the expected stream is dropped."""
        unknown = _frame_bytes(0x08, b"garbage", stream_id=1)
        resp = _response(stream_id=1)
        _, _, d = _dispatcher(unknown, resp)

        frame = d.recv_response(1, Keepalive.OFF)
        assert frame.type == common_pb2.FRAME_TYPE_RESPONSE

    def test_unknown_type_0x00_dropped(self) -> None:
        """Frame type 0x00 (UNSPECIFIED) is treated as unknown and dropped."""
        unknown = _frame_bytes(0x00, b"garbage", stream_id=1)
        resp = _response(stream_id=1)
        _, _, d = _dispatcher(unknown, resp)

        frame = d.recv_response(1, Keepalive.OFF)
        assert frame.type == common_pb2.FRAME_TYPE_RESPONSE


# ---------------------------------------------------------------------------
# TestPongResolution
# ---------------------------------------------------------------------------


class TestPongResolution:
    def test_pong_skipped_response_still_returned(self) -> None:
        """A PONG frame is consumed; the following RESPONSE is returned."""
        pong = _pong()
        resp = _response(stream_id=1)
        _, _, d = _dispatcher(pong, resp)

        frame = d.recv_response(1, Keepalive.OFF)
        assert frame.type == common_pb2.FRAME_TYPE_RESPONSE


# ---------------------------------------------------------------------------
# TestEchoedOpcodeValidation
# ---------------------------------------------------------------------------


class TestEchoedOpcodeValidation:
    def test_matching_opcode_ok(self) -> None:
        resp = _response(opcode=common_pb2.OPCODE_PING, stream_id=1)
        _, _, d = _dispatcher(resp)

        frame = d.recv_response(
            1, Keepalive.OFF, expected_opcode=common_pb2.OPCODE_PING
        )
        assert frame.opcode == common_pb2.OPCODE_PING

    def test_mismatched_opcode_raises(self) -> None:
        resp = _response(opcode=common_pb2.OPCODE_VERSION, stream_id=1)
        _, _, d = _dispatcher(resp)

        with pytest.raises(TazProtocolError):
            d.recv_response(1, Keepalive.OFF, expected_opcode=common_pb2.OPCODE_PING)

    def test_none_opcode_skips_validation(self) -> None:
        resp = _response(opcode=common_pb2.OPCODE_VERSION, stream_id=1)
        _, _, d = _dispatcher(resp)

        frame = d.recv_response(1, Keepalive.OFF, expected_opcode=None)
        assert frame is not None


# ---------------------------------------------------------------------------
# TestBacklogOverflow
# ---------------------------------------------------------------------------


class TestBacklogOverflow:
    def test_backlog_fill_no_exception(self) -> None:
        """Filling the backlog with frames for another stream raises nothing."""
        # max_frames=4: add 5 frames for stream 2, then get the RESPONSE for stream 1.
        frames = b"".join(_response(stream_id=2) for _ in range(5))
        resp1 = _response(stream_id=1)
        backlog = Backlog(max_frames=4)
        _, _, d = _dispatcher(frames, resp1, backlog=backlog)

        frame = d.recv_response(1, Keepalive.OFF)
        assert frame.stream_id == 1

    def test_overflowed_stream_joins_closed(self) -> None:
        """The overflowed stream ends up in the closed set."""
        frames = b"".join(_response(stream_id=2) for _ in range(5))
        resp1 = _response(stream_id=1)
        backlog = Backlog(max_frames=4)
        _, _, d = _dispatcher(frames, resp1, backlog=backlog)

        d.recv_response(1, Keepalive.OFF)
        assert 2 in d._closed

    def test_overflowed_stream_later_frames_discarded(self) -> None:
        """Frames for the overflowed stream arriving after overflow are discarded."""
        frames = b"".join(_response(stream_id=2) for _ in range(5))
        resp1 = _response(stream_id=1)
        resp2_late = _response(stream_id=2)
        resp1_b = _response(stream_id=1, opcode=common_pb2.OPCODE_VERSION)
        backlog = Backlog(max_frames=4)
        _, _, d = _dispatcher(frames, resp1, resp2_late, resp1_b, backlog=backlog)

        d.recv_response(1, Keepalive.OFF)
        # resp2_late is discarded (stream 2 is closed); resp1_b is returned.
        frame = d.recv_response(1, Keepalive.OFF)
        assert frame.opcode == common_pb2.OPCODE_VERSION

    def test_no_cancel_when_not_advertised(self) -> None:
        """CANCEL is not sent when the daemon does not advertise it."""
        frames = b"".join(_response(stream_id=2) for _ in range(5))
        resp1 = _response(stream_id=1)
        backlog = Backlog(max_frames=4)
        # No OPCODE_CANCEL in operations.
        _, sock, d = _dispatcher(frames, resp1, backlog=backlog)

        d.recv_response(1, Keepalive.OFF)
        # Nothing was written: no PING, no CANCEL.
        assert _send_count(sock) == 0

    def test_cancel_sent_when_advertised(self) -> None:
        """CANCEL REQUEST is sent for the victim when OPCODE_CANCEL is advertised."""
        frames = b"".join(_response(stream_id=2) for _ in range(5))
        resp1 = _response(stream_id=1)
        backlog = Backlog(max_frames=4)
        conn, sock, d = _dispatcher(
            frames,
            resp1,
            backlog=backlog,
            operations=[common_pb2.OPCODE_PING, common_pb2.OPCODE_CANCEL],
        )
        # Pre-claim stream_id=1 so the CANCEL request gets a higher stream_id
        # and does not shadow the response we are waiting for.
        assert conn.send_request(common_pb2.OPCODE_PING, b"") == 1

        d.recv_response(1, Keepalive.OFF)

        # The CANCEL REQUEST was sent: two writes (the PING request pre-claim
        # and the CANCEL request).
        assert _send_count(sock) >= 2

    def test_backlog_bytes_limit_respected(self) -> None:
        """Overflow triggers when bytes limit is reached."""
        payload = b"x" * 100
        # max_bytes=250: third frame (cumulative 300 bytes) triggers overflow.
        frames = b"".join(_response(stream_id=2, payload=payload) for _ in range(3))
        resp1 = _response(stream_id=1)
        backlog = Backlog(max_frames=999, max_bytes=250)
        _, _, d = _dispatcher(frames, resp1, backlog=backlog)

        frame = d.recv_response(1, Keepalive.OFF)
        assert frame.stream_id == 1
        assert 2 in d._closed


# ---------------------------------------------------------------------------
# TestKeepalive
# ---------------------------------------------------------------------------


class TestKeepalive:
    def test_idle_timeout_sends_ping(self) -> None:
        """When wait_readable times out (idle), a PING is sent."""
        pong = _pong()
        resp = _response(stream_id=1)
        wait_calls: list[float | None] = []
        seq = iter([False, True, True])

        def mock_wait(sock: object, timeout: float | None) -> bool:
            wait_calls.append(timeout)
            return next(seq)

        _, sock, d = _dispatcher(pong, resp, _wait_readable=mock_wait)

        frame = d.recv_response(1, Keepalive(idle=5.0, timeout=10.0))
        assert frame.stream_id == 1
        # First wait used idle, second used timeout (probe), third True → data.
        assert wait_calls[0] == 5.0
        assert wait_calls[1] == 10.0
        # PING was sent.
        assert _send_count(sock) >= 1

    def test_pong_ends_probe_response_returned(self) -> None:
        """A PONG after PING probe ends probing; RESPONSE is still returned."""
        pong = _pong()
        resp = _response(stream_id=1)
        seq = iter([False, True, True])
        _, _, d = _dispatcher(pong, resp, _wait_readable=lambda s, t: next(seq))

        frame = d.recv_response(1, Keepalive(idle=5.0, timeout=10.0))
        assert frame.type == common_pb2.FRAME_TYPE_RESPONSE

    def test_any_frame_during_probe_ends_it(self) -> None:
        """A non-PONG frame during probe also counts as liveness."""
        # Receive an unrelated RESPONSE (stream 2) during the probe window.
        resp2 = _response(stream_id=2)
        resp1 = _response(stream_id=1)
        seq = iter([False, True, True])
        _, _, d = _dispatcher(resp2, resp1, _wait_readable=lambda s, t: next(seq))

        frame = d.recv_response(1, Keepalive(idle=5.0, timeout=10.0))
        assert frame.stream_id == 1

    def test_no_pong_raises_connection_lost(self) -> None:
        """No response to PING probe → TazConnectionLost."""
        seq = iter([False, False])
        _, _, d = _dispatcher(_wait_readable=lambda s, t: next(seq))

        with pytest.raises(TazConnectionLost, match="no PONG"):
            d.recv_response(1, Keepalive(idle=5.0, timeout=10.0))

    def test_no_pong_closes_connection(self) -> None:
        """TazConnectionLost from probe also closes the connection."""
        seq = iter([False, False])
        conn, _, d = _dispatcher(_wait_readable=lambda s, t: next(seq))

        with pytest.raises(TazConnectionLost):
            d.recv_response(1, Keepalive(idle=5.0, timeout=10.0))

        assert conn.closed

    def test_keepalive_off_no_ping_sent(self) -> None:
        """Keepalive.OFF: wait_readable is called with None (block forever)."""
        resp = _response(stream_id=1)
        wait_calls: list[float | None] = []

        def mock_wait(sock: object, timeout: float | None) -> bool:
            wait_calls.append(timeout)
            return True

        _, sock, d = _dispatcher(resp, _wait_readable=mock_wait)

        d.recv_response(1, Keepalive.OFF)
        # OFF normalizes both idle and timeout to None.
        assert all(t is None for t in wait_calls)
        # No PING was sent.
        assert _send_count(sock) == 0

    def test_per_call_keepalive_overrides_default(self) -> None:
        """Per-call keepalive values are used, not a hard-coded default."""
        resp = _response(stream_id=1)
        wait_calls: list[float | None] = []

        def mock_wait(sock: object, timeout: float | None) -> bool:
            wait_calls.append(timeout)
            return True

        _, _, d = _dispatcher(resp, _wait_readable=mock_wait)

        d.recv_response(1, Keepalive(idle=7.0, timeout=15.0))
        assert wait_calls[0] == 7.0

    def test_probe_timeout_uses_keepalive_timeout(self) -> None:
        """After sending PING, the subsequent wait uses keepalive.timeout."""
        pong = _pong()
        resp = _response(stream_id=1)
        wait_calls: list[float | None] = []
        seq = iter([False, True, True])

        def mock_wait(sock: object, timeout: float | None) -> bool:
            wait_calls.append(timeout)
            return next(seq)

        _, _, d = _dispatcher(pong, resp, _wait_readable=mock_wait)

        d.recv_response(1, Keepalive(idle=5.0, timeout=11.0))
        assert wait_calls[0] == 5.0  # idle
        assert wait_calls[1] == 11.0  # probe timeout


# ---------------------------------------------------------------------------
# TestChunkedResponse: RESPONSE frames with FRAME_FLAG_CONTINUATION (§6.1)
# ---------------------------------------------------------------------------


def _chunk(
    msg: Message,
    *,
    opcode: int = common_pb2.OPCODE_COMMAND_EXEC,
    stream_id: int = 1,
    final: bool = False,
) -> bytes:
    """One RESPONSE frame of a chunked response, carrying ``msg``."""
    return _frame_bytes(
        common_pb2.FRAME_TYPE_RESPONSE,
        msg.SerializeToString(),
        opcode=opcode,
        stream_id=stream_id,
        flags=0 if final else common_pb2.FRAME_FLAG_CONTINUATION,
    )


def _recv_exec(d: Dispatcher, stream_id: int = 1) -> Frame:
    return d.recv_response(
        stream_id,
        Keepalive.OFF,
        expected_opcode=common_pb2.OPCODE_COMMAND_EXEC,
        response_type=command_pb2.CommandExecResponse,
    )


def _exec_response(frame: Frame) -> command_pb2.CommandExecResponse:
    msg = command_pb2.CommandExecResponse()
    msg.ParseFromString(frame.payload)
    return msg


class TestChunkedResponse:
    def test_three_frames_reassemble_to_original(self) -> None:
        """bytes fields concatenate across frames; scalars ride the last."""
        original = command_pb2.CommandExecResponse(
            exit_code=3,
            stdout_data=bytes(range(256)) * 600,
            stderr_data=b"warning: output cut\n",
            truncated=True,
        )
        out, err = original.stdout_data, original.stderr_data
        third = len(out) // 3
        frames = (
            _chunk(command_pb2.CommandExecResponse(stdout_data=out[:third]))
            + _chunk(
                command_pb2.CommandExecResponse(
                    stdout_data=out[third : 2 * third], stderr_data=err[:8]
                )
            )
            + _chunk(
                command_pb2.CommandExecResponse(
                    stdout_data=out[2 * third :],
                    stderr_data=err[8:],
                    exit_code=3,
                    truncated=True,
                ),
                final=True,
            )
        )
        _, _, d = _dispatcher(frames)

        frame = _recv_exec(d)

        assert _exec_response(frame) == original
        assert frame.flags == 0
        assert frame.length == len(frame.payload)

    def test_string_repeated_and_submessage_fields_merge(self) -> None:
        original = process_pb2.ProcessInfoResponse(
            info=process_pb2.ProcessInfo(pid=42, name="python3", state="S"),
            command_line="python3 -m http.server 8000",
            start_time=1_790_000_000,
            open_files=[f"/srv/www/f{i}" for i in range(6)],
        )
        frames = _chunk(
            process_pb2.ProcessInfoResponse(
                info=process_pb2.ProcessInfo(name="pyt"),
                command_line="python3 -m ",
                open_files=original.open_files[:4],
            ),
            opcode=common_pb2.OPCODE_PROCESS_INFO,
        ) + _chunk(
            process_pb2.ProcessInfoResponse(
                info=process_pb2.ProcessInfo(pid=42, name="hon3", state="S"),
                command_line="http.server 8000",
                start_time=1_790_000_000,
                open_files=original.open_files[4:],
            ),
            opcode=common_pb2.OPCODE_PROCESS_INFO,
            final=True,
        )
        _, _, d = _dispatcher(frames)

        frame = d.recv_response(
            1, Keepalive.OFF, response_type=process_pb2.ProcessInfoResponse
        )

        merged = process_pb2.ProcessInfoResponse()
        merged.ParseFromString(frame.payload)
        assert merged == original

    def test_repeated_messages_appended_in_frame_order(self) -> None:
        keys = ["log.level", "compression", "exec.max_output_bytes"]
        frames = b"".join(
            _chunk(
                daemon_control_pb2.ConfigurationGetResponse(
                    config=[common_pb2.KeyValue(key=key, value=str(i))]
                ),
                opcode=common_pb2.OPCODE_CONFIGURATION_GET,
                final=i == len(keys) - 1,
            )
            for i, key in enumerate(keys)
        )
        _, _, d = _dispatcher(frames)

        frame = d.recv_response(
            1,
            Keepalive.OFF,
            response_type=daemon_control_pb2.ConfigurationGetResponse,
        )

        merged = daemon_control_pb2.ConfigurationGetResponse()
        merged.ParseFromString(frame.payload)
        assert [(kv.key, kv.value) for kv in merged.config] == [
            ("log.level", "0"),
            ("compression", "1"),
            ("exec.max_output_bytes", "2"),
        ]

    def test_scalars_come_from_final_frame(self) -> None:
        """An earlier frame's scalar does not survive a default in the last."""
        frames = _chunk(
            command_pb2.CommandExecResponse(
                stdout_data=b"a", exit_code=1, timed_out=True
            )
        ) + _chunk(command_pb2.CommandExecResponse(stdout_data=b"b"), final=True)
        _, _, d = _dispatcher(frames)

        merged = _exec_response(_recv_exec(d))

        assert merged.stdout_data == b"ab"
        assert merged.exit_code == 0
        assert merged.timed_out is False

    def test_single_frame_returned_unchanged(self) -> None:
        payload = command_pb2.CommandExecResponse(stdout_data=b"hi").SerializeToString()
        _, _, d = _dispatcher(
            _response(opcode=common_pb2.OPCODE_COMMAND_EXEC, payload=payload)
        )

        frame = _recv_exec(d)

        assert frame.payload == payload

    def test_error_frame_ends_response(self) -> None:
        """Chunks already received are dropped; the ERROR frame is returned."""
        info = common_pb2.ErrorInfo(
            code=common_pb2.ERROR_CODE_INTERNAL, message="spawn failed"
        )
        frames = _chunk(
            command_pb2.CommandExecResponse(stdout_data=b"partial")
        ) + _frame_bytes(
            common_pb2.FRAME_TYPE_ERROR,
            info.SerializeToString(),
            opcode=common_pb2.OPCODE_COMMAND_EXEC,
        )
        _, _, d = _dispatcher(frames)

        frame = _recv_exec(d)

        assert frame.type == common_pb2.FRAME_TYPE_ERROR
        got = common_pb2.ErrorInfo()
        got.ParseFromString(frame.payload)
        assert got == info

    def test_frames_for_other_streams_buffered_meanwhile(self) -> None:
        first = _chunk(
            command_pb2.CommandExecResponse(stdout_data=b"one "), stream_id=1
        )
        other = _response(opcode=common_pb2.OPCODE_VERSION, stream_id=2)
        last = _chunk(
            command_pb2.CommandExecResponse(stdout_data=b"two"), stream_id=1, final=True
        )
        _, _, d = _dispatcher(first, other, last)

        assert _exec_response(_recv_exec(d)).stdout_data == b"one two"
        assert d.recv_response(2, Keepalive.OFF).stream_id == 2

    def test_buffered_chunks_merge(self) -> None:
        """Chunks buffered while another stream was awaited still merge."""
        first = _chunk(
            command_pb2.CommandExecResponse(stdout_data=b"one "), stream_id=1
        )
        last = _chunk(
            command_pb2.CommandExecResponse(stdout_data=b"two", exit_code=7),
            stream_id=1,
            final=True,
        )
        other = _response(opcode=common_pb2.OPCODE_VERSION, stream_id=2)
        _, _, d = _dispatcher(first, last, other)

        d.recv_response(2, Keepalive.OFF)
        merged = _exec_response(_recv_exec(d))

        assert merged.stdout_data == b"one two"
        assert merged.exit_code == 7

    def test_without_response_type_each_frame_returned(self) -> None:
        """A streamed response sees every CONTINUATION frame on its own."""
        frames = _chunk(command_pb2.CommandExecResponse(stdout_data=b"a")) + _chunk(
            command_pb2.CommandExecResponse(stdout_data=b"b"), final=True
        )
        _, _, d = _dispatcher(frames)

        first = d.recv_response(1, Keepalive.OFF)
        second = d.recv_response(1, Keepalive.OFF)

        assert first.flags & common_pb2.FRAME_FLAG_CONTINUATION
        assert _exec_response(first).stdout_data == b"a"
        assert not second.flags & common_pb2.FRAME_FLAG_CONTINUATION
        assert _exec_response(second).stdout_data == b"b"
