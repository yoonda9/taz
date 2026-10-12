"""Dispatch loop: pending-frame buffer, per-stream backlog, keepalive probing."""

from __future__ import annotations

import collections
import dataclasses
import logging
import socket
import time
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING, Literal

from google.protobuf.descriptor import FieldDescriptor
from google.protobuf.message import Message

from taz.c3.errors import (
    TazConnectionLost,
    TazError,
    TazPriorityError,
    TazProtocolError,
)
from taz.c3.protocol.frame import DEFAULT_MAX_PAYLOAD, Frame, wait_readable
from taz.c3.settings import Backlog, Keepalive
from taz.v1 import advanced_pb2, common_pb2

if TYPE_CHECKING:
    from taz.c3.connection import Connection

_DEFAULT_BACKLOG: Backlog = Backlog()

_log = logging.getLogger(__name__)

Overflow = Literal["cancel", "drop_oldest"]
PriorityPolicy = Literal["raise", "queue"]

# No stream_id was ever registered for the task (the client never set up a
# mapping, e.g. the frame arrived for a stream this Dispatcher never heard
# of through register_priority_task). 0 is not a valid task_id (daemon
# task_ids are a monotonic counter starting at 1).
_UNKNOWN_TASK_ID = 0


class MaxWaitExpired(Exception):  # noqa: N818
    """Internal: a recv_response ``max_wait`` deadline elapsed with no frame.

    Not a TazError — callers that offer a public ``max_wait`` (StreamIterator)
    catch this and raise ``TazError(TIMEOUT, ...)`` themselves.
    """


@dataclasses.dataclass
class _StreamState:
    """Per-stream backlog bookkeeping for a registered stream."""

    overflow: Overflow = "cancel"
    dropped: int = 0
    error: TazError | None = None


def merge_chunks[M: Message](message_type: type[M], payloads: Sequence[bytes]) -> M:
    """Merge the payloads of a chunked response into one message (§6.1).

    Each payload is a well-formed ``message_type``.  bytes and string fields
    are concatenated and repeated fields appended in payload order; scalars
    come from the final payload.  Singular sub-messages merge by the same rules.
    """
    parts: list[M] = []
    for payload in payloads:
        part = message_type()
        part.ParseFromString(payload)
        parts.append(part)
    merged = message_type()
    _merge_parts(merged, parts)
    return merged


def _merge_parts(dst: Message, parts: Sequence[Message]) -> None:
    # Protobuf's own merge keeps only the last value of a singular bytes or
    # string field, so each field is merged explicitly.
    dst.CopyFrom(parts[-1])
    for fd in dst.DESCRIPTOR.fields:
        name = fd.name
        if fd.is_repeated:
            dst.ClearField(name)
            repeated = getattr(dst, name)
            for part in parts:
                repeated.extend(getattr(part, name))
        elif fd.type == FieldDescriptor.TYPE_BYTES:
            data = [getattr(part, name) for part in parts]
            if any(data):
                setattr(dst, name, b"".join(data))
        elif fd.type == FieldDescriptor.TYPE_STRING:
            text = [getattr(part, name) for part in parts]
            if any(text):
                setattr(dst, name, "".join(text))
        elif fd.type == FieldDescriptor.TYPE_MESSAGE:
            present = [getattr(part, name) for part in parts if part.HasField(name)]
            if present:
                _merge_parts(getattr(dst, name), present)


class Dispatcher:
    """Per-connection dispatch loop with buffering, backlog, and keepalive.

    Drives the receive side of a single Connection: buffers frames for streams
    other than the one currently being awaited, enforces the bounded backlog,
    and sends keepalive PINGs when the socket is idle.
    """

    def __init__(
        self,
        conn: Connection,
        backlog: Backlog | None = None,
        *,
        priority_policy: PriorityPolicy = "raise",
        on_priority_error: Callable[[TazPriorityError], None] | None = None,
        _wait_readable: (Callable[[socket.socket, float | None], bool] | None) = None,
    ) -> None:
        self._conn = conn
        self._backlog = backlog if backlog is not None else _DEFAULT_BACKLOG
        # Per-stream pending frames.
        self._buffer: dict[int, collections.deque[Frame]] = {}
        # Stream IDs whose frames are silently discarded (§10.2).
        self._closed: set[int] = set()
        # Per-stream overflow policy / dropped count / recorded backlog error,
        # for streams a StreamIterator has registered.
        self._streams: dict[int, _StreamState] = {}
        self._total_frames: int = 0
        self._total_bytes: int = 0
        self._wait: Callable[[socket.socket, float | None], bool] = (
            _wait_readable if _wait_readable is not None else wait_readable
        )
        # PRIORITY (protocol.md §10.2): the originating DETACH stream_id for
        # a task the caller wants attributed, set via register_priority_task.
        self._priority_tasks: dict[int, int] = {}
        # policy="raise" errors seen while no discrete call was in flight
        # (a StreamIterator/file transfer was reading, or nothing was),
        # held until the next discrete call raises them before sending.
        self._pending_priority: collections.deque[TazPriorityError] = (
            collections.deque()
        )
        self.priority_policy: PriorityPolicy = priority_policy
        self.on_priority_error = on_priority_error
        # policy="queue": every PRIORITY error ever seen, appended in order.
        self.priority_errors: list[TazPriorityError] = []
        # Discard a closed-set entry (and any stale buffer) the
        # moment its id is reused after the counter wraps.
        conn.stream_assigned_hook = self._forget_closed

    # ------------------------------------------------------------------
    # Stream lifecycle
    # ------------------------------------------------------------------

    def close_stream(self, stream_id: int) -> None:
        """Mark stream_id as closed; buffered frames for it are freed."""
        self._closed.add(stream_id)
        removed = self._buffer.pop(stream_id, collections.deque())
        for frame in removed:
            self._total_frames -= 1
            self._total_bytes -= len(frame.payload)

    def is_closed(self, stream_id: int) -> bool:
        """True if stream_id is in the closed-stream set right now."""
        return stream_id in self._closed

    def close_after_cancel(self, stream_id: int, was_already_closed: bool) -> None:
        """Close stream_id following a successful CANCEL response.

        ``was_already_closed`` is the id's ``is_closed`` state from just
        before the CANCEL request was sent (``StreamIterator.close()``
        pre-closes before cancelling). If it was already closed and the
        closed-stream leave rule removed it while this call waited for the
        response, the stream's true final frame was already seen and
        nothing more will ever arrive for it - re-adding it here would
        strand the id in the closed set forever instead of letting the
        leave rule finish its job.
        """
        if was_already_closed and stream_id not in self._closed:
            return
        self.close_stream(stream_id)

    def register_stream(self, stream_id: int, overflow: Overflow) -> None:
        """Start per-stream backlog bookkeeping for a newly opened stream."""
        self._streams[stream_id] = _StreamState(overflow=overflow)

    def forget_stream(self, stream_id: int) -> None:
        """Drop a stream's per-stream bookkeeping (it has ended)."""
        self._streams.pop(stream_id, None)

    def stream_error(self, stream_id: int) -> TazError | None:
        """The backlog's recorded CANCELLED error for stream_id, if any."""
        state = self._streams.get(stream_id)
        return None if state is None else state.error

    def stream_dropped(self, stream_id: int) -> int:
        """Frames the backlog's drop_oldest policy has dropped for stream_id."""
        state = self._streams.get(stream_id)
        return 0 if state is None else state.dropped

    # ------------------------------------------------------------------
    # PRIORITY (protocol.md §10.2)
    # ------------------------------------------------------------------

    def register_priority_task(self, stream_id: int, task_id: int) -> None:
        """Attribute a later PRIORITY ERROR on stream_id to task_id.

        Called once a DETACH response for stream_id is in hand (the
        request's own stream_id is also the one a CRITICAL-priority FAILED
        escalation for that task arrives on). A task escalates at most
        once, so the mapping is consumed by the first PRIORITY frame seen
        for stream_id.
        """
        self._priority_tasks[stream_id] = task_id

    def raise_pending_priority(self) -> None:
        """Raise the oldest PRIORITY error deferred under policy "raise".

        A discrete call runs this before sending its request, so a deferred
        error never leaves that call's request running on the daemon with
        nobody to read its response. A no-op when nothing is deferred.
        """
        if self._pending_priority:
            raise self._pending_priority.popleft()

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _priority_error_from_frame(self, frame: Frame) -> TazPriorityError:
        info = common_pb2.ErrorInfo()
        info.ParseFromString(frame.payload)
        task_id = self._priority_tasks.pop(frame.stream_id, _UNKNOWN_TASK_ID)
        error = TazError(info.code, info.message, info.detail)
        return TazPriorityError(task_id, frame.stream_id, error)

    def _handle_priority_frame(
        self, frame: Frame, expected: int, *, discrete: bool
    ) -> None:
        """Resolve one PRIORITY-flagged ERROR frame per priority_policy.

        ``policy="queue"`` always records it (never raises) and calls
        ``on_priority_error``; an exception from that callback is logged and
        ignored, never a failure of the call that happened to be reading.
        Otherwise (the default, "raise"): a discrete call in flight raises
        it right away, closing its own ``expected`` stream so the real, late
        RESPONSE for that call is discarded (§10.2) rather than misdelivered
        to whatever next reuses the id; anything else (a StreamIterator, a
        file transfer, a cleanup CANCEL, or no call in flight at all) defers
        it to the next discrete call instead of interrupting a transfer that
        is not equipped to handle it mid-stream.
        """
        error = self._priority_error_from_frame(frame)
        if self.priority_policy == "queue":
            self.priority_errors.append(error)
            if self.on_priority_error is not None:
                try:
                    self.on_priority_error(error)
                except Exception:
                    _log.exception("on_priority_error raised; ignored")
            return
        if discrete:
            self.close_stream(expected)
            raise error
        self._pending_priority.append(error)

    def _forget_closed(self, stream_id: int) -> None:
        """Discard a stale closed-set entry/buffer for a just-reused id."""
        self._closed.discard(stream_id)
        removed = self._buffer.pop(stream_id, collections.deque())
        for frame in removed:
            self._total_frames -= 1
            self._total_bytes -= len(frame.payload)
        # A stale task_id mapping would otherwise misattribute a PRIORITY
        # frame meant for the old task to whatever reuses this stream_id.
        self._priority_tasks.pop(stream_id, None)

    @staticmethod
    def _ends_stream(frame: Frame) -> bool:
        """True once frame is the last one a stream will ever send (§11)."""
        if frame.type == common_pb2.FRAME_TYPE_ERROR:
            return True
        if frame.type in (
            common_pb2.FRAME_TYPE_RESPONSE,
            common_pb2.FRAME_TYPE_FILE_CHUNK,
        ):
            return not frame.flags & common_pb2.FRAME_FLAG_CONTINUATION
        return False

    def _pop_buffered(self, stream_id: int) -> Frame | None:
        q = self._buffer.get(stream_id)
        if not q:
            return None
        frame = q.popleft()
        self._total_frames -= 1
        self._total_bytes -= len(frame.payload)
        if not q:
            del self._buffer[stream_id]
        return frame

    def _guarded_wait(self, sock: socket.socket, timeout: float | None) -> bool:
        """Wait for readability under the connection's failure guard.

        An EOF or socket error seen while waiting closes the connection and
        is recorded, exactly as one seen while reading a frame is.
        """
        with self._conn._io():
            return self._wait(sock, timeout)

    def cancel_without_reply(self, stream_id: int) -> None:
        """Close stream_id and send CANCEL for it without waiting.

        The CANCEL's own response is discarded when it arrives. Nothing is
        sent if the daemon does not advertise CANCEL.
        """
        if self._cancel_advertised():
            cancel_payload = advanced_pb2.CancelRequest(
                target_stream_id=stream_id
            ).SerializeToString()
            cancel_stream = self._conn.send_request(
                common_pb2.OPCODE_CANCEL, cancel_payload
            )
            self._closed.add(cancel_stream)
        self.close_stream(stream_id)

    def _cancel_advertised(self) -> bool:
        try:
            ops = self._conn.capabilities.operations
        except RuntimeError:
            return False
        return common_pb2.OPCODE_CANCEL in ops

    def _cancel_victim(self, victim_id: int) -> None:
        """Overflow policy "cancel": send CANCEL, drop frames, close, record."""
        self.cancel_without_reply(victim_id)

        error = TazError(
            common_pb2.ERROR_CODE_CANCELLED,
            f"backlog limit reached ({self._backlog.max_frames} frames /"
            f" {self._backlog.max_bytes} bytes) on stream {victim_id};"
            " cancelled — use a slower interval",
        )
        state = self._streams.get(victim_id)
        if state is None:
            self._streams[victim_id] = _StreamState(overflow="cancel", error=error)
        else:
            state.error = error

    def _drop_oldest_frame(self, victim_id: int) -> bool:
        """Overflow policy "drop_oldest": drop the oldest non-final frame.

        Returns False (no frame dropped) when every buffered frame for
        victim_id is a final frame (the search then tries the next
        most-buffered stream).
        """
        q = self._buffer.get(victim_id)
        if not q:
            return False
        for index, frame in enumerate(q):
            if not frame.flags & common_pb2.FRAME_FLAG_CONTINUATION:
                continue
            del q[index]
            self._total_frames -= 1
            self._total_bytes -= len(frame.payload)
            if not q:
                del self._buffer[victim_id]
            state = self._streams.get(victim_id)
            if state is None:
                state = _StreamState(overflow="drop_oldest")
                self._streams[victim_id] = state
            state.dropped += 1
            return True
        return False

    def _overflow(self) -> None:
        """Apply each victim's own overflow policy until one yields room.

        Victims are tried most-buffered first (ties broken by the lower
        stream_id); if none can yield a frame, the incoming frame is simply
        buffered anyway, overshooting the limit by one frame.
        """
        candidates = sorted(
            self._buffer,
            key=lambda stream_id: (-len(self._buffer[stream_id]), stream_id),
        )
        for victim_id in candidates:
            state = self._streams.get(victim_id)
            policy: Overflow = "cancel" if state is None else state.overflow
            if policy == "drop_oldest":
                if self._drop_oldest_frame(victim_id):
                    return
                continue
            self._cancel_victim(victim_id)
            return

    def _add_to_backlog(self, frame: Frame) -> None:
        """Buffer frame, triggering overflow policy if the backlog is full."""
        if (
            self._total_frames >= self._backlog.max_frames
            or self._total_bytes + len(frame.payload) > self._backlog.max_bytes
        ):
            self._overflow()

        # The overflow may have closed this frame's stream. If this was the
        # stream's last frame, nothing more will come to trigger the
        # closed-set leave rule, so apply it now.
        if frame.stream_id in self._closed:
            if self._ends_stream(frame):
                self._closed.discard(frame.stream_id)
            return

        q = self._buffer.setdefault(frame.stream_id, collections.deque())
        q.append(frame)
        self._total_frames += 1
        self._total_bytes += len(frame.payload)

    # ------------------------------------------------------------------
    # PONG waiter (used by TazClient.ping())
    # ------------------------------------------------------------------

    def recv_pong(self, keepalive: Keepalive) -> None:
        """Block until a PONG frame arrives, buffering any other live frames.

        Called immediately after Connection._send_ping(); the caller already
        sent the PING so the keepalive.timeout is the only relevant wait.
        """
        # A connection lost earlier (perhaps while reading for another
        # stream) re-raises that loss, not a bare "not connected".
        self._conn._check_failure()
        sock = self._conn._sock
        if sock is None:
            raise RuntimeError("not connected")

        raw_wait: float | None = keepalive.timeout
        if raw_wait is not None and raw_wait == float("inf"):
            raw_wait = None

        while True:
            if not self._guarded_wait(sock, raw_wait):
                self._conn.close()
                raise TazConnectionLost(
                    f"no PONG within {keepalive.timeout:g} s; connection presumed dead"
                )

            frame = self._conn.recv_frame()

            if frame.type == common_pb2.FRAME_TYPE_PONG:
                return

            if frame.type not in DEFAULT_MAX_PAYLOAD:
                continue

            if frame.type == common_pb2.FRAME_TYPE_ERROR and (
                frame.flags & common_pb2.FRAME_FLAG_PRIORITY
            ):
                # recv_pong has no discrete call of its own in flight.
                self._handle_priority_frame(frame, -1, discrete=False)
                continue

            if frame.stream_id in self._closed:
                if self._ends_stream(frame):
                    self._closed.discard(frame.stream_id)
                continue

            self._add_to_backlog(frame)

    # ------------------------------------------------------------------
    # Main dispatch loop
    # ------------------------------------------------------------------

    def recv_response(
        self,
        expected: int,
        keepalive: Keepalive,
        *,
        expected_opcode: int | None = None,
        response_type: type[Message] | None = None,
        max_wait: float | None = None,
        discrete: bool = True,
    ) -> Frame:
        """Receive the next RESPONSE or ERROR frame for ``expected`` stream_id.

        Frames for other streams are buffered.  ``expected_opcode`` (if given)
        is validated against the echoed opcode on RESPONSE/ERROR frames.

        With ``response_type``, a chunked response is collected up to the
        RESPONSE frame without FRAME_FLAG_CONTINUATION and merged per §6.1; the
        final frame is returned carrying the merged message.  An ERROR frame
        ends the response and is returned as it arrived.  Without
        ``response_type`` every frame is returned on its own, as a streamed
        response needs.

        ``max_wait``, if given, bounds only the wait for the *next* frame to
        start arriving; one deadline is set for the whole call (not reset by
        frames for other streams or by PONGs) and ``MaxWaitExpired`` is raised
        if it elapses.

        ``discrete`` (default ``True``) marks this as a standalone
        request/response call (``TazClient._call``), as opposed to one leg
        of a ``StreamIterator`` or a file transfer, or a cleanup CANCEL
        (all pass ``False``).
        Under ``priority_policy="raise"`` (the default), only a discrete
        call raises a PRIORITY error as it arrives; the other callers defer
        it to the next discrete call instead, per docs/api.md §5.4.
        """
        deadline = None if max_wait is None else time.monotonic() + max_wait
        frame = self._next_frame(
            expected, keepalive, expected_opcode, deadline, discrete
        )
        if response_type is None:
            return frame
        chunks: list[bytes] = []
        while (
            frame.type == common_pb2.FRAME_TYPE_RESPONSE
            and frame.flags & common_pb2.FRAME_FLAG_CONTINUATION
        ):
            chunks.append(frame.payload)
            frame = self._next_frame(
                expected, keepalive, expected_opcode, deadline, discrete
            )
        if not chunks or frame.type != common_pb2.FRAME_TYPE_RESPONSE:
            return frame
        chunks.append(frame.payload)
        payload = merge_chunks(response_type, chunks).SerializeToString()
        return dataclasses.replace(frame, length=len(payload), payload=payload)

    def _next_frame(
        self,
        expected: int,
        keepalive: Keepalive,
        expected_opcode: int | None,
        deadline: float | None = None,
        discrete: bool = True,
    ) -> Frame:
        """Return the next frame for ``expected``, buffered or read."""
        buffered = self._pop_buffered(expected)
        if buffered is not None:
            return buffered

        # A connection lost earlier (perhaps while reading for another
        # stream) re-raises that loss, not a bare "not connected".
        self._conn._check_failure()
        sock = self._conn._sock
        if sock is None:
            raise RuntimeError("not connected")

        probing = False
        while True:
            raw_wait: float | None = keepalive.timeout if probing else keepalive.idle
            # Normalize infinity (Keepalive.OFF or explicit inf) to None.
            if raw_wait is not None and raw_wait == float("inf"):
                raw_wait = None

            wait = raw_wait
            capped_by_deadline = False
            if deadline is not None:
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    raise MaxWaitExpired
                if raw_wait is None or remaining < raw_wait:
                    wait = remaining
                    capped_by_deadline = True
                else:
                    wait = raw_wait

            readable = self._guarded_wait(sock, wait)
            if not readable:
                if capped_by_deadline:
                    # wait was already set to exactly the remaining time, so
                    # a not-readable result means the deadline is up. Trust
                    # that outcome directly instead of re-deriving it from a
                    # second time.monotonic() call: a real OS sleep can
                    # return a few ms before its nominal duration (observed
                    # on Windows), which would make such a re-check flicker
                    # and misreport this as a dead connection instead.
                    raise MaxWaitExpired
                if probing:
                    self._conn.close()
                    raise TazConnectionLost(
                        f"no PONG within {keepalive.timeout:g} s;"
                        " connection presumed dead"
                    )
                # Idle timeout expired: probe with PING.
                self._conn._send_ping()
                probing = True
                continue

            frame = self._conn.recv_frame()
            probing = False  # any frame counts as liveness

            # Unknown frame type: drop and continue (§10.1).
            if frame.type not in DEFAULT_MAX_PAYLOAD:
                continue

            # PONG: keepalive probe resolved; keep waiting for the real response.
            if frame.type == common_pb2.FRAME_TYPE_PONG:
                continue

            # PRIORITY (§10.2): checked before the closed-set/backlog logic,
            # since it can arrive on a stream (the task's DETACH stream)
            # this loop already treats as finished - and is never itself
            # the frame `expected` is waiting for, even if the ids match.
            if frame.type == common_pb2.FRAME_TYPE_ERROR and (
                frame.flags & common_pb2.FRAME_FLAG_PRIORITY
            ):
                self._handle_priority_frame(frame, expected, discrete=discrete)
                continue

            # Closed stream: discard (§10.2), leaving the set on its last frame.
            if frame.stream_id in self._closed:
                if self._ends_stream(frame):
                    self._closed.discard(frame.stream_id)
                continue

            # Expected stream: validate echoed opcode and return.
            if frame.stream_id == expected:
                if (
                    expected_opcode is not None
                    and frame.type
                    in (
                        common_pb2.FRAME_TYPE_RESPONSE,
                        common_pb2.FRAME_TYPE_ERROR,
                    )
                    and frame.opcode != expected_opcode
                ):
                    raise TazProtocolError(
                        f"echoed opcode 0x{frame.opcode:04x} !="
                        f" expected 0x{expected_opcode:04x}"
                    )
                return frame

            # Frame for another live stream: buffer it.
            self._add_to_backlog(frame)
