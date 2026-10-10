"""Tests for client.process.monitor() and StreamIterator."""

from __future__ import annotations

import gc
import socket
import time
import warnings
from unittest.mock import MagicMock, patch

import pytest
import taz.c3
from taz.c3.client import TazClient
from taz.c3.errors import TazConnectionLost, TazError, TazProtocolError
from taz.c3.process import ProcessMonitorUpdate
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header, unpack_header
from taz.c3.protocol.frame import wait_readable as _real_wait_readable
from taz.c3.settings import Backlog, Keepalive
from taz.c3.stream import StreamIterator
from taz.v1 import advanced_pb2, common_pb2, daemon_control_pb2, process_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_cancel.py)
# ---------------------------------------------------------------------------

_MONITOR_OPS = [
    common_pb2.OPCODE_PING,
    common_pb2.OPCODE_CANCEL,
    common_pb2.OPCODE_PROCESS_MONITOR,
    common_pb2.OPCODE_VERSION,
]


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    ops = operations if operations is not None else _MONITOR_OPS
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


def _monitor_frame(
    *,
    stream_id: int = 1,
    continuation: bool,
    pid: int = 42,
    name: str = "proc",
    user: str = "user",
    cpu_percent: float = 0.0,
    memory_bytes: int = 1024,
    state: str = "running",
    exited: bool = False,
    exit_code: int = 0,
    exit_code_known: bool = False,
    reason: str = "",
) -> bytes:
    resp = process_pb2.ProcessMonitorResponse(
        info=process_pb2.ProcessInfo(
            pid=pid,
            name=name,
            user=user,
            cpu_percent=cpu_percent,
            memory_bytes=memory_bytes,
            state=state,
        ),
        exited=exited,
        exit_code=exit_code,
        exit_code_known=exit_code_known,
        reason=reason,
    )
    flags = common_pb2.FRAME_FLAG_CONTINUATION if continuation else 0
    return _frame_bytes(
        common_pb2.FRAME_TYPE_RESPONSE,
        resp.SerializeToString(),
        opcode=common_pb2.OPCODE_PROCESS_MONITOR,
        stream_id=stream_id,
        flags=flags,
    )


def _error_frame(
    code: common_pb2.ErrorCode,
    message: str = "boom",
    *,
    opcode: int = common_pb2.OPCODE_PROCESS_MONITOR,
    stream_id: int = 1,
) -> bytes:
    info = common_pb2.ErrorInfo(code=code, message=message)
    return _frame_bytes(
        common_pb2.FRAME_TYPE_ERROR,
        info.SerializeToString(),
        opcode=opcode,
        stream_id=stream_id,
    )


def _cancel_response_bytes(cancelled: bool, stream_id: int) -> bytes:
    resp = advanced_pb2.CancelResponse(cancelled=cancelled)
    return _frame_bytes(
        common_pb2.FRAME_TYPE_RESPONSE,
        resp.SerializeToString(),
        opcode=common_pb2.OPCODE_CANCEL,
        stream_id=stream_id,
    )


def _pong_bytes() -> bytes:
    return _frame_bytes(common_pb2.FRAME_TYPE_PONG, stream_id=0)


def _version_response_bytes(stream_id: int) -> bytes:
    resp = daemon_control_pb2.VersionResponse()
    return _frame_bytes(
        common_pb2.FRAME_TYPE_RESPONSE,
        resp.SerializeToString(),
        opcode=common_pb2.OPCODE_VERSION,
        stream_id=stream_id,
    )


_SOCKET_SPEC = sorted({*dir(socket.socket), "sendmsg"})


def _mock_sock(*chunks: bytes) -> MagicMock:
    """Mock socket that serves ``chunks`` sequentially then EOF.

    Records every ``sendmsg``/``sendall`` call's bytes on ``sock.sent``.
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
    *extra_chunks: bytes,
    operations: list[int] | None = None,
    backlog: Backlog | None = None,
) -> tuple[TazClient, MagicMock]:
    mock_sock = _mock_sock(_capability_bytes(operations=operations), *extra_chunks)
    client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF, backlog=backlog)
    with patch(_PATCH_CC, return_value=mock_sock):
        client.connect()
    return client, mock_sock


def _requests_sent(mock_sock: MagicMock) -> list[Frame]:
    """Every REQUEST frame's header, decoded in send order."""
    sent = b"".join(mock_sock.sent)
    frames = []
    offset = 0
    while offset < len(sent):
        header = unpack_header(sent[offset : offset + HEADER_SIZE])
        offset += HEADER_SIZE
        header.payload = sent[offset : offset + header.length]
        offset += header.length
        frames.append(header)
    return frames


# ---------------------------------------------------------------------------
# monitor(): request building
# ---------------------------------------------------------------------------


class TestMonitorRequest:
    def test_sends_process_monitor_request_with_default_interval(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _monitor_frame(continuation=False)
        )
        list(client.process.monitor(42))
        [frame] = _requests_sent(mock_sock)
        assert frame.type == common_pb2.FRAME_TYPE_REQUEST
        assert frame.opcode == common_pb2.OPCODE_PROCESS_MONITOR
        assert frame.opcode == 0x0023
        req = process_pb2.ProcessMonitorRequest()
        req.ParseFromString(frame.payload)
        assert req.pid == 42
        assert req.interval_ms == 1000

    def test_sends_requested_interval(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _monitor_frame(continuation=False)
        )
        list(client.process.monitor(42, interval_ms=250))
        [frame] = _requests_sent(mock_sock)
        req = process_pb2.ProcessMonitorRequest()
        req.ParseFromString(frame.payload)
        assert req.interval_ms == 250

    def test_registers_stream_with_requested_overflow(self) -> None:
        client, _ = _connected_client_with_sock(_monitor_frame(continuation=False))
        it = client.process.monitor(42, overflow="drop_oldest")
        assert client._dispatcher._streams[it.stream_id].overflow == "drop_oldest"
        list(it)

    def test_default_overflow_is_cancel(self) -> None:
        client, _ = _connected_client_with_sock(_monitor_frame(continuation=False))
        it = client.process.monitor(42)
        assert client._dispatcher._streams[it.stream_id].overflow == "cancel"
        list(it)

    def test_not_advertised_raises_not_supported(self) -> None:
        client, _ = _connected_client_with_sock(operations=[common_pb2.OPCODE_PING])
        with pytest.raises(TazError) as exc_info:
            client.process.monitor(42)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED

    def test_not_advertised_sends_nothing(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING]
        )
        with pytest.raises(TazError):
            client.process.monitor(42)
        mock_sock.sendmsg.assert_not_called()
        mock_sock.sendall.assert_not_called()

    def test_negative_pid_raises_value_error_sends_nothing(self) -> None:
        client, mock_sock = _connected_client_with_sock()
        with pytest.raises(ValueError):
            client.process.monitor(-1)
        mock_sock.sendmsg.assert_not_called()
        mock_sock.sendall.assert_not_called()

    def test_forwards_per_call_keepalive_to_recv_response(self) -> None:
        client, _ = _connected_client_with_sock(_monitor_frame(continuation=False))
        kv = Keepalive(idle=60.0, timeout=10.0)
        it = client.process.monitor(42, keepalive=kv)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            next(it)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# Iteration
# ---------------------------------------------------------------------------


class TestMonitorIteration:
    def test_yields_updates_then_stop_iteration_on_final_frame(self) -> None:
        client, _ = _connected_client_with_sock(
            _monitor_frame(continuation=True, cpu_percent=0.0, memory_bytes=1),
            _monitor_frame(continuation=True, cpu_percent=12.5, memory_bytes=2),
            _monitor_frame(
                continuation=False,
                exited=True,
                reason="exited",
                exit_code_known=False,
                memory_bytes=3,
            ),
        )
        it = client.process.monitor(42)
        updates = list(it)
        assert len(updates) == 3
        assert all(isinstance(u, ProcessMonitorUpdate) for u in updates)
        assert [u.info.memory_bytes for u in updates] == [1, 2, 3]
        assert updates[0].info.pid == 42
        assert updates[0].info.name == "proc"
        assert updates[0].info.user == "user"
        assert updates[0].info.state == "running"
        assert updates[0].exited is False
        assert updates[0].reason == ""
        assert updates[2].exited is True
        assert updates[2].reason == "exited"
        assert updates[2].exit_code_known is False
        assert it.ended

    def test_close_after_natural_end_sends_nothing(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _monitor_frame(continuation=False, exited=True, reason="exited"),
        )
        it = client.process.monitor(42)
        list(it)
        mock_sock.sent.clear()
        it.close()
        assert mock_sock.sent == []
        assert it.cancelled is None

    def test_error_frame_raises_taz_error_and_ends(self) -> None:
        client, _ = _connected_client_with_sock(
            _error_frame(common_pb2.ERROR_CODE_NOT_FOUND, "no such pid"),
        )
        it = client.process.monitor(42)
        with pytest.raises(TazError) as exc_info:
            next(it)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        assert it.ended

    def test_error_frame_close_sends_no_cancel(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _error_frame(common_pb2.ERROR_CODE_NOT_FOUND, "no such pid"),
        )
        it = client.process.monitor(42)
        with pytest.raises(TazError):
            next(it)
        mock_sock.sent.clear()
        it.close()
        assert mock_sock.sent == []

    def test_unexpected_frame_type_raises_protocol_error(self) -> None:
        client, _ = _connected_client_with_sock(
            _frame_bytes(
                common_pb2.FRAME_TYPE_FILE_CHUNK,
                b"data",
                stream_id=1,
                flags=0,
            ),
        )
        it = client.process.monitor(42)
        with pytest.raises(TazProtocolError):
            next(it)
        assert it.ended


# ---------------------------------------------------------------------------
# close() / CANCEL on break
# ---------------------------------------------------------------------------


class TestMonitorCancelOnClose:
    @pytest.mark.parametrize("cancelled", [True, False])
    def test_break_inside_with_sends_cancel_and_records_verdict(
        self, cancelled: bool
    ) -> None:
        client, mock_sock = _connected_client_with_sock(
            _monitor_frame(continuation=True, memory_bytes=1),
            _monitor_frame(continuation=True, memory_bytes=2),
            # A final frame queued ahead of the CancelResponse: close_stream()
            # already closed the id before this is read, so it is discarded
            # (and removes the id from the closed set per the leave rule).
            _monitor_frame(continuation=False, exited=True, reason="exited"),
            _cancel_response_bytes(cancelled, stream_id=2),
        )
        with client.process.monitor(42) as it:
            for i, update in enumerate(it):
                assert update.info.memory_bytes in (1, 2)
                if i == 1:
                    break
        assert it.cancelled is cancelled
        [req_frame] = [
            f for f in _requests_sent(mock_sock) if f.opcode == common_pb2.OPCODE_CANCEL
        ]
        req = advanced_pb2.CancelRequest()
        req.ParseFromString(req_frame.payload)
        assert req.target_stream_id == it.stream_id
        # The late final frame was consumed (and discarded) while waiting
        # for the CancelResponse, so nothing is left buffered.
        assert it.stream_id not in client._dispatcher._buffer

    def test_cancel_not_advertised_records_none_and_sends_nothing(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _monitor_frame(continuation=True),
            operations=[common_pb2.OPCODE_PROCESS_MONITOR],
        )
        it = client.process.monitor(42)
        next(it)
        mock_sock.sent.clear()
        it.close()
        assert it.cancelled is None
        assert mock_sock.sent == []
        assert it.stream_id in client._dispatcher._closed

    def test_close_twice_sends_one_cancel(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _monitor_frame(continuation=True),
            _cancel_response_bytes(True, stream_id=2),
        )
        it = client.process.monitor(42)
        next(it)
        it.close()
        it.close()
        cancel_requests = [
            f for f in _requests_sent(mock_sock) if f.opcode == common_pb2.OPCODE_CANCEL
        ]
        assert len(cancel_requests) == 1


# ---------------------------------------------------------------------------
# __del__ / ResourceWarning
# ---------------------------------------------------------------------------


@pytest.mark.filterwarnings("error::ResourceWarning")
class TestMonitorResourceWarning:
    def test_abandoned_iterator_warns(self) -> None:
        client, _ = _connected_client_with_sock(
            _monitor_frame(continuation=True),
            _pong_bytes(),
        )
        it = client.process.monitor(42)
        next(it)
        with pytest.warns(ResourceWarning, match="stream"):
            del it
            gc.collect()
        client.ping()

    def test_closed_iterator_does_not_warn(self) -> None:
        client, _ = _connected_client_with_sock(
            _monitor_frame(continuation=True),
            operations=[common_pb2.OPCODE_PROCESS_MONITOR],
        )
        it = client.process.monitor(42)
        next(it)
        it.close()
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            del it
            gc.collect()

    def test_naturally_ended_iterator_does_not_warn(self) -> None:
        client, _ = _connected_client_with_sock(
            _monitor_frame(continuation=False, exited=True, reason="exited"),
        )
        it = client.process.monitor(42)
        list(it)
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            del it
            gc.collect()

    def test_error_ended_iterator_does_not_warn(self) -> None:
        client, _ = _connected_client_with_sock(
            _error_frame(common_pb2.ERROR_CODE_NOT_FOUND),
        )
        it = client.process.monitor(42)
        with pytest.raises(TazError):
            next(it)
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            del it
            gc.collect()


# ---------------------------------------------------------------------------
# Backlog overflow while unread
# ---------------------------------------------------------------------------


class TestMonitorBacklog:
    def test_cancel_policy_cancels_unread_monitor_stream(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _monitor_frame(continuation=True, memory_bytes=1),
            _monitor_frame(continuation=True, memory_bytes=2),
            _monitor_frame(continuation=True, memory_bytes=3),
            _monitor_frame(continuation=True, memory_bytes=4),
            _version_response_bytes(stream_id=2),
            backlog=Backlog(max_frames=3),
        )
        it = client.process.monitor(42)
        client.version()

        cancel_requests = [
            f for f in _requests_sent(mock_sock) if f.opcode == common_pb2.OPCODE_CANCEL
        ]
        assert len(cancel_requests) == 1
        req = advanced_pb2.CancelRequest()
        req.ParseFromString(cancel_requests[0].payload)
        assert req.target_stream_id == it.stream_id

        with pytest.raises(TazError) as exc_info:
            next(it)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CANCELLED
        assert "slower interval" in exc_info.value.message
        # The iterator keeps the error; the dispatcher forgets the stream.
        assert it.stream_id not in client._dispatcher._streams
        # Every later __next__ raises the same recorded error.
        with pytest.raises(TazError) as exc_info2:
            next(it)
        assert exc_info2.value.code == common_pb2.ERROR_CODE_CANCELLED

    def test_cancel_policy_on_the_incoming_final_frame_leaves_no_closed_entry(
        self,
    ) -> None:
        # The monitor's own final frame arrives at the limit: the overflow
        # cancels that same stream, and since nothing more will ever come
        # for it, its id must not stay in the closed set.
        client, _ = _connected_client_with_sock(
            _monitor_frame(continuation=True, memory_bytes=1),
            _monitor_frame(continuation=True, memory_bytes=2),
            _monitor_frame(continuation=False, exited=True, memory_bytes=3),
            _version_response_bytes(stream_id=2),
            backlog=Backlog(max_frames=2),
        )
        it = client.process.monitor(42)
        client.version()
        assert it.stream_id not in client._dispatcher._closed
        with pytest.raises(TazError) as exc_info:
            next(it)
        assert exc_info.value.code == common_pb2.ERROR_CODE_CANCELLED

    def test_drop_oldest_policy_keeps_stream_open_and_drops_oldest(self) -> None:
        client, _ = _connected_client_with_sock(
            _monitor_frame(continuation=True, memory_bytes=1),
            _monitor_frame(continuation=True, memory_bytes=2),
            _monitor_frame(continuation=True, memory_bytes=3),
            _monitor_frame(continuation=False, exited=True, memory_bytes=4),
            _version_response_bytes(stream_id=2),
            backlog=Backlog(max_frames=2),
        )
        it = client.process.monitor(42, overflow="drop_oldest")
        client.version()

        assert it.dropped == 2
        updates = list(it)
        assert [u.info.memory_bytes for u in updates] == [3, 4]


# ---------------------------------------------------------------------------
# max_wait
# ---------------------------------------------------------------------------


class TestMonitorMaxWait:
    def test_expiry_raises_timeout_cancels_and_leaves_connection_usable(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _cancel_response_bytes(True, stream_id=2),
            _pong_bytes(),
        )
        it = client.process.monitor(42, max_wait=0.05)
        # Only the monitor's own wait must really elapse (and find nothing
        # readable) to prove the deadline; later waits (the CANCEL response,
        # then the PONG) must see the data actually queued in the mock.
        calls = {"n": 0}

        def fake_wait(sock: socket.socket, timeout: float | None) -> bool:
            calls["n"] += 1
            if calls["n"] == 1:
                time.sleep(timeout if timeout is not None else 0)
                return False
            return _real_wait_readable(sock, timeout)

        client._dispatcher._wait = fake_wait

        with pytest.raises(TazError) as exc_info:
            next(it)
        assert exc_info.value.code == common_pb2.ERROR_CODE_TIMEOUT
        # CANCEL went out without waiting for its reply: when the daemon is
        # what stalled, waiting would turn a bounded wait into an unbounded
        # one. Its verdict is therefore unknown.
        assert calls["n"] == 1
        assert it.cancelled is None

        cancel_requests = [
            f for f in _requests_sent(mock_sock) if f.opcode == common_pb2.OPCODE_CANCEL
        ]
        assert len(cancel_requests) == 1
        client.ping()

    def test_mid_frame_stall_raises_connection_lost(self) -> None:
        client, mock_sock = _connected_client_with_sock()
        it = client.process.monitor(42, max_wait=5.0)

        header = pack_header(
            Frame(
                type=common_pb2.FRAME_TYPE_RESPONSE,
                flags=0,
                opcode=common_pb2.OPCODE_PROCESS_MONITOR,
                length=10,
                stream_id=it.stream_id,
            )
        )

        calls = {"n": 0}
        first_byte = header[:1]

        def recv(bufsize: int, flags: int = 0) -> bytes:
            if flags & socket.MSG_PEEK:
                return first_byte
            calls["n"] += 1
            if calls["n"] == 1:
                return first_byte
            raise TimeoutError

        mock_sock.recv.side_effect = recv
        mock_sock.gettimeout.return_value = 5.0

        with pytest.raises(TazConnectionLost) as exc_info:
            next(it)
        assert "mid-frame" in exc_info.value.message
        assert client._conn.closed
        assert it.ended

    def test_with_block_after_connection_loss_keeps_the_original_error(
        self,
    ) -> None:
        # EOF right after the handshake: next() loses the connection, and
        # __exit__ must not replace that error with a failed CANCEL's.
        client, mock_sock = _connected_client_with_sock()
        with (
            pytest.raises(TazConnectionLost) as exc_info,
            client.process.monitor(42) as it,
        ):
            next(it)
        assert exc_info.value.__context__ is None
        assert client._conn.closed
        assert [f.opcode for f in _requests_sent(mock_sock)] == [
            common_pb2.OPCODE_PROCESS_MONITOR
        ]
        assert it.ended


# ---------------------------------------------------------------------------
# Packaging / exports
# ---------------------------------------------------------------------------


class TestExports:
    def test_stream_iterator_and_update_are_exported(self) -> None:
        assert "StreamIterator" in taz.c3.__all__
        assert "ProcessMonitorUpdate" in taz.c3.__all__
        assert taz.c3.StreamIterator is StreamIterator
        assert taz.c3.ProcessMonitorUpdate is ProcessMonitorUpdate
