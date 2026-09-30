"""TCP connection with CAPABILITY handshake."""

from __future__ import annotations

import contextlib
import socket
from collections.abc import Iterator, Mapping

from taz.c3.errors import TazConnectionLost, TazError, TazProtocolError
from taz.c3.protocol.frame import (
    DEFAULT_MAX_PAYLOAD,
    Frame,
)
from taz.c3.protocol.frame import recv_frame as _recv_frame
from taz.c3.protocol.frame import send_frame as _send_frame
from taz.v1 import common_pb2, daemon_control_pb2

_PROTOCOL_MAJOR: int = 1


class Connection:
    """TCP connection lifecycle and capability gate.

    Manages the socket, performs the CAPABILITY handshake on connect, and
    exposes the negotiated payload-limit table for use by the dispatch layer.
    """

    def __init__(self, timeout: float | None = None) -> None:
        self._timeout = timeout
        self._sock: socket.socket | None = None
        self._capabilities: daemon_control_pb2.CapabilityPayload | None = None
        self._limits: dict[int, int] = dict(DEFAULT_MAX_PAYLOAD)
        self._failure: BaseException | None = None
        self._next_stream_id: int = 1

    def connect(self, host: str, port: int) -> None:
        """Open a TCP connection and complete the CAPABILITY handshake."""
        sock = socket.create_connection((host, port), timeout=self._timeout)
        self._sock = sock
        try:
            self._handshake()
        except BaseException:
            self.close()
            raise

    def _handshake(self) -> None:
        sock = self._sock
        if sock is None:
            raise RuntimeError("not connected")
        frame = _recv_frame(sock, DEFAULT_MAX_PAYLOAD)
        if frame.type != common_pb2.FRAME_TYPE_CAPABILITY:
            raise TazProtocolError(
                f"expected CAPABILITY frame, got type 0x{frame.type:02x}"
            )
        cap = daemon_control_pb2.CapabilityPayload()
        cap.ParseFromString(frame.payload)
        if cap.protocol_major != _PROTOCOL_MAJOR:
            raise TazProtocolError(
                f"server protocol_major {cap.protocol_major} != {_PROTOCOL_MAJOR}"
            )
        limits: dict[int, int] = dict(DEFAULT_MAX_PAYLOAD)
        for pair in cap.max_payload_sizes:
            if pair.key in DEFAULT_MAX_PAYLOAD:
                limits[pair.key] = pair.value
        self._capabilities = cap
        self._limits = limits

    @contextlib.contextmanager
    def _io(self) -> Iterator[None]:
        """Close and record the failure on any BaseException from socket I/O."""
        try:
            yield
        except BaseException as exc:
            self.close()
            self._failure = exc
            raise

    def _check_failure(self) -> None:
        """Fail fast if a prior I/O error has poisoned this connection."""
        cause = self._failure
        if cause is None:
            return
        msg = f"connection closed after an earlier failure: {cause}"
        if isinstance(cause, TazConnectionLost):
            raise TazConnectionLost(msg) from cause
        if isinstance(cause, TazProtocolError):
            raise TazProtocolError(msg) from cause
        if isinstance(cause, TazError):
            raise TazError(cause.code, msg) from cause
        raise TazConnectionLost(msg) from cause

    def close(self) -> None:
        """Close the underlying socket."""
        if self._sock is not None:
            with contextlib.suppress(OSError):
                self._sock.close()
            self._sock = None

    @property
    def closed(self) -> bool:
        return self._sock is None

    @property
    def capabilities(self) -> daemon_control_pb2.CapabilityPayload:
        if self._capabilities is None:
            raise RuntimeError("not connected")
        return self._capabilities

    @property
    def limits(self) -> Mapping[int, int]:
        """Negotiated payload-limit table for use by recv_frame."""
        return self._limits

    def recv_frame(self) -> Frame:
        """Receive one frame using the negotiated payload limits."""
        self._check_failure()
        sock = self._sock
        if sock is None:
            raise RuntimeError("not connected")
        with self._io():
            return _recv_frame(sock, self._limits)

    def _send_ping(self) -> None:
        """Send a PING frame (keepalive probe)."""
        sock = self._sock
        if sock is None:
            raise RuntimeError("not connected")
        frame = Frame(
            type=common_pb2.FRAME_TYPE_PING,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
        )
        with self._io():
            _send_frame(sock, frame)

    def send_request(self, opcode: int, payload: bytes) -> int:
        """Send a REQUEST frame and return its assigned stream_id.

        Local pre-checks (opcode advertisement, payload size) run before any
        socket interaction and never close the connection on failure.
        """
        if self._capabilities is not None and opcode not in set(
            self._capabilities.operations
        ):
            raise TazError(
                common_pb2.ERROR_CODE_NOT_SUPPORTED,
                f"opcode 0x{opcode:04x} is not advertised by the daemon",
            )
        request_limit = self._limits.get(
            common_pb2.FRAME_TYPE_REQUEST,
            DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST],
        )
        if len(payload) > request_limit:
            raise TazError(
                common_pb2.ERROR_CODE_INVALID_REQUEST,
                f"request payload {len(payload)} bytes exceeds daemon REQUEST"
                f" limit {request_limit}; not sent",
            )
        self._check_failure()
        sock = self._sock
        if sock is None:
            raise RuntimeError("not connected")
        stream_id = self._next_stream_id
        self._next_stream_id = self._next_stream_id % 0xFFFFFFFF + 1
        frame = Frame(
            type=common_pb2.FRAME_TYPE_REQUEST,
            flags=0,
            opcode=opcode,
            length=len(payload),
            stream_id=stream_id,
            payload=payload,
        )
        with self._io():
            _send_frame(sock, frame)
        return stream_id
