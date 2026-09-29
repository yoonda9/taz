"""Protocol framing layer — header pack/unpack, validation, send/recv."""

from __future__ import annotations

import enum
import socket
import struct
import sys
import types
from collections.abc import Mapping
from dataclasses import dataclass, field, replace

from taz.c3.errors import TazConnectionLost, TazProtocolError
from taz.v1 import common_pb2

_STRUCT = struct.Struct("<BBHII")
HEADER_SIZE = _STRUCT.size

# Protocol §6 default payload limits keyed by frame type integer value. Its keys
# are exactly the types assigned in §4.2. Read-only because recv_frame uses it
# as its default table.
DEFAULT_MAX_PAYLOAD: Mapping[int, int] = types.MappingProxyType(
    {
        common_pb2.FRAME_TYPE_PING: 0,
        common_pb2.FRAME_TYPE_PONG: 0,
        common_pb2.FRAME_TYPE_CAPABILITY: 1024,
        common_pb2.FRAME_TYPE_ERROR: 4096,
        common_pb2.FRAME_TYPE_REQUEST: 65536,
        common_pb2.FRAME_TYPE_RESPONSE: 65536,
        common_pb2.FRAME_TYPE_FILE_CHUNK: 65536,
    }
)

# Upper bound applied to unknown frame types when using the default limits (§10.1).
DEFAULT_UNKNOWN_TYPE_MAX_PAYLOAD: int = max(DEFAULT_MAX_PAYLOAD.values())

_HAS_SENDMSG = hasattr(socket.socket, "sendmsg")
_RECV_CAP = 256 * 1024  # max bytes requested per recv() call


class Verdict(enum.Enum):
    OK = enum.auto()
    UNKNOWN_TYPE = enum.auto()
    OVERSIZED = enum.auto()


@dataclass(slots=True)
class Frame:
    type: int
    flags: int
    opcode: int
    length: int
    stream_id: int
    # Kept out of repr so logging a frame does not dump up to 64 KiB of payload.
    payload: bytes = field(default=b"", repr=False)


def pack_header(frame: Frame) -> bytes:
    if not (0 <= frame.type <= 0xFF):
        raise ValueError(f"type out of range: {frame.type!r}")
    if not (0 <= frame.flags <= 0xFF):
        raise ValueError(f"flags out of range: {frame.flags!r}")
    if not (0 <= frame.opcode <= 0xFFFF):
        raise ValueError(f"opcode out of range: {frame.opcode!r}")
    if not (0 <= frame.length <= 0xFFFFFFFF):
        raise ValueError(f"length out of range: {frame.length!r}")
    if not (0 <= frame.stream_id <= 0xFFFFFFFF):
        raise ValueError(f"stream_id out of range: {frame.stream_id!r}")
    return _STRUCT.pack(
        frame.type, frame.flags, frame.opcode, frame.length, frame.stream_id
    )


def unpack_header(data: bytes) -> Frame:
    # Frame's header fields are declared in wire order.
    return Frame(*_STRUCT.unpack_from(data))


def validate_header(frame: Frame, limits: Mapping[int, int]) -> Verdict:
    """Check a header against per-type payload limits.

    A type is known if protocol §4.2 assigns it (the keys of
    DEFAULT_MAX_PAYLOAD), as in the C daemon. ``limits`` overrides the default
    for a known type; entries for other types are ignored.
    """
    if frame.type in DEFAULT_MAX_PAYLOAD:
        if frame.length > limits.get(frame.type, DEFAULT_MAX_PAYLOAD[frame.type]):
            return Verdict.OVERSIZED
        return Verdict.OK
    # Unknown type — bound by the largest known-type limit (§10.1).
    bound = max(limits.get(t, d) for t, d in DEFAULT_MAX_PAYLOAD.items())
    if frame.length > bound:
        return Verdict.OVERSIZED
    return Verdict.UNKNOWN_TYPE


def _connection_lost(exc: OSError) -> TazConnectionLost:
    if isinstance(exc, TimeoutError):
        message = "connection timed out"
    else:
        message = f"connection lost: {exc}"
    return TazConnectionLost(message)


def _header_detail(frame: Frame) -> str:
    return (
        f"type=0x{frame.type:02x} flags=0x{frame.flags:02x} "
        f"opcode=0x{frame.opcode:04x} stream_id={frame.stream_id} "
        f"length={frame.length}"
    )


def _recv_failed(
    sock: socket.socket,
    exc: OSError,
    got: int,
    n: int,
    what: str,
    header: Frame | None,
) -> TazConnectionLost:
    # Before the first header byte nothing of a frame has arrived, so a
    # timeout there is not a stall inside a frame.
    if isinstance(exc, TimeoutError) and (header is not None or got > 0):
        timeout = sock.gettimeout()
        waited = f": no data for {timeout:g} s" if timeout else ""
        return TazConnectionLost(
            f"connection stalled mid-frame{waited}"
            f" (received {got} of {n} {what} bytes)",
            _header_detail(header) if header else "",
        )
    return _connection_lost(exc)


def _closed_by_peer(
    got: int, n: int, what: str, header: Frame | None
) -> TazConnectionLost:
    if header is None and got == 0:
        return TazConnectionLost("connection closed by peer")
    return TazConnectionLost(
        f"connection closed by peer mid-frame (received {got} of {n} {what} bytes)",
        _header_detail(header) if header else "",
    )


def _recv_exact(
    sock: socket.socket, n: int, what: str, header: Frame | None = None
) -> bytes:
    """Read exactly ``n`` bytes of a frame's ``what`` ("header" or "payload").

    ``header`` is the frame's unpacked header once known (the payload read);
    failures then carry its fields in ``detail``. Messages are formatted only
    on the error path.
    """
    if n == 0:
        return b""
    try:
        first = sock.recv(min(n, _RECV_CAP))
    except OSError as exc:
        raise _recv_failed(sock, exc, 0, n, what, header) from exc
    if len(first) == n:
        return first
    parts, got = [first], len(first)
    while got < n:
        if not parts[-1]:
            raise _closed_by_peer(got, n, what, header)
        try:
            piece = sock.recv(min(n - got, _RECV_CAP))
        except OSError as exc:
            raise _recv_failed(sock, exc, got, n, what, header) from exc
        parts.append(piece)
        got += len(piece)
    return b"".join(parts)


def wait_readable(sock: socket.socket, timeout: float | None) -> bool:
    """Peek at the socket to check if data is available, without consuming it.

    Returns True if data is ready, False if timeout expires. Raises
    TazConnectionLost on EOF or error. Restores the socket's original timeout.
    """
    old = sock.gettimeout()
    sock.settimeout(timeout)
    try:
        data = sock.recv(1, socket.MSG_PEEK)
    except TimeoutError:
        return False
    except OSError as exc:
        raise _connection_lost(exc) from exc
    finally:
        sock.settimeout(old)
    if not data:
        raise TazConnectionLost("connection closed by peer")
    return True


def recv_frame(
    sock: socket.socket,
    limits: Mapping[int, int] = DEFAULT_MAX_PAYLOAD,
) -> Frame:
    header_bytes = _recv_exact(sock, HEADER_SIZE, "header")
    frame = unpack_header(header_bytes)
    verdict = validate_header(frame, limits)
    if verdict is Verdict.OVERSIZED:
        if frame.type in DEFAULT_MAX_PAYLOAD:
            limit = limits.get(frame.type, DEFAULT_MAX_PAYLOAD[frame.type])
            frame_name = common_pb2.FrameType.Name(frame.type).removeprefix(
                "FRAME_TYPE_"
            )
        else:
            limit = max(limits.get(t, d) for t, d in DEFAULT_MAX_PAYLOAD.items())
            frame_name = f"type 0x{frame.type:02x}"
        raise TazProtocolError(
            f"{frame_name} payload {frame.length} bytes exceeds limit {limit}",
            f"{_header_detail(frame)} limit={limit}",
        )
    frame.payload = _recv_exact(sock, frame.length, "payload", frame)
    return frame


def _sendmsg_all(sock: socket.socket, views: list[memoryview]) -> None:
    """Send all bytes from multiple buffers via sendmsg, retrying partial sends."""
    if sys.platform == "win32":
        raise NotImplementedError("sendmsg is not available on Windows")
    # An empty buffer (a PING, or any frame without payload) would stay at the
    # head of the list: sendmsg returns 0 for it and the loop never ends.
    views = [v for v in views if len(v)]
    while views:
        sent = sock.sendmsg(views)
        remaining = sent
        while views and remaining > 0:
            if remaining >= len(views[0]):
                remaining -= len(views[0])
                views.pop(0)
            else:
                views[0] = views[0][remaining:]
                remaining = 0


def send_frame(sock: socket.socket, frame: Frame) -> None:
    out = replace(frame, length=len(frame.payload))
    header = pack_header(out)
    try:
        if _HAS_SENDMSG:
            _sendmsg_all(sock, [memoryview(header), memoryview(frame.payload)])
        else:
            sock.sendall(header + frame.payload)
    except OSError as exc:  # timeout (possibly mid-frame), reset, broken pipe
        raise _connection_lost(exc) from exc
