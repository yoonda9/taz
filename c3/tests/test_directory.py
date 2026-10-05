"""Tests for DirectoryNamespace.make/list/remove() and DirEntry."""

from __future__ import annotations

import socket
from collections.abc import Callable
from unittest.mock import MagicMock, patch

import pytest
from taz.c3.client import TazClient
from taz.c3.directory import DirEntry
from taz.c3.errors import TazError
from taz.c3.file import Kind
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, file_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_file.py)
# ---------------------------------------------------------------------------

_DIRECTORY_OPS = [
    common_pb2.OPCODE_PING,
    common_pb2.OPCODE_DIR_MAKE,
    common_pb2.OPCODE_DIR_LIST,
    common_pb2.OPCODE_DIR_REMOVE,
]


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    from taz.v1 import daemon_control_pb2

    ops = operations if operations is not None else _DIRECTORY_OPS
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


# ---------------------------------------------------------------------------
# directory.make()
# ---------------------------------------------------------------------------


class TestDirectoryMake:
    def test_make_sends_path_permissions_parents(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_DIR_MAKE, file_pb2.DirMakeResponse)
        )
        result = client.directory.make(  # type: ignore[func-returns-value]
            "/srv/app/sub", permissions=0o750, parents=True
        )
        assert result is None
        sent = b"".join(mock_sock.sent)
        req = file_pb2.DirMakeRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.path == "/srv/app/sub"
        assert req.permissions == 0o750
        assert req.parents is True

    def test_make_defaults_leave_permissions_and_parents_false(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_DIR_MAKE, file_pb2.DirMakeResponse)
        )
        result = client.directory.make(  # type: ignore[func-returns-value]
            "/srv/app/sub"
        )
        assert result is None
        sent = b"".join(mock_sock.sent)
        req = file_pb2.DirMakeRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.path == "/srv/app/sub"
        assert req.permissions == 0
        assert req.parents is False

    def test_make_existing_path_raises_already_exists(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_DIR_MAKE,
            common_pb2.ERROR_CODE_ALREADY_EXISTS,
            "path already exists",
            detail="/srv/app/sub",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.directory.make("/srv/app/sub")
        assert exc_info.value.code == common_pb2.ERROR_CODE_ALREADY_EXISTS
        assert exc_info.value.message == "path already exists"
        assert exc_info.value.detail == "/srv/app/sub"

    def test_make_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _success_bytes(common_pb2.OPCODE_DIR_MAKE, file_pb2.DirMakeResponse)
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.directory.make("/srv/app/sub", keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# directory.list()
# ---------------------------------------------------------------------------


class TestDirectoryList:
    def test_list_sends_path_include_hidden(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_DIR_LIST,
                file_pb2.DirListResponse().SerializeToString(),
            )
        )
        result = client.directory.list("/srv/app", include_hidden=True)
        assert result == []
        sent = b"".join(mock_sock.sent)
        req = file_pb2.DirListRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.path == "/srv/app"
        assert req.include_hidden is True

    def test_list_default_include_hidden_false(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_DIR_LIST,
                file_pb2.DirListResponse().SerializeToString(),
            )
        )
        client.directory.list("/srv/app")
        sent = b"".join(mock_sock.sent)
        req = file_pb2.DirListRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.include_hidden is False

    def test_list_maps_entries_with_kind(self) -> None:
        resp = file_pb2.DirListResponse(
            entries=[
                file_pb2.DirEntry(name="a.txt", kind=common_pb2.KIND_FILE, size=3),
                file_pb2.DirEntry(name="sub", kind=common_pb2.KIND_DIR, size=0),
                file_pb2.DirEntry(name="link", kind=common_pb2.KIND_SYMLINK, size=0),
            ]
        )
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_DIR_LIST, resp.SerializeToString())
        )
        entries = client.directory.list("/srv/app")
        assert entries == [
            DirEntry(name="a.txt", kind=Kind.FILE, size=3),
            DirEntry(name="sub", kind=Kind.DIR, size=0),
            DirEntry(name="link", kind=Kind.SYMLINK, size=0),
        ]

    def test_list_empty_directory_returns_empty_list(self) -> None:
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_DIR_LIST,
                file_pb2.DirListResponse().SerializeToString(),
            )
        )
        assert client.directory.list("/srv/app/empty") == []

    def test_list_chunked_response_concatenates_entries_in_frame_order(
        self,
    ) -> None:
        # Three RESPONSE frames on the same stream, CONTINUATION set on the
        # first two; the dispatcher merges them (§6.1) before DirectoryNamespace
        # sees the result, so entries must come back in frame order.
        frame1 = file_pb2.DirListResponse(
            entries=[file_pb2.DirEntry(name="a", kind=common_pb2.KIND_FILE, size=1)]
        )
        frame2 = file_pb2.DirListResponse(
            entries=[file_pb2.DirEntry(name="b", kind=common_pb2.KIND_FILE, size=2)]
        )
        frame3 = file_pb2.DirListResponse(
            entries=[file_pb2.DirEntry(name="c", kind=common_pb2.KIND_FILE, size=3)]
        )
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_DIR_LIST,
                frame1.SerializeToString(),
                flags=common_pb2.FRAME_FLAG_CONTINUATION,
            ),
            _response_bytes(
                common_pb2.OPCODE_DIR_LIST,
                frame2.SerializeToString(),
                flags=common_pb2.FRAME_FLAG_CONTINUATION,
            ),
            _response_bytes(
                common_pb2.OPCODE_DIR_LIST,
                frame3.SerializeToString(),
            ),
        )
        entries = client.directory.list("/srv/app")
        assert entries == [
            DirEntry(name="a", kind=Kind.FILE, size=1),
            DirEntry(name="b", kind=Kind.FILE, size=2),
            DirEntry(name="c", kind=Kind.FILE, size=3),
        ]

    def test_list_missing_path_raises_not_found(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_DIR_LIST,
            common_pb2.ERROR_CODE_NOT_FOUND,
            "path not found",
            detail="/srv/app/missing",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.directory.list("/srv/app/missing")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        assert exc_info.value.message == "path not found"
        assert exc_info.value.detail == "/srv/app/missing"

    def test_list_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_DIR_LIST,
                file_pb2.DirListResponse().SerializeToString(),
            )
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.directory.list("/srv/app", keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# directory.remove()
# ---------------------------------------------------------------------------


class TestDirectoryRemove:
    def test_remove_sends_path_recursive(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_DIR_REMOVE, file_pb2.DirRemoveResponse)
        )
        result = client.directory.remove(  # type: ignore[func-returns-value]
            "/srv/app/sub", recursive=True
        )
        assert result is None
        sent = b"".join(mock_sock.sent)
        req = file_pb2.DirRemoveRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.path == "/srv/app/sub"
        assert req.recursive is True

    def test_remove_default_recursive_false(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _success_bytes(common_pb2.OPCODE_DIR_REMOVE, file_pb2.DirRemoveResponse)
        )
        client.directory.remove("/srv/app/sub")
        sent = b"".join(mock_sock.sent)
        req = file_pb2.DirRemoveRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.recursive is False

    def test_remove_missing_path_raises_not_found(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_DIR_REMOVE,
            common_pb2.ERROR_CODE_NOT_FOUND,
            "path not found",
            detail="/srv/app/missing",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.directory.remove("/srv/app/missing")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND
        assert exc_info.value.message == "path not found"
        assert exc_info.value.detail == "/srv/app/missing"

    def test_remove_non_empty_without_recursive_raises(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_DIR_REMOVE,
            common_pb2.ERROR_CODE_INVALID_REQUEST,
            "directory not empty",
            detail="/srv/app/sub",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.directory.remove("/srv/app/sub")
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST

    def test_remove_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _success_bytes(common_pb2.OPCODE_DIR_REMOVE, file_pb2.DirRemoveResponse)
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.directory.remove("/srv/app/sub", keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# Local opcode gating
# ---------------------------------------------------------------------------


class TestDirectoryUnadvertised:
    @pytest.mark.parametrize(
        "call",
        [
            lambda client: client.directory.make("/srv/app/sub"),
            lambda client: client.directory.list("/srv/app"),
            lambda client: client.directory.remove("/srv/app/sub"),
        ],
        ids=["make", "list", "remove"],
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


class TestDirectoryExports:
    def test_dir_entry_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.DirEntry is DirEntry

    def test_dir_entry_in_all(self) -> None:
        import taz.c3

        assert "DirEntry" in taz.c3.__all__
