"""Tests for the dispatch loop's PRIORITY handling (protocol.md §10.2)."""

from __future__ import annotations

import logging
import socket
from collections.abc import Callable
from typing import Literal
from unittest.mock import MagicMock, patch

import pytest
import taz.c3
from taz.c3.client import TazClient
from taz.c3.connection import Connection
from taz.c3.errors import TazPriorityError
from taz.c3.protocol.dispatch import Dispatcher
from taz.c3.protocol.frame import Frame, pack_header
from taz.c3.settings import Backlog, Keepalive
from taz.c3.task import TaskState
from taz.v1 import advanced_pb2, command_pb2, common_pb2, daemon_control_pb2

# ---------------------------------------------------------------------------
# Test helpers (same pattern as test_dispatch.py)
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


def _priority_error(
    stream_id: int,
    *,
    opcode: int = common_pb2.OPCODE_DETACH,
    code: common_pb2.ErrorCode = common_pb2.ERROR_CODE_INTERNAL,
    message: str = "exit code 1",
) -> bytes:
    """A FRAME_FLAG_PRIORITY ERROR frame (protocol.md §10.2)."""
    info = common_pb2.ErrorInfo(code=code, message=message)
    return _frame_bytes(
        common_pb2.FRAME_TYPE_ERROR,
        info.SerializeToString(),
        opcode=opcode,
        stream_id=stream_id,
        flags=common_pb2.FRAME_FLAG_PRIORITY,
    )


def _cont_frame(stream_id: int, payload: bytes) -> bytes:
    """One non-final RESPONSE frame (CONTINUATION set) for a streamed op."""
    return _frame_bytes(
        common_pb2.FRAME_TYPE_RESPONSE,
        payload,
        stream_id=stream_id,
        flags=common_pb2.FRAME_FLAG_CONTINUATION,
    )


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
# TestPriorityDispatch: FRAME_FLAG_PRIORITY ERROR frames (protocol.md §10.2)
# ---------------------------------------------------------------------------


class TestPriorityDispatch:
    def test_raise_policy_interrupts_discrete_call(self) -> None:
        """Default policy: a PRIORITY frame raises from the waiting call."""
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        _, _, d = _dispatcher(priority, resp1)
        d.register_priority_task(5, task_id=42)

        with pytest.raises(TazPriorityError) as exc_info:
            d.recv_response(1, Keepalive.OFF)

        err = exc_info.value
        assert err.task_id == 42
        assert err.stream_id == 5
        assert err.error.code == common_pb2.ERROR_CODE_INTERNAL
        assert err.error.message == "exit code 1"

    def test_raise_policy_closes_the_interrupted_calls_own_stream(self) -> None:
        """The in-flight call's own stream joins the closed set, not the
        PRIORITY frame's stream."""
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        _, _, d = _dispatcher(priority, resp1)
        d.register_priority_task(5, task_id=42)

        with pytest.raises(TazPriorityError):
            d.recv_response(1, Keepalive.OFF)

        assert 1 in d._closed
        assert 5 not in d._closed

    def test_unregistered_stream_reports_task_id_zero(self) -> None:
        """A PRIORITY frame for a stream nobody registered still raises."""
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        _, _, d = _dispatcher(priority, resp1)

        with pytest.raises(TazPriorityError) as exc_info:
            d.recv_response(1, Keepalive.OFF)

        assert exc_info.value.task_id == 0

    def test_registration_consumed_by_first_priority_frame(self) -> None:
        """The stream_id -> task_id mapping is popped once used."""
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        _, _, d = _dispatcher(priority, resp1)
        d.register_priority_task(5, task_id=42)

        with pytest.raises(TazPriorityError):
            d.recv_response(1, Keepalive.OFF)

        assert 5 not in d._priority_tasks

    def test_priority_on_the_expected_stream_id_still_raises(self) -> None:
        """A PRIORITY frame is never treated as the awaited response, even
        if its stream_id happens to equal `expected`."""
        priority = _priority_error(stream_id=1)
        _, _, d = _dispatcher(priority)
        d.register_priority_task(1, task_id=9)

        with pytest.raises(TazPriorityError) as exc_info:
            d.recv_response(1, Keepalive.OFF)

        assert exc_info.value.task_id == 9

    def test_priority_on_an_already_closed_stream_still_delivered(self) -> None:
        """The PRIORITY check precedes the closed-set logic: a PRIORITY
        frame arriving on a DETACH stream the client already treats as
        finished (closed) is still raised/delivered, not silently
        discarded."""
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        _, _, d = _dispatcher(priority, resp1)
        d.register_priority_task(5, task_id=42)
        d.close_stream(5)

        with pytest.raises(TazPriorityError) as exc_info:
            d.recv_response(1, Keepalive.OFF)

        assert exc_info.value.task_id == 42
        assert exc_info.value.stream_id == 5

    def test_non_discrete_call_defers_instead_of_raising(self) -> None:
        """A StreamIterator/file-transfer read (discrete=False) is not
        interrupted; the error is queued for the next discrete call."""
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        _, _, d = _dispatcher(priority, resp1)
        d.register_priority_task(5, task_id=42)

        frame = d.recv_response(1, Keepalive.OFF, discrete=False)

        assert frame.stream_id == 1
        assert len(d._pending_priority) == 1
        assert d._pending_priority[0].task_id == 42

    def test_deferred_priority_waits_for_raise_pending_priority(self) -> None:
        """recv_response never raises a deferred error itself (a discrete
        TazClient call raises it before sending); raise_pending_priority
        raises it once, oldest first."""
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        resp2 = _response(stream_id=2)
        _, _, d = _dispatcher(priority, resp1, resp2)
        d.register_priority_task(5, task_id=42)

        d.recv_response(1, Keepalive.OFF, discrete=False)
        assert len(d._pending_priority) == 1
        assert d.recv_response(2, Keepalive.OFF).stream_id == 2

        with pytest.raises(TazPriorityError) as exc_info:
            d.raise_pending_priority()
        assert exc_info.value.task_id == 42
        d.raise_pending_priority()  # nothing left: a no-op

    def test_recv_pong_defers_priority_without_raising(self) -> None:
        """recv_pong has no discrete call of its own; it always defers."""
        priority = _priority_error(stream_id=5)
        pong = _pong()
        _, _, d = _dispatcher(priority, pong)
        d.register_priority_task(5, task_id=7)

        d.recv_pong(Keepalive.OFF)

        assert len(d._pending_priority) == 1
        assert d._pending_priority[0].task_id == 7

    def test_queue_policy_never_raises(self) -> None:
        """priority_policy="queue": recorded, never raised, even discrete."""
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        conn, _ = _connected(priority, resp1)
        d = Dispatcher(conn, priority_policy="queue")
        d.register_priority_task(5, task_id=42)

        frame = d.recv_response(1, Keepalive.OFF)

        assert frame.stream_id == 1
        assert len(d.priority_errors) == 1
        assert d.priority_errors[0].task_id == 42
        assert not d._pending_priority

    def test_queue_policy_invokes_callback(self) -> None:
        priority = _priority_error(stream_id=5)
        resp1 = _response(stream_id=1)
        conn, _ = _connected(priority, resp1)
        seen: list[TazPriorityError] = []
        d = Dispatcher(conn, priority_policy="queue", on_priority_error=seen.append)
        d.register_priority_task(5, task_id=42)

        d.recv_response(1, Keepalive.OFF)

        assert len(seen) == 1
        assert seen[0].task_id == 42

    def test_stream_id_reuse_discards_stale_task_mapping(self) -> None:
        """A wrapped-counter id reuse forgets the old task_id attribution."""
        conn, _, d = _dispatcher(operations=[common_pb2.OPCODE_PING])
        d.register_priority_task(1, task_id=99)
        conn._next_stream_id = 0xFFFFFFFF
        conn.send_request(common_pb2.OPCODE_PING, b"")  # claims 0xFFFFFFFF

        reused = conn.send_request(common_pb2.OPCODE_PING, b"")  # wraps to 1

        assert reused == 1
        assert 1 not in d._priority_tasks

    def test_taz_priority_error_exported(self) -> None:
        assert "TazPriorityError" in taz.c3.__all__
        assert taz.c3.TazPriorityError is TazPriorityError

    def test_raise_policy_interrupts_a_chunked_merge_in_progress(self) -> None:
        """A PRIORITY frame mid-merge raises without waiting for the rest."""
        first = _cont_frame(1, b"partial")
        priority = _priority_error(stream_id=5)
        _, _, d = _dispatcher(first, priority)
        d.register_priority_task(5, task_id=3)

        with pytest.raises(TazPriorityError) as exc_info:
            d.recv_response(
                1,
                Keepalive.OFF,
                response_type=command_pb2.CommandExecResponse,
            )

        assert exc_info.value.task_id == 3


# ---------------------------------------------------------------------------
# TestPriorityClient: how TazClient's discrete calls and cleanup CANCELs
# treat a PRIORITY error
# ---------------------------------------------------------------------------


def _client(
    *extra_chunks: bytes,
    priority_policy: Literal["raise", "queue"] = "raise",
    on_priority_error: Callable[[TazPriorityError], None] | None = None,
) -> tuple[TazClient, MagicMock]:
    cap = _capability_bytes(
        operations=[common_pb2.OPCODE_CANCEL, common_pb2.OPCODE_TASK_STATUS]
    )
    sock = _mock_sock(cap, *extra_chunks)
    client = TazClient(
        "127.0.0.1",
        5555,
        keepalive=Keepalive.OFF,
        priority_policy=priority_policy,
        on_priority_error=on_priority_error,
    )
    with patch("taz.c3.connection.socket.create_connection", return_value=sock):
        client.connect()
    return client, sock


def _sends(sock: MagicMock) -> int:
    return int(sock.sendmsg.call_count + sock.sendall.call_count)


def _task_status_response(stream_id: int) -> bytes:
    resp = advanced_pb2.TaskStatusResponse(state=advanced_pb2.TASK_STATE_RUNNING)
    return _response(common_pb2.OPCODE_TASK_STATUS, stream_id, resp.SerializeToString())


def _cancel_response(stream_id: int) -> bytes:
    resp = advanced_pb2.CancelResponse(cancelled=True)
    return _response(common_pb2.OPCODE_CANCEL, stream_id, resp.SerializeToString())


def _deferred(task_id: int) -> TazPriorityError:
    error = taz.c3.TazError(common_pb2.ERROR_CODE_INTERNAL, "exit code 1")
    return TazPriorityError(task_id, 9, error)


class TestPriorityClient:
    def test_deferred_error_is_raised_before_the_next_call_sends(self) -> None:
        """The interrupted call never reaches the daemon, so nothing runs
        there with nobody left to read its response."""
        client, sock = _client(_task_status_response(stream_id=1))
        client._dispatcher._pending_priority.append(_deferred(42))
        before = _sends(sock)

        with pytest.raises(TazPriorityError) as exc_info:
            client.task.status(1)

        assert exc_info.value.task_id == 42
        assert _sends(sock) == before
        # The next call is the first to send, on stream 1, and succeeds.
        assert client.task.status(1).state == TaskState.RUNNING
        assert not client._dispatcher._buffer

    def test_closing_a_stream_leaves_a_deferred_error_queued(self) -> None:
        """StreamIterator.close() (and so a with block) never raises a
        deferred error, so it cannot replace the user's own exception."""
        client, _ = _client(_cancel_response(stream_id=2))
        client._dispatcher._pending_priority.append(_deferred(42))

        with (
            pytest.raises(ValueError, match="user code"),
            client.task.status(1, subscribe=True) as it,
        ):
            raise ValueError("user code")

        assert it.cancelled is True
        assert len(client._dispatcher._pending_priority) == 1

    def test_priority_arriving_during_a_cleanup_cancel_is_deferred(self) -> None:
        client, _ = _client(_priority_error(stream_id=5), _cancel_response(2))
        client._dispatcher.register_priority_task(5, task_id=7)

        it = client.task.status(1, subscribe=True)
        it.close()

        assert it.cancelled is True
        assert [e.task_id for e in client._dispatcher._pending_priority] == [7]
        with pytest.raises(TazPriorityError):
            client.task.status(1)

    def test_failing_callback_is_logged_and_keeps_the_connection(
        self, caplog: pytest.LogCaptureFixture
    ) -> None:
        def buggy(error: TazPriorityError) -> None:
            raise RuntimeError("buggy callback")

        client, _ = _client(
            _priority_error(stream_id=5),
            _task_status_response(stream_id=1),
            priority_policy="queue",
            on_priority_error=buggy,
        )

        with caplog.at_level(logging.ERROR, logger="taz.c3.protocol.dispatch"):
            status = client.task.status(1)

        assert "on_priority_error raised" in caplog.text
        assert "buggy callback" in caplog.text
        assert status.state == TaskState.RUNNING
        assert not client._conn.closed
        assert len(client.priority_errors) == 1
