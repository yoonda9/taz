"""Tests for FileNamespace.put()."""

from __future__ import annotations

import socket
from pathlib import Path
from unittest.mock import MagicMock, patch

import google_crc32c
import pytest
from taz.c3.client import TazClient
from taz.c3.errors import TazChecksumError, TazError, TazProtocolError
from taz.c3.file import FileTransfer
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header, unpack_header
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, file_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_cancel.py)
# ---------------------------------------------------------------------------

_PUT_OPS = [
    common_pb2.OPCODE_PING,
    common_pb2.OPCODE_CANCEL,
    common_pb2.OPCODE_FILE_PUT,
]


def _capability_bytes(
    operations: list[int] | None = None,
    max_payload_sizes: dict[int, int] | None = None,
) -> bytes:
    from taz.v1 import daemon_control_pb2

    ops = operations if operations is not None else _PUT_OPS
    cap = daemon_control_pb2.CapabilityPayload(
        protocol_major=1,
        protocol_minor=0,
        operations=ops,
    )
    if max_payload_sizes:
        for key, value in max_payload_sizes.items():
            cap.max_payload_sizes.add(key=key, value=value)
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


def _response_bytes(
    opcode: int,
    payload: bytes = b"",
    stream_id: int = 1,
    flags: int = 0,
) -> bytes:
    header = pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_RESPONSE,
            flags=flags,
            opcode=opcode,
            length=len(payload),
            stream_id=stream_id,
        )
    )
    return header + payload


def _error_bytes(
    opcode: int,
    code: common_pb2.ErrorCode,
    message: str,
    stream_id: int = 1,
    detail: str = "",
) -> bytes:
    info = common_pb2.ErrorInfo(code=code, message=message, detail=detail)
    payload = info.SerializeToString()
    header = pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_ERROR,
            flags=0,
            opcode=opcode,
            length=len(payload),
            stream_id=stream_id,
        )
    )
    return header + payload


def _ack_bytes(ready: bool = True, stream_id: int = 1) -> bytes:
    resp = file_pb2.FilePutResponse(ack=file_pb2.FilePutResponse.Ack(ready=ready))
    return _response_bytes(
        common_pb2.OPCODE_FILE_PUT, resp.SerializeToString(), stream_id=stream_id
    )


def _confirm_bytes(bytes_written: int, checksum: bytes, stream_id: int = 1) -> bytes:
    resp = file_pb2.FilePutResponse(
        confirm=file_pb2.FilePutResponse.Confirmation(
            bytes_written=bytes_written, checksum=checksum
        )
    )
    return _response_bytes(
        common_pb2.OPCODE_FILE_PUT, resp.SerializeToString(), stream_id=stream_id
    )


def _pong_bytes() -> bytes:
    return pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_PONG,
            flags=0,
            opcode=0,
            length=0,
            stream_id=0,
        )
    )


def _cancel_response_bytes(cancelled: bool, stream_id: int) -> bytes:
    from taz.v1 import advanced_pb2

    resp = advanced_pb2.CancelResponse(cancelled=cancelled)
    return _response_bytes(
        common_pb2.OPCODE_CANCEL, resp.SerializeToString(), stream_id=stream_id
    )


_SOCKET_SPEC = sorted({*dir(socket.socket), "sendmsg"})


def _mock_sock(*chunks: bytes) -> MagicMock:
    """Mock socket that serves ``chunks`` sequentially then EOF.

    Records every ``sendmsg``/``sendall`` call's bytes on ``sock.sent``, one
    list entry per frame (``send_frame`` makes exactly one such call/frame).
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
    max_payload_sizes: dict[int, int] | None = None,
) -> tuple[TazClient, MagicMock]:
    mock_sock = _mock_sock(
        _capability_bytes(operations=operations, max_payload_sizes=max_payload_sizes),
        *extra_chunks,
    )
    client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
    with patch(_PATCH_CC, return_value=mock_sock):
        client.connect()
    return client, mock_sock


def _sent_frames(mock_sock: MagicMock) -> list[Frame]:
    """Decode every frame recorded on ``mock_sock.sent`` (one entry each)."""
    frames = []
    for blob in mock_sock.sent:
        frame = unpack_header(blob[:HEADER_SIZE])
        frame.payload = blob[HEADER_SIZE : HEADER_SIZE + frame.length]
        frames.append(frame)
    return frames


# ---------------------------------------------------------------------------
# put()
# ---------------------------------------------------------------------------


class TestPut:
    def test_sends_file_put_request_with_metadata(self, tmp_path: Path) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"hello")
        checksum = google_crc32c.value(b"hello").to_bytes(4, "little")
        client, mock_sock = _connected_client_with_sock(
            _ack_bytes(), _confirm_bytes(5, checksum)
        )
        client.file.put(str(local), "/remote/dest", overwrite=True, permissions=0o600)
        frames = _sent_frames(mock_sock)
        request_frame = frames[0]
        assert request_frame.type == common_pb2.FRAME_TYPE_REQUEST
        assert request_frame.opcode == common_pb2.OPCODE_FILE_PUT
        req = file_pb2.FilePutRequest()
        req.ParseFromString(request_frame.payload)
        assert req.dest == "/remote/dest"
        assert req.size == 5
        assert req.permissions == 0o600
        assert req.overwrite is True

    def test_default_permissions_0644(self, tmp_path: Path) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"x")
        checksum = google_crc32c.value(b"x").to_bytes(4, "little")
        client, mock_sock = _connected_client_with_sock(
            _ack_bytes(), _confirm_bytes(1, checksum)
        )
        client.file.put(str(local), "/remote/dest")
        req = file_pb2.FilePutRequest()
        req.ParseFromString(_sent_frames(mock_sock)[0].payload)
        assert req.permissions == 0o644
        assert req.overwrite is False

    def test_small_file_single_chunk_and_returns_transfer(self, tmp_path: Path) -> None:
        local = tmp_path / "src.bin"
        data = b"hello world"
        local.write_bytes(data)
        checksum = google_crc32c.value(data).to_bytes(4, "little")
        client, mock_sock = _connected_client_with_sock(
            _ack_bytes(), _confirm_bytes(len(data), checksum)
        )
        result = client.file.put(str(local), "/remote/dest")
        assert result == FileTransfer(size=len(data), checksum=checksum)

        chunk_frames = _sent_frames(mock_sock)[1:]
        assert len(chunk_frames) == 1
        chunk = chunk_frames[0]
        assert chunk.type == common_pb2.FRAME_TYPE_FILE_CHUNK
        assert chunk.opcode == 0
        assert chunk.flags == 0  # last chunk: CONTINUATION clear
        assert chunk.payload == data

    def test_zero_byte_file_sends_one_empty_last_chunk(self, tmp_path: Path) -> None:
        local = tmp_path / "empty.bin"
        local.write_bytes(b"")
        checksum = google_crc32c.value(b"").to_bytes(4, "little")
        assert checksum == b"\x00\x00\x00\x00"
        client, mock_sock = _connected_client_with_sock(
            _ack_bytes(), _confirm_bytes(0, checksum)
        )
        result = client.file.put(str(local), "/remote/dest")
        assert result.size == 0
        assert result.checksum == b"\x00\x00\x00\x00"

        chunk_frames = _sent_frames(mock_sock)[1:]
        assert len(chunk_frames) == 1
        assert chunk_frames[0].payload == b""
        assert chunk_frames[0].flags == 0

    def test_large_file_chunked_to_advertised_limit(self, tmp_path: Path) -> None:
        local = tmp_path / "src.bin"
        data = b"ABCDEFGHIJ"  # 10 bytes
        local.write_bytes(data)
        checksum = google_crc32c.value(data).to_bytes(4, "little")
        client, mock_sock = _connected_client_with_sock(
            _ack_bytes(),
            _confirm_bytes(len(data), checksum),
            max_payload_sizes={common_pb2.FRAME_TYPE_FILE_CHUNK: 4},
        )
        client.file.put(str(local), "/remote/dest")

        chunk_frames = _sent_frames(mock_sock)[1:]
        assert [f.payload for f in chunk_frames] == [b"ABCD", b"EFGH", b"IJ"]
        assert [f.flags for f in chunk_frames] == [
            common_pb2.FRAME_FLAG_CONTINUATION,
            common_pb2.FRAME_FLAG_CONTINUATION,
            0,
        ]

    def test_chunk_limit_zero_raises_not_supported_before_sending(
        self, tmp_path: Path
    ) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"data")
        client, mock_sock = _connected_client_with_sock(
            _pong_bytes(),
            max_payload_sizes={common_pb2.FRAME_TYPE_FILE_CHUNK: 0},
        )
        with pytest.raises(TazError) as exc_info:
            client.file.put(str(local), "/remote/dest")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        mock_sock.sendmsg.assert_not_called()
        mock_sock.sendall.assert_not_called()

        # The connection is still usable after the pre-send rejection.
        client.ping()

    def test_ack_error_response_raises_and_sends_no_chunks(
        self, tmp_path: Path
    ) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"data")
        client, mock_sock = _connected_client_with_sock(
            _error_bytes(
                common_pb2.OPCODE_FILE_PUT,
                common_pb2.ERROR_CODE_ALREADY_EXISTS,
                "destination exists",
            )
        )
        with pytest.raises(TazError) as exc_info:
            client.file.put(str(local), "/remote/dest")
        assert exc_info.value.code == common_pb2.ERROR_CODE_ALREADY_EXISTS
        assert len(mock_sock.sent) == 1  # only the FILE_PUT REQUEST

    def test_ack_not_ready_raises_protocol_error(self, tmp_path: Path) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"data")
        client, mock_sock = _connected_client_with_sock(_ack_bytes(ready=False))
        with pytest.raises(TazProtocolError):
            client.file.put(str(local), "/remote/dest")
        assert len(mock_sock.sent) == 1

    def test_confirm_error_response_raises(self, tmp_path: Path) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"data")
        client, _ = _connected_client_with_sock(
            _ack_bytes(),
            _error_bytes(
                common_pb2.OPCODE_FILE_PUT,
                common_pb2.ERROR_CODE_INTERNAL,
                "disk full",
            ),
        )
        with pytest.raises(TazError) as exc_info:
            client.file.put(str(local), "/remote/dest")
        assert exc_info.value.code == common_pb2.ERROR_CODE_INTERNAL

    def test_confirm_bytes_written_mismatch_raises_protocol_error(
        self, tmp_path: Path
    ) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"data")
        checksum = google_crc32c.value(b"data").to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _ack_bytes(), _confirm_bytes(3, checksum)
        )
        with pytest.raises(TazProtocolError):
            client.file.put(str(local), "/remote/dest")

    def test_confirm_checksum_mismatch_raises_checksum_error(
        self, tmp_path: Path
    ) -> None:
        local = tmp_path / "src.bin"
        data = b"data"
        local.write_bytes(data)
        local_checksum = google_crc32c.value(data).to_bytes(4, "little")
        wrong_checksum = b"\xff\xff\xff\xff"
        client, _ = _connected_client_with_sock(
            _ack_bytes(), _confirm_bytes(len(data), wrong_checksum)
        )
        with pytest.raises(TazChecksumError) as exc_info:
            client.file.put(str(local), "/remote/dest")
        assert exc_info.value.expected == local_checksum
        assert exc_info.value.actual == wrong_checksum

    def test_not_advertised_raises_not_supported_and_sends_nothing(
        self, tmp_path: Path
    ) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"data")
        client, mock_sock = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING]
        )
        with pytest.raises(TazError) as exc_info:
            client.file.put(str(local), "/remote/dest")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        mock_sock.sendmsg.assert_not_called()
        mock_sock.sendall.assert_not_called()

    def test_local_read_failure_sends_cancel_then_reraises(
        self, tmp_path: Path
    ) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"x" * 20)
        client, mock_sock = _connected_client_with_sock(
            _ack_bytes(),
            _cancel_response_bytes(True, stream_id=2),
            max_payload_sizes={common_pb2.FRAME_TYPE_FILE_CHUNK: 4},
        )
        boom = OSError("read error")
        real_open = open

        class _FailingFile:
            def __init__(self, path: str) -> None:
                self._f = real_open(path, "rb")
                self._reads = 0

            def read(self, n: int) -> bytes:
                self._reads += 1
                if self._reads == 2:
                    raise boom
                return self._f.read(n)

            def __enter__(self) -> _FailingFile:
                return self

            def __exit__(self, *exc: object) -> None:
                self._f.close()

        with (
            patch("taz.c3.file.open", lambda path, mode: _FailingFile(path)),
            pytest.raises(OSError, match="read error"),
        ):
            client.file.put(str(local), "/remote/dest")

        # one FILE_PUT REQUEST, one FILE_CHUNK, then a CANCEL REQUEST.
        frames = _sent_frames(mock_sock)
        assert frames[0].opcode == common_pb2.OPCODE_FILE_PUT
        assert frames[1].type == common_pb2.FRAME_TYPE_FILE_CHUNK
        assert frames[2].opcode == common_pb2.OPCODE_CANCEL

    def test_local_file_shrinks_mid_upload_raises_and_sends_cancel(
        self, tmp_path: Path
    ) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"x" * 20)
        client, mock_sock = _connected_client_with_sock(
            _ack_bytes(),
            _cancel_response_bytes(True, stream_id=2),
            max_payload_sizes={common_pb2.FRAME_TYPE_FILE_CHUNK: 4},
        )
        real_open = open

        class _ShrinkingFile:
            def __init__(self, path: str) -> None:
                self._f = real_open(path, "rb")
                self._reads = 0

            def read(self, n: int) -> bytes:
                self._reads += 1
                if self._reads >= 3:
                    # Simulate another process truncating the file: a real
                    # EOF arrives well before the pre-transfer os.stat()
                    # size (20) is reached.
                    return b""
                return self._f.read(n)

            def __enter__(self) -> _ShrinkingFile:
                return self

            def __exit__(self, *exc: object) -> None:
                self._f.close()

        with (
            patch("taz.c3.file.open", lambda path, mode: _ShrinkingFile(path)),
            pytest.raises(OSError, match="shrank"),
        ):
            client.file.put(str(local), "/remote/dest")

        # one FILE_PUT REQUEST, two FILE_CHUNKs from the reads before the
        # shrink was detected, then a CANCEL REQUEST - no infinite loop of
        # empty chunks.
        frames = _sent_frames(mock_sock)
        assert frames[0].opcode == common_pb2.OPCODE_FILE_PUT
        assert frames[1].type == common_pb2.FRAME_TYPE_FILE_CHUNK
        assert frames[2].type == common_pb2.FRAME_TYPE_FILE_CHUNK
        assert frames[3].opcode == common_pb2.OPCODE_CANCEL
        assert len(frames) == 4

    def test_local_read_failure_without_cancel_advertised_just_reraises(
        self, tmp_path: Path
    ) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"x" * 20)
        client, mock_sock = _connected_client_with_sock(
            _ack_bytes(),
            operations=[common_pb2.OPCODE_PING, common_pb2.OPCODE_FILE_PUT],
            max_payload_sizes={common_pb2.FRAME_TYPE_FILE_CHUNK: 4},
        )

        def _raising_open(path: str, mode: str) -> None:
            raise OSError("cannot open")

        with (
            patch("taz.c3.file.open", _raising_open),
            pytest.raises(OSError, match="cannot open"),
        ):
            client.file.put(str(local), "/remote/dest")

        frames = _sent_frames(mock_sock)
        assert len(frames) == 1  # only the FILE_PUT REQUEST, no CANCEL sent
        assert frames[0].opcode == common_pb2.OPCODE_FILE_PUT

    def test_keepalive_forwarded_to_every_receive(self, tmp_path: Path) -> None:
        local = tmp_path / "src.bin"
        local.write_bytes(b"data")
        checksum = google_crc32c.value(b"data").to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _ack_bytes(), _confirm_bytes(4, checksum)
        )
        kv = Keepalive(idle=5, timeout=5)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as spy:
            client.file.put(str(local), "/remote/dest", keepalive=kv)
        assert spy.call_count == 2
        for call in spy.call_args_list:
            assert call.args[1] is kv
