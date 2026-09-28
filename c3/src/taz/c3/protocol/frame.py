"""Protocol framing layer — header pack/unpack, validation, send/recv."""

from __future__ import annotations

import enum
import socket as _socket
import struct
import types
from collections.abc import Mapping
from dataclasses import dataclass, field

from taz.c3.errors import TazError, TazProtocolError
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


class Verdict(enum.Enum):
    OK = "ok"
    UNKNOWN_TYPE = "unknown_type"
    OVERSIZED = "oversized"


@dataclass
class Frame:
    type: int
    flags: int
    opcode: int
    length: int
    stream_id: int
    # Kept out of repr so logging a frame does not dump up to 64 KiB of payload.
    payload: bytes = field(default=b"", repr=False)


def pack_header(frame: Frame) -> bytes:
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


def _connection_lost(exc: OSError) -> TazError:
    if isinstance(exc, TimeoutError):
        message = "connection timed out"
    else:
        message = f"connection lost: {exc}"
    return TazError(common_pb2.ERROR_CODE_CONNECTION_LOST, message)


def _recv_exact(sock: _socket.socket, n: int) -> bytes:
    buf = bytearray(n)
    view = memoryview(buf)
    received = 0
    while received < n:
        try:
            chunk = sock.recv_into(view[received:], n - received)
        except OSError as exc:  # timeout, reset, abort, closed socket
            raise _connection_lost(exc) from exc
        if chunk == 0:
            raise TazError(
                common_pb2.ERROR_CODE_CONNECTION_LOST,
                "connection closed by peer",
            )
        received += chunk
    return bytes(buf)


def recv_frame(
    sock: _socket.socket,
    limits: Mapping[int, int] = DEFAULT_MAX_PAYLOAD,
) -> Frame:
    header_bytes = _recv_exact(sock, HEADER_SIZE)
    frame = unpack_header(header_bytes)
    verdict = validate_header(frame, limits)
    if verdict is Verdict.OVERSIZED:
        raise TazProtocolError(
            f"oversized frame: type=0x{frame.type:02x} length={frame.length}",
        )
    frame.payload = _recv_exact(sock, frame.length)
    return frame


def send_frame(sock: _socket.socket, frame: Frame) -> None:
    frame.length = len(frame.payload)
    data = pack_header(frame) + frame.payload
    try:
        sock.sendall(data)
    except OSError as exc:  # timeout (possibly mid-frame), reset, broken pipe
        raise _connection_lost(exc) from exc
