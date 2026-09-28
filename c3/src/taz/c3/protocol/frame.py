"""Protocol framing layer — header pack/unpack, validation, send/recv."""

from __future__ import annotations

import enum
import socket as _socket
import struct
from collections.abc import Mapping
from dataclasses import dataclass, field

from taz.c3.errors import TazError, TazProtocolError
from taz.v1 import common_pb2

HEADER_SIZE = 12
_STRUCT = struct.Struct("<BBHII")

# Protocol §6 default payload limits keyed by frame type integer value.
DEFAULT_MAX_PAYLOAD: Mapping[int, int] = {
    common_pb2.FRAME_TYPE_PING: 0,
    common_pb2.FRAME_TYPE_PONG: 0,
    common_pb2.FRAME_TYPE_CAPABILITY: 1024,
    common_pb2.FRAME_TYPE_ERROR: 4096,
    common_pb2.FRAME_TYPE_REQUEST: 65536,
    common_pb2.FRAME_TYPE_RESPONSE: 65536,
    common_pb2.FRAME_TYPE_FILE_CHUNK: 65536,
}


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
    payload: bytes = field(default=b"")


def pack_header(frame: Frame) -> bytes:
    return _STRUCT.pack(
        frame.type, frame.flags, frame.opcode, frame.length, frame.stream_id
    )


def unpack_header(data: bytes) -> Frame:
    type_, flags, opcode, length, stream_id = _STRUCT.unpack(data[:HEADER_SIZE])
    return Frame(
        type=type_,
        flags=flags,
        opcode=opcode,
        length=length,
        stream_id=stream_id,
    )


def validate_header(frame: Frame, limits: Mapping[int, int]) -> Verdict:
    if frame.type in limits:
        if frame.length > limits[frame.type]:
            return Verdict.OVERSIZED
        return Verdict.OK
    # Unknown type — bound by the largest limit in the table (§10.1).
    if frame.length > max(limits.values()):
        return Verdict.OVERSIZED
    return Verdict.UNKNOWN_TYPE


def _recv_exact(sock: _socket.socket, n: int) -> bytes:
    if n == 0:
        return b""
    buf = bytearray(n)
    view = memoryview(buf)
    received = 0
    while received < n:
        try:
            chunk = sock.recv_into(view[received:], n - received)
        except TimeoutError as exc:
            raise TazError(
                common_pb2.ERROR_CODE_CONNECTION_LOST,
                "connection timed out",
            ) from exc
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
    sock.sendall(data)
