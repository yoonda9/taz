"""Tests for FileNamespace.create/delete/stat/chmod() and FileStat/Kind."""

from __future__ import annotations

import socket
from collections.abc import Callable
from typing import cast
from unittest.mock import MagicMock, patch

import pytest
from taz.c3.client import TazClient
from taz.c3.errors import TazError
from taz.c3.file import _REQUEST_LIMIT, FileStat, FileTransfer, Kind
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, file_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_command.py)
# ---------------------------------------------------------------------------

_FILE_OPS = [
    common_pb2.OPCODE_PING,
    common_pb2.OPCODE_FILE_CREATE,
    common_pb2.OPCODE_FILE_DELETE,
    common_pb2.OPCODE_FILE_STAT,
    common_pb2.OPCODE_FILE_CHMOD,
]


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    from taz.v1 import daemon_control_pb2

    ops = operations if operations is not None else _FILE_OPS
    cap = daemon_control_pb2.CapabilityPayload(
        protocol_major=1,
        protocol_minor=0,
        operations=ops,
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


_SOCKET_SPEC = sorted({*dir(socket.socket), "sendmsg"})


def _mock_sock(*chunks: bytes) -> MagicMock:
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


def _connected_client(
    *extra_chunks: bytes, operations: list[int] | None = None
) -> TazClient:
    client, _ = _connected_client_with_sock(*extra_chunks, operations=operations)
    return client


def _connected_client_with_sock(
    *extra_chunks: bytes, operations: list[int] | None = None
) -> tuple[TazClient, MagicMock]:
    mock_sock = _mock_sock(_capability_bytes(operations=operations), *extra_chunks)
    client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
    with patch(_PATCH_CC, return_value=mock_sock):
        client.connect()
    return client, mock_sock


def _success_bytes(opcode: int, response_cls: type) -> bytes:
    return _response_bytes(opcode, response_cls(success=True).SerializeToString())


def _content_at_serialized_size(path: str, target_size: int) -> bytes:
    """Content bytes such that ``FileCreateRequest(path, content=...)`` serializes to
    exactly ``target_size`` bytes.

    Derives the content length from the actual wire encoding (varint tag/length
    bytes included) instead of a hardcoded per-path overhead, since that overhead
    depends on the length-prefix varint width, which itself depends on the content
    length being solved for.
    """

    def size_for(n: int) -> int:
        return file_pb2.FileCreateRequest(path=path, content=b"x" * n).ByteSize()

    content_len = max(target_size - len(path) - 16, 0)
    size = size_for(content_len)
    while size < target_size:
        content_len += 1
        size = size_for(content_len)
    while size > target_size:
        content_len -= 1
        size = size_for(content_len)
    assert size == target_size
    return b"x" * content_len


# ---------------------------------------------------------------------------
# file.create()
# ---------------------------------------------------------------------------


class TestFileCreate:
    def test_create_sends_path_content_permissions(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_FILE_CREATE, file_pb2.FileCreateResponse)
        )
        result = client.file.create(  # type: ignore[func-returns-value]
            "/srv/app/hello.txt", content=b"hi", permissions=0o600
        )
        assert result is None
        sent = b"".join(mock_sock.sent)
        req = file_pb2.FileCreateRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.path == "/srv/app/hello.txt"
        assert req.content == b"hi"
        assert req.permissions == 0o600

    def test_create_default_content_and_permissions(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_FILE_CREATE, file_pb2.FileCreateResponse)
        )
        result = client.file.create(  # type: ignore[func-returns-value]
            "/srv/app/empty.txt"
        )
        assert result is None
        sent = b"".join(mock_sock.sent)
        req = file_pb2.FileCreateRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.path == "/srv/app/empty.txt"
        assert req.content == b""
        assert req.permissions == 0

    def test_create_oversized_content_raises_value_error_without_round_trip(
        self,
    ) -> None:
        client, mock_sock = _connected_client_with_sock()
        content = b"x" * (100 * 1024)
        with pytest.raises(ValueError, match=r"file\.put") as exc_info:
            client.file.create("/srv/app/big.bin", content=content)
        assert str(len(content)) in str(exc_info.value)
        assert str(_REQUEST_LIMIT) in str(exc_info.value)
        # Only the capability handshake's bytes were ever read; nothing sent.
        assert mock_sock.sendmsg.call_count == 0
        assert mock_sock.sendall.call_count == 0

    def test_create_content_at_exact_request_limit_sends(self) -> None:
        # The guard must measure the *serialized request*, not raw content
        # length: this content's own length is under _REQUEST_LIMIT, but once
        # wrapped in a FileCreateRequest (path + field tags/length varints)
        # the serialized size lands on exactly _REQUEST_LIMIT, which must
        # still be accepted.
        path = "/p"
        content = _content_at_serialized_size(path, _REQUEST_LIMIT)
        assert len(content) < _REQUEST_LIMIT
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_FILE_CREATE, file_pb2.FileCreateResponse)
        )
        result = client.file.create(  # type: ignore[func-returns-value]
            path, content=content
        )
        assert result is None
        sent = b"".join(mock_sock.sent)
        req = file_pb2.FileCreateRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert len(req.content) == len(content)
        assert req.ByteSize() == _REQUEST_LIMIT

    def test_create_content_one_byte_over_request_limit_raises(self) -> None:
        path = "/p"
        content = _content_at_serialized_size(path, _REQUEST_LIMIT) + b"x"
        assert (
            file_pb2.FileCreateRequest(path=path, content=content).ByteSize()
            == _REQUEST_LIMIT + 1
        )
        client, mock_sock = _connected_client_with_sock()
        with pytest.raises(ValueError, match=r"file\.put"):
            client.file.create(path, content=content)
        assert mock_sock.sendmsg.call_count == 0
        assert mock_sock.sendall.call_count == 0

    def test_create_existing_path_raises_already_exists(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_FILE_CREATE,
            common_pb2.ERROR_CODE_ALREADY_EXISTS,
            "path already exists",
            detail="/srv/app/exists.txt",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.file.create("/srv/app/exists.txt")
        assert exc_info.value.code == common_pb2.ERROR_CODE_ALREADY_EXISTS
        assert exc_info.value.message == "path already exists"
        assert exc_info.value.detail == "/srv/app/exists.txt"

    def test_create_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _success_bytes(common_pb2.OPCODE_FILE_CREATE, file_pb2.FileCreateResponse)
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.file.create("/srv/app/hello.txt", keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# file.delete()
# ---------------------------------------------------------------------------


class TestFileDelete:
    def test_delete_sends_path(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_FILE_DELETE, file_pb2.FileDeleteResponse)
        )
        result = client.file.delete(  # type: ignore[func-returns-value]
            "/srv/app/gone.txt"
        )
        assert result is None
        sent = b"".join(mock_sock.sent)
        req = file_pb2.FileDeleteRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.path == "/srv/app/gone.txt"

    def test_delete_missing_path_raises_not_found(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_FILE_DELETE,
            common_pb2.ERROR_CODE_NOT_FOUND,
            "path not found",
            detail="/srv/app/missing.txt",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.file.delete("/srv/app/missing.txt")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        assert exc_info.value.message == "path not found"
        assert exc_info.value.detail == "/srv/app/missing.txt"

    def test_delete_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _success_bytes(common_pb2.OPCODE_FILE_DELETE, file_pb2.FileDeleteResponse)
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.file.delete("/srv/app/gone.txt", keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# file.stat()
# ---------------------------------------------------------------------------


class TestFileStat:
    def test_kind_values_match_wire_protocol(self) -> None:
        # Kind is an IntEnum, so members compare equal to their wire value.
        assert int(Kind.UNSPECIFIED) == 0
        assert int(Kind.FILE) == 1
        assert int(Kind.DIR) == 2
        assert int(Kind.SYMLINK) == 3
        assert int(Kind.OTHER) == 4

    def test_stat_returns_file_stat(self) -> None:
        resp = file_pb2.FileStatResponse(
            size=42,
            permissions=0o644,
            owner="alice",
            modified=1000,
            created=900,
            kind=common_pb2.KIND_FILE,
            link_target="",
        )
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_FILE_STAT, resp.SerializeToString())
        )
        info = client.file.stat("/srv/app/hello.txt")
        assert info == FileStat(
            size=42,
            permissions=0o644,
            owner="alice",
            modified=1000,
            created=900,
            kind=Kind.FILE,
            link_target="",
        )

    def test_stat_symlink_reports_link_target(self) -> None:
        resp = file_pb2.FileStatResponse(
            size=0,
            permissions=0o777,
            kind=common_pb2.KIND_SYMLINK,
            link_target="/srv/app/real.txt",
        )
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_FILE_STAT, resp.SerializeToString())
        )
        info = client.file.stat("/srv/app/link.txt")
        assert info.kind == Kind.SYMLINK
        assert info.link_target == "/srv/app/real.txt"

    def test_stat_dir_kind(self) -> None:
        resp = file_pb2.FileStatResponse(kind=common_pb2.KIND_DIR)
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_FILE_STAT, resp.SerializeToString())
        )
        assert client.file.stat("/srv/app").kind == Kind.DIR

    def test_stat_other_kind(self) -> None:
        resp = file_pb2.FileStatResponse(kind=common_pb2.KIND_OTHER)
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_FILE_STAT, resp.SerializeToString())
        )
        assert client.file.stat("/dev/null").kind == Kind.OTHER

    def test_stat_unknown_kind_maps_to_other(self) -> None:
        # As parsed from a newer daemon (proto3 enums are open).
        future_kind = cast(common_pb2.Kind, 99)
        resp = file_pb2.FileStatResponse(kind=future_kind)
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_FILE_STAT, resp.SerializeToString())
        )
        assert client.file.stat("/srv/app/mystery").kind == Kind.OTHER

    def test_stat_missing_path_raises_not_found(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_FILE_STAT,
            common_pb2.ERROR_CODE_NOT_FOUND,
            "path not found",
            detail="/srv/app/missing.txt",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.file.stat("/srv/app/missing.txt")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        assert exc_info.value.message == "path not found"
        assert exc_info.value.detail == "/srv/app/missing.txt"

    def test_stat_forwards_per_call_keepalive(self) -> None:
        resp = file_pb2.FileStatResponse(kind=common_pb2.KIND_FILE)
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_FILE_STAT, resp.SerializeToString())
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.file.stat("/srv/app/hello.txt", keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# file.chmod()
# ---------------------------------------------------------------------------


class TestFileChmod:
    def test_chmod_sends_path_and_permissions(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_FILE_CHMOD, file_pb2.FileChmodResponse)
        )
        result = client.file.chmod(  # type: ignore[func-returns-value]
            "/srv/app/hello.txt", 0o600
        )
        assert result is None
        sent = b"".join(mock_sock.sent)
        req = file_pb2.FileChmodRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.path == "/srv/app/hello.txt"
        assert req.permissions == 0o600

    def test_chmod_missing_path_raises_not_found(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_FILE_CHMOD,
            common_pb2.ERROR_CODE_NOT_FOUND,
            "path not found",
            detail="/srv/app/missing.txt",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.file.chmod("/srv/app/missing.txt", 0o600)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        assert exc_info.value.message == "path not found"
        assert exc_info.value.detail == "/srv/app/missing.txt"

    def test_chmod_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _success_bytes(common_pb2.OPCODE_FILE_CHMOD, file_pb2.FileChmodResponse)
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.file.chmod("/srv/app/hello.txt", 0o600, keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# Local opcode gating
# ---------------------------------------------------------------------------


class TestFileUnadvertised:
    @pytest.mark.parametrize(
        "call",
        [
            lambda client: client.file.create("/srv/app/hello.txt"),
            lambda client: client.file.delete("/srv/app/hello.txt"),
            lambda client: client.file.stat("/srv/app/hello.txt"),
            lambda client: client.file.chmod("/srv/app/hello.txt", 0o600),
        ],
        ids=["create", "delete", "stat", "chmod"],
    )
    def test_unadvertised_opcode_raises_not_supported_locally(
        self, call: Callable[[TazClient], None]
    ) -> None:
        client, mock_sock = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING]
        )
        with pytest.raises(TazError) as exc_info:
            call(client)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        assert not client._conn.closed
        # Nothing was sent: the opcode was gated locally against capabilities.
        assert mock_sock.sendmsg.call_count == 0
        assert mock_sock.sendall.call_count == 0


class TestFileTransfer:
    def test_is_a_frozen_slotted_dataclass(self) -> None:
        transfer = FileTransfer(size=5, checksum=b"\x01\x02\x03\x04")
        assert transfer.size == 5
        assert transfer.checksum == b"\x01\x02\x03\x04"
        with pytest.raises(AttributeError):
            transfer.size = 6  # type: ignore[misc]


class TestFileExports:
    def test_file_stat_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.FileStat is FileStat

    def test_kind_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.Kind is Kind

    def test_file_transfer_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.FileTransfer is FileTransfer

    def test_both_in_all(self) -> None:
        import taz.c3

        assert "FileStat" in taz.c3.__all__
        assert "FileTransfer" in taz.c3.__all__
        assert "Kind" in taz.c3.__all__
