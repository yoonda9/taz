"""Dispatch loop: pending-frame buffer, per-stream backlog, keepalive probing."""

from __future__ import annotations

import collections
import dataclasses
import socket
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING

from google.protobuf.descriptor import FieldDescriptor
from google.protobuf.message import Message

from taz.c3.errors import TazConnectionLost, TazProtocolError
from taz.c3.protocol.frame import DEFAULT_MAX_PAYLOAD, Frame, wait_readable
from taz.c3.settings import Backlog, Keepalive
from taz.v1 import advanced_pb2, common_pb2

if TYPE_CHECKING:
    from taz.c3.connection import Connection

_DEFAULT_BACKLOG: Backlog = Backlog()


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
        _wait_readable: (Callable[[socket.socket, float | None], bool] | None) = None,
    ) -> None:
        self._conn = conn
        self._backlog = backlog if backlog is not None else _DEFAULT_BACKLOG
        # Per-stream pending frames.
        self._buffer: dict[int, collections.deque[Frame]] = {}
        # Stream IDs whose frames are silently discarded (§10.2).
        self._closed: set[int] = set()
        self._total_frames: int = 0
        self._total_bytes: int = 0
        self._wait: Callable[[socket.socket, float | None], bool] = (
            _wait_readable if _wait_readable is not None else wait_readable
        )

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

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

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

    def _cancel_advertised(self) -> bool:
        try:
            ops = self._conn.capabilities.operations
        except RuntimeError:
            return False
        return common_pb2.OPCODE_CANCEL in ops

    def _overflow(self) -> None:
        """Apply the cancel overflow policy to the most-buffered stream."""
        if not self._buffer:
            return
        victim_id = max(self._buffer, key=lambda s: len(self._buffer[s]))

        if self._cancel_advertised():
            cancel_payload = advanced_pb2.CancelRequest(
                target_stream_id=victim_id
            ).SerializeToString()
            cancel_stream = self._conn.send_request(
                common_pb2.OPCODE_CANCEL, cancel_payload
            )
            # Discard the CANCEL response; we don't need to wait for it.
            self._closed.add(cancel_stream)

        # Drop victim's buffered frames and mark the stream closed.
        removed = self._buffer.pop(victim_id, collections.deque())
        for frame in removed:
            self._total_frames -= 1
            self._total_bytes -= len(frame.payload)
        self._closed.add(victim_id)

    def _add_to_backlog(self, frame: Frame) -> None:
        """Buffer frame, triggering overflow policy if the backlog is full."""
        if (
            self._total_frames >= self._backlog.max_frames
            or self._total_bytes + len(frame.payload) > self._backlog.max_bytes
        ):
            self._overflow()

        # The overflow may have closed this frame's stream.
        if frame.stream_id in self._closed:
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
        sock = self._conn._sock
        if sock is None:
            raise RuntimeError("not connected")

        raw_wait: float | None = keepalive.timeout
        if raw_wait is not None and raw_wait == float("inf"):
            raw_wait = None

        while True:
            if not self._wait(sock, raw_wait):
                self._conn.close()
                raise TazConnectionLost(
                    f"no PONG within {keepalive.timeout:g} s; connection presumed dead"
                )

            frame = self._conn.recv_frame()

            if frame.type == common_pb2.FRAME_TYPE_PONG:
                return

            if frame.type not in DEFAULT_MAX_PAYLOAD:
                continue

            if frame.stream_id in self._closed:
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
        """
        frame = self._next_frame(expected, keepalive, expected_opcode)
        if response_type is None:
            return frame
        chunks: list[bytes] = []
        while (
            frame.type == common_pb2.FRAME_TYPE_RESPONSE
            and frame.flags & common_pb2.FRAME_FLAG_CONTINUATION
        ):
            chunks.append(frame.payload)
            frame = self._next_frame(expected, keepalive, expected_opcode)
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
    ) -> Frame:
        """Return the next frame for ``expected``, buffered or read."""
        buffered = self._pop_buffered(expected)
        if buffered is not None:
            return buffered

        sock = self._conn._sock
        if sock is None:
            raise RuntimeError("not connected")

        probing = False
        while True:
            raw_wait: float | None = keepalive.timeout if probing else keepalive.idle
            # Normalize infinity (Keepalive.OFF or explicit inf) to None.
            if raw_wait is not None and raw_wait == float("inf"):
                raw_wait = None

            readable = self._wait(sock, raw_wait)
            if not readable:
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

            # A PRIORITY error is routed by stream_id like any other frame:
            # nothing runs in the background yet, so there is nothing to
            # interrupt.

            # Closed stream: discard (§10.2).
            if frame.stream_id in self._closed:
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
