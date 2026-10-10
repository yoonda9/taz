"""StreamIterator: client-side iteration over a streamed RESPONSE sequence."""

from __future__ import annotations

import warnings
from collections.abc import Callable
from types import TracebackType
from typing import TYPE_CHECKING, Literal

from taz.c3.errors import TazConnectionError, TazError, TazProtocolError
from taz.c3.protocol.dispatch import MaxWaitExpired
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2

if TYPE_CHECKING:
    # Avoid a client.py <-> stream.py import cycle: TazClient's namespaces
    # construct a StreamIterator, so this module can only see TazClient's
    # type, not its runtime module.
    from taz.c3.client import TazClient


def _raise_from_error_frame(payload: bytes) -> None:
    info = common_pb2.ErrorInfo()
    info.ParseFromString(payload)
    raise TazError(info.code, info.message, info.detail)


class StreamIterator[T]:
    """Iterate the streamed RESPONSE frames of one opened stream.

    Constructed by a namespace method (e.g. ``ProcessNamespace.monitor``),
    never directly. ``__next__`` returns one parsed update per call and
    raises ``StopIteration`` after the final update (the frame without
    CONTINUATION) has been returned.
    """

    def __init__(
        self,
        client: TazClient,
        stream_id: int,
        opcode: int,
        parse: Callable[[bytes], T],
        *,
        keepalive: Keepalive,
        max_wait: float | None,
        overflow: Literal["cancel", "drop_oldest"],
    ) -> None:
        self._client = client
        self.stream_id = stream_id
        self._opcode = opcode
        self._parse = parse
        self._keepalive = keepalive
        self._max_wait = max_wait
        self._ended = False
        self._closed = False
        self._error: TazError | None = None
        self.cancelled: bool | None = None
        client._dispatcher.register_stream(stream_id, overflow)

    @property
    def dropped(self) -> int:
        """Frames the backlog's drop_oldest policy has dropped for this stream."""
        return self._client._dispatcher.stream_dropped(self.stream_id)

    @property
    def ended(self) -> bool:
        """True once no further update will ever arrive on this stream."""
        return self._ended

    def __iter__(self) -> StreamIterator[T]:
        return self

    def __next__(self) -> T:
        dispatcher = self._client._dispatcher
        if self._error is None:
            self._error = dispatcher.stream_error(self.stream_id)
            if self._error is not None:
                # The backlog already cancelled this stream. Keep its error
                # here, so every later call raises it, and let the
                # dispatcher forget the stream.
                self._ended = True
                dispatcher.forget_stream(self.stream_id)
        if self._error is not None:
            raise self._error
        if self._ended:
            raise StopIteration

        try:
            frame = dispatcher.recv_response(
                self.stream_id,
                self._keepalive,
                expected_opcode=self._opcode,
                max_wait=self._max_wait,
            )
        except MaxWaitExpired:
            # Cancel without waiting for the reply: when the daemon is what
            # stalled, waiting for it would make the wait unbounded again.
            self._closed = True
            self._ended = True
            dispatcher.cancel_without_reply(self.stream_id)
            dispatcher.forget_stream(self.stream_id)
            raise TazError(
                common_pb2.ERROR_CODE_TIMEOUT,
                f"no frame on stream {self.stream_id} within {self._max_wait:g} s",
            ) from None
        except TazConnectionError:
            # The connection is gone, and the stream with it.
            self._ended = True
            dispatcher.forget_stream(self.stream_id)
            raise

        if frame.type == common_pb2.FRAME_TYPE_ERROR:
            self._ended = True
            dispatcher.forget_stream(self.stream_id)
            _raise_from_error_frame(frame.payload)

        if frame.type != common_pb2.FRAME_TYPE_RESPONSE:
            self._ended = True
            dispatcher.forget_stream(self.stream_id)
            raise TazProtocolError(
                f"unexpected frame type 0x{frame.type:02x} on stream"
                f" {self.stream_id} (opcode 0x{self._opcode:04x})"
            )

        item = self._parse(frame.payload)
        if not frame.flags & common_pb2.FRAME_FLAG_CONTINUATION:
            self._ended = True
            dispatcher.forget_stream(self.stream_id)
        return item

    def close(self) -> None:
        """Stop reading this stream, cancelling it with the daemon if possible.

        A no-op if the stream has already ended or been closed. Idempotent:
        calling this twice sends at most one CANCEL.
        """
        if self._ended or self._closed:
            return
        self._closed = True
        self._ended = True
        dispatcher = self._client._dispatcher
        # Close first so a final frame raced by the CANCEL response is
        # discarded rather than buffered.
        dispatcher.close_stream(self.stream_id)
        # A lost connection needs no CANCEL, and trying one would raise a
        # second error over the one already leaving a with block.
        if not self._client._conn.closed and dispatcher._cancel_advertised():
            self.cancelled = self._client.cancel(self.stream_id, self._keepalive)
        else:
            self.cancelled = None
        dispatcher.forget_stream(self.stream_id)

    def __enter__(self) -> StreamIterator[T]:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_val: BaseException | None,
        exc_tb: TracebackType | None,
    ) -> None:
        self.close()

    def __del__(self) -> None:
        # Never touches the socket: just warns if the stream was abandoned
        # without close() and without running to its final frame.
        if self._ended or self._closed:
            return
        warnings.warn(
            f"unclosed StreamIterator for stream {self.stream_id}"
            f" (opcode 0x{self._opcode:04x})",
            ResourceWarning,
            stacklevel=2,
            source=self,
        )
