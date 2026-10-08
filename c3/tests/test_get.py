"""Tests for FileNamespace.get()."""

from __future__ import annotations

import socket
import tempfile
from pathlib import Path
from unittest.mock import MagicMock, patch

import crc32c
import pytest
from taz.c3.client import TazClient
from taz.c3.errors import TazChecksumError, TazError, TazProtocolError
from taz.c3.file import FileTransfer
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header, unpack_header
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, file_pb2

# Captured before any test patches "taz.c3.file.tempfile.NamedTemporaryFile"
# (which patches the real, shared tempfile module attribute): a helper that
# called tempfile.NamedTemporaryFile directly while patched would recurse
# into itself.
_real_named_temporary_file = tempfile.NamedTemporaryFile

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_put.py)
# ---------------------------------------------------------------------------

_GET_OPS = [
    common_pb2.OPCODE_PING,
    common_pb2.OPCODE_CANCEL,
    common_pb2.OPCODE_FILE_GET,
]


def _capability_bytes(
    operations: list[int] | None = None,
    max_payload_sizes: dict[int, int] | None = None,
) -> bytes:
    from taz.v1 import daemon_control_pb2

    ops = operations if operations is not None else _GET_OPS
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


def _meta_bytes(
    size: int, checksum: bytes, permissions: int = 0o644, stream_id: int = 1
) -> bytes:
    resp = file_pb2.FileGetResponse(
        size=size, permissions=permissions, checksum=checksum
    )
    return _response_bytes(
        common_pb2.OPCODE_FILE_GET, resp.SerializeToString(), stream_id=stream_id
    )


def _chunk_bytes(data: bytes, *, last: bool, stream_id: int = 1) -> bytes:
    header = pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_FILE_CHUNK,
            flags=0 if last else common_pb2.FRAME_FLAG_CONTINUATION,
            opcode=0,
            length=len(data),
            stream_id=stream_id,
        )
    )
    return header + data


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
# get()
# ---------------------------------------------------------------------------


class TestGet:
    def test_sends_file_get_request_with_src(self, tmp_path: Path) -> None:
        data = b"hello"
        checksum = crc32c.crc32c(data).to_bytes(4, "little")
        client, mock_sock = _connected_client_with_sock(
            _meta_bytes(len(data), checksum),
            _chunk_bytes(data, last=True),
        )
        client.file.get("/remote/src", str(tmp_path / "dest.bin"))
        frames = _sent_frames(mock_sock)
        assert len(frames) == 1  # only the FILE_GET REQUEST
        assert frames[0].type == common_pb2.FRAME_TYPE_REQUEST
        assert frames[0].opcode == common_pb2.OPCODE_FILE_GET
        req = file_pb2.FileGetRequest()
        req.ParseFromString(frames[0].payload)
        assert req.src == "/remote/src"

    def test_small_download_single_chunk_writes_file_and_returns_transfer(
        self, tmp_path: Path
    ) -> None:
        data = b"hello world"
        checksum = crc32c.crc32c(data).to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _meta_bytes(len(data), checksum),
            _chunk_bytes(data, last=True),
        )
        dest = tmp_path / "dest.bin"
        result = client.file.get("/remote/src", str(dest))
        assert result == FileTransfer(size=len(data), checksum=checksum)
        assert dest.read_bytes() == data
        # No leftover temp file next to the destination.
        assert list(tmp_path.iterdir()) == [dest]

    def test_zero_byte_download(self, tmp_path: Path) -> None:
        checksum = crc32c.crc32c(b"").to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _meta_bytes(0, checksum),
            _chunk_bytes(b"", last=True),
        )
        dest = tmp_path / "dest.bin"
        result = client.file.get("/remote/src", str(dest))
        assert result == FileTransfer(size=0, checksum=checksum)
        assert dest.read_bytes() == b""

    def test_multi_chunk_download_reassembles_bytes_in_order(
        self, tmp_path: Path
    ) -> None:
        data = b"ABCDEFGHIJ"
        checksum = crc32c.crc32c(data).to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _meta_bytes(len(data), checksum),
            _chunk_bytes(b"ABCD", last=False),
            _chunk_bytes(b"EFGH", last=False),
            _chunk_bytes(b"IJ", last=True),
        )
        dest = tmp_path / "dest.bin"
        result = client.file.get("/remote/src", str(dest))
        assert result == FileTransfer(size=len(data), checksum=checksum)
        assert dest.read_bytes() == data

    def test_response_error_raises_and_no_temp_file_left(self, tmp_path: Path) -> None:
        client, mock_sock = _connected_client_with_sock(
            _error_bytes(
                common_pb2.OPCODE_FILE_GET,
                common_pb2.ERROR_CODE_NOT_FOUND,
                "no such file",
            )
        )
        dest = tmp_path / "dest.bin"
        with pytest.raises(TazError) as exc_info:
            client.file.get("/remote/src", str(dest))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        assert not dest.exists()
        assert list(tmp_path.iterdir()) == []
        assert len(mock_sock.sent) == 1  # only the FILE_GET REQUEST

    def test_mid_transfer_error_frame_deletes_temp_and_raises(
        self, tmp_path: Path
    ) -> None:
        data = b"partial"
        checksum = crc32c.crc32c(data + b"more").to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _meta_bytes(len(data) + 4, checksum),
            _chunk_bytes(data, last=False),
            _error_bytes(
                common_pb2.OPCODE_FILE_GET,
                common_pb2.ERROR_CODE_INTERNAL,
                "file changed during transfer",
            ),
        )
        dest = tmp_path / "dest.bin"
        with pytest.raises(TazError) as exc_info:
            client.file.get("/remote/src", str(dest))
        assert exc_info.value.code == common_pb2.ERROR_CODE_INTERNAL
        assert not dest.exists()
        assert list(tmp_path.iterdir()) == []

    def test_unexpected_frame_type_mid_transfer_raises_protocol_error(
        self, tmp_path: Path
    ) -> None:
        data = b"partial"
        checksum = crc32c.crc32c(data).to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _meta_bytes(len(data), checksum),
            _chunk_bytes(data, last=False),
            _response_bytes(common_pb2.OPCODE_FILE_GET, b""),
        )
        dest = tmp_path / "dest.bin"
        with pytest.raises(TazProtocolError):
            client.file.get("/remote/src", str(dest))
        assert not dest.exists()
        assert list(tmp_path.iterdir()) == []

    def test_fewer_bytes_than_announced_raises_checksum_error_no_temp_left(
        self, tmp_path: Path
    ) -> None:
        data = b"short"
        wrong_size_checksum = crc32c.crc32c(data + b"xxx").to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _meta_bytes(len(data) + 3, wrong_size_checksum),
            _chunk_bytes(data, last=True),
        )
        dest = tmp_path / "dest.bin"
        with pytest.raises(TazChecksumError):
            client.file.get("/remote/src", str(dest))
        assert not dest.exists()
        assert list(tmp_path.iterdir()) == []

    def test_checksum_mismatch_raises_checksum_error_no_temp_left(
        self, tmp_path: Path
    ) -> None:
        data = b"data"
        actual_checksum = crc32c.crc32c(data).to_bytes(4, "little")
        wrong_checksum = b"\xff\xff\xff\xff"
        client, _ = _connected_client_with_sock(
            _meta_bytes(len(data), wrong_checksum),
            _chunk_bytes(data, last=True),
        )
        dest = tmp_path / "dest.bin"
        preexisting = b"pre-existing content"
        dest.write_bytes(preexisting)
        with pytest.raises(TazChecksumError) as exc_info:
            client.file.get("/remote/src", str(dest))
        assert exc_info.value.expected == wrong_checksum
        assert exc_info.value.actual == actual_checksum
        assert dest.read_bytes() == preexisting
        assert list(tmp_path.iterdir()) == [dest]

    def test_not_advertised_raises_not_supported_and_sends_nothing(
        self, tmp_path: Path
    ) -> None:
        client, mock_sock = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING]
        )
        dest = tmp_path / "dest.bin"
        with pytest.raises(TazError) as exc_info:
            client.file.get("/remote/src", str(dest))
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        mock_sock.sendmsg.assert_not_called()
        mock_sock.sendall.assert_not_called()
        assert list(tmp_path.iterdir()) == []

    def test_local_write_failure_sends_cancel_then_reraises_and_deletes_temp(
        self, tmp_path: Path
    ) -> None:
        data = b"x" * 20
        checksum = crc32c.crc32c(data).to_bytes(4, "little")
        client, mock_sock = _connected_client_with_sock(
            _meta_bytes(len(data), checksum),
            _chunk_bytes(b"x" * 4, last=False),
            _chunk_bytes(b"x" * 4, last=False),
            _cancel_response_bytes(True, stream_id=2),
        )
        dest = tmp_path / "dest.bin"
        boom = OSError("disk full")

        class _FailingTmp:
            # Keyword names match tempfile.NamedTemporaryFile's: this
            # replaces it via patch("taz.c3.file.tempfile.NamedTemporaryFile").
            def __init__(
                self,
                *,
                dir: str,  # noqa: A002
                prefix: str,
                suffix: str,
                delete: bool,
            ) -> None:
                self._f = _real_named_temporary_file(
                    dir=dir, prefix=prefix, suffix=suffix, delete=delete
                )
                self._writes = 0
                self.name = self._f.name

            def write(self, data: bytes) -> int:
                self._writes += 1
                if self._writes == 2:
                    raise boom
                return self._f.write(data)

            def close(self) -> None:
                self._f.close()

        with (
            patch("taz.c3.file.tempfile.NamedTemporaryFile", _FailingTmp),
            pytest.raises(OSError, match="disk full"),
        ):
            client.file.get("/remote/src", str(dest))

        assert not dest.exists()
        assert list(tmp_path.iterdir()) == []
        # metadata RESPONSE already consumed; only the CANCEL REQUEST is new.
        frames = _sent_frames(mock_sock)
        assert frames[0].opcode == common_pb2.OPCODE_FILE_GET
        assert frames[1].opcode == common_pb2.OPCODE_CANCEL

    def test_local_write_failure_without_cancel_advertised_just_reraises(
        self, tmp_path: Path
    ) -> None:
        data = b"x" * 20
        checksum = crc32c.crc32c(data).to_bytes(4, "little")
        client, mock_sock = _connected_client_with_sock(
            _meta_bytes(len(data), checksum),
            _chunk_bytes(b"x" * 4, last=False),
            operations=[common_pb2.OPCODE_PING, common_pb2.OPCODE_FILE_GET],
        )
        dest = tmp_path / "dest.bin"

        class _FailingTmp:
            def __init__(
                self,
                *,
                dir: str,  # noqa: A002
                prefix: str,
                suffix: str,
                delete: bool,
            ) -> None:
                self._f = _real_named_temporary_file(
                    dir=dir, prefix=prefix, suffix=suffix, delete=delete
                )
                self.name = self._f.name

            def write(self, data: bytes) -> int:
                raise OSError("disk full")

            def close(self) -> None:
                self._f.close()

        with (
            patch("taz.c3.file.tempfile.NamedTemporaryFile", _FailingTmp),
            pytest.raises(OSError, match="disk full"),
        ):
            client.file.get("/remote/src", str(dest))

        assert not dest.exists()
        assert list(tmp_path.iterdir()) == []
        frames = _sent_frames(mock_sock)
        assert len(frames) == 1  # only the FILE_GET REQUEST, no CANCEL sent
        assert frames[0].opcode == common_pb2.OPCODE_FILE_GET

    def test_keepalive_forwarded_to_every_receive(self, tmp_path: Path) -> None:
        data = b"data"
        checksum = crc32c.crc32c(data).to_bytes(4, "little")
        client, _ = _connected_client_with_sock(
            _meta_bytes(len(data), checksum),
            _chunk_bytes(data, last=True),
        )
        dest = tmp_path / "dest.bin"
        kv = Keepalive(idle=5, timeout=5)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as spy:
            client.file.get("/remote/src", str(dest), keepalive=kv)
        assert spy.call_count == 2
        for call in spy.call_args_list:
            assert call.args[1] is kv

    def test_temp_file_is_created_next_to_destination(self, tmp_path: Path) -> None:
        """The temp file must live in local_path's own directory for an atomic
        same-filesystem os.replace(), not in a platform tempdir."""
        data = b"data"
        checksum = crc32c.crc32c(data).to_bytes(4, "little")
        seen_dirs: list[str] = []

        def _recording_temp(
            *,
            dir: str,  # noqa: A002
            prefix: str,
            suffix: str,
            delete: bool,
        ) -> tempfile._TemporaryFileWrapper[bytes]:
            seen_dirs.append(str(dir))
            return _real_named_temporary_file(
                dir=dir, prefix=prefix, suffix=suffix, delete=delete
            )

        client, _ = _connected_client_with_sock(
            _meta_bytes(len(data), checksum),
            _chunk_bytes(data, last=True),
        )
        dest = tmp_path / "subdir"
        dest.mkdir()
        dest_file = dest / "dest.bin"
        with patch("taz.c3.file.tempfile.NamedTemporaryFile", _recording_temp):
            client.file.get("/remote/src", str(dest_file))
        assert seen_dirs == [str(dest)]
