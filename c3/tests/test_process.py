"""Tests for ProcessNamespace.list()/kill()/info() and ProcessInfo/ProcessDetail."""

from __future__ import annotations

import signal
import socket
from unittest.mock import MagicMock, patch

import pytest
from taz.c3.client import TazClient
from taz.c3.errors import TazError
from taz.c3.process import ProcessDetail, ProcessInfo
from taz.c3.protocol.frame import HEADER_SIZE, Frame, pack_header
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, process_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_directory.py)
# ---------------------------------------------------------------------------

_PROCESS_OPS = [
    common_pb2.OPCODE_PING,
    common_pb2.OPCODE_PROCESS_LIST,
    common_pb2.OPCODE_PROCESS_KILL,
    common_pb2.OPCODE_PROCESS_INFO,
]


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    from taz.v1 import daemon_control_pb2

    ops = operations if operations is not None else _PROCESS_OPS
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


def _entry(pid: int) -> process_pb2.ProcessInfo:
    return process_pb2.ProcessInfo(
        pid=pid,
        name=f"p{pid:04d}",
        user="alice",
        cpu_percent=1.5,
        memory_bytes=2048,
        state="running",
    )


def _detail(
    pid: int = 1,
    command_line: str = "p --flag",
    start_time: int = 123,
    open_files: list[str] | None = None,
) -> process_pb2.ProcessInfoResponse:
    return process_pb2.ProcessInfoResponse(
        info=_entry(pid),
        command_line=command_line,
        start_time=start_time,
        open_files=open_files if open_files is not None else [],
    )


# ---------------------------------------------------------------------------
# process.list()
# ---------------------------------------------------------------------------


class TestProcessList:
    def test_list_sends_empty_filter_by_default(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_LIST,
                process_pb2.ProcessListResponse().SerializeToString(),
            )
        )
        result = client.process.list()
        assert result == []
        sent = b"".join(mock_sock.sent)
        req = process_pb2.ProcessListRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.filter == ""

    def test_list_sends_given_filter(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_LIST,
                process_pb2.ProcessListResponse().SerializeToString(),
            )
        )
        client.process.list(filter="py")
        sent = b"".join(mock_sock.sent)
        req = process_pb2.ProcessListRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.filter == "py"

    def test_list_maps_every_field(self) -> None:
        resp = process_pb2.ProcessListResponse(processes=[_entry(1), _entry(2)])
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_PROCESS_LIST, resp.SerializeToString())
        )
        entries = client.process.list()
        assert entries == [
            ProcessInfo(
                pid=1,
                name="p0001",
                user="alice",
                cpu_percent=1.5,
                memory_bytes=2048,
                state="running",
            ),
            ProcessInfo(
                pid=2,
                name="p0002",
                user="alice",
                cpu_percent=1.5,
                memory_bytes=2048,
                state="running",
            ),
        ]
        assert isinstance(entries[0].cpu_percent, float)
        assert isinstance(entries[0].memory_bytes, int)

    def test_list_empty_response_returns_empty_list(self) -> None:
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_LIST,
                process_pb2.ProcessListResponse().SerializeToString(),
            )
        )
        assert client.process.list() == []

    def test_list_chunked_response_concatenates_entries_in_frame_order(self) -> None:
        # One RESPONSE frame with 128 entries + CONTINUATION, a second with 3;
        # the dispatcher merges them (§6.1) before ProcessNamespace sees the
        # result, so entries must come back in frame order.
        frame1 = process_pb2.ProcessListResponse(
            processes=[_entry(pid) for pid in range(1, 129)]
        )
        frame2 = process_pb2.ProcessListResponse(
            processes=[_entry(pid) for pid in range(129, 132)]
        )
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_LIST,
                frame1.SerializeToString(),
                flags=common_pb2.FRAME_FLAG_CONTINUATION,
            ),
            _response_bytes(common_pb2.OPCODE_PROCESS_LIST, frame2.SerializeToString()),
        )
        entries = client.process.list()
        assert len(entries) == 131
        assert [entry.pid for entry in entries] == list(range(1, 132))

    def test_list_error_raises_taz_error(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_PROCESS_LIST,
            common_pb2.ERROR_CODE_INVALID_REQUEST,
            "filter too long",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.process.list(filter="a" * 256)
        assert exc_info.value.code == common_pb2.ERROR_CODE_INVALID_REQUEST
        assert exc_info.value.message == "filter too long"

    def test_list_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_LIST,
                process_pb2.ProcessListResponse().SerializeToString(),
            )
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.process.list(keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# process.kill()
# ---------------------------------------------------------------------------


class TestProcessKill:
    def test_kill_sends_default_signal(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_KILL,
                process_pb2.ProcessKillResponse(success=True).SerializeToString(),
            )
        )
        client.process.kill(42)
        sent = b"".join(mock_sock.sent)
        req = process_pb2.ProcessKillRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.pid == 42
        assert req.signal == 0

    def test_kill_sends_given_signal(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_KILL,
                process_pb2.ProcessKillResponse(success=True).SerializeToString(),
            )
        )
        client.process.kill(42, signal=10)
        sent = b"".join(mock_sock.sent)
        req = process_pb2.ProcessKillRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.signal == 10

    def test_kill_sends_signal_enum_as_int(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_KILL,
                process_pb2.ProcessKillResponse(success=True).SerializeToString(),
            )
        )
        client.process.kill(42, signal=signal.SIGTERM)
        sent = b"".join(mock_sock.sent)
        req = process_pb2.ProcessKillRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.signal == int(signal.SIGTERM)

    def test_kill_response_true(self) -> None:
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_KILL,
                process_pb2.ProcessKillResponse(success=True).SerializeToString(),
            )
        )
        assert client.process.kill(42) is True

    def test_kill_error_raises_taz_error(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_PROCESS_KILL,
            common_pb2.ERROR_CODE_NOT_FOUND,
            "no such process",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.process.kill(999999)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_kill_negative_pid_raises_value_error_and_sends_nothing(self) -> None:
        client, mock_sock = _connected_client_with_sock()
        with pytest.raises(ValueError):
            client.process.kill(-1)
        assert mock_sock.sent == []

    def test_kill_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_KILL,
                process_pb2.ProcessKillResponse(success=True).SerializeToString(),
            )
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.process.kill(42, keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# process.info()
# ---------------------------------------------------------------------------


class TestProcessInfo:
    def test_info_sends_request_with_pid(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_INFO, _detail().SerializeToString()
            )
        )
        client.process.info(7)
        sent = b"".join(mock_sock.sent)
        req = process_pb2.ProcessInfoRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.pid == 7

    def test_info_maps_every_field(self) -> None:
        resp = _detail(
            pid=7, command_line="p --flag", start_time=123, open_files=["a", "b"]
        )
        client = _connected_client(
            _response_bytes(common_pb2.OPCODE_PROCESS_INFO, resp.SerializeToString())
        )
        detail = client.process.info(7)
        assert detail == ProcessDetail(
            pid=7,
            name="p0007",
            user="alice",
            cpu_percent=1.5,
            memory_bytes=2048,
            state="running",
            command_line="p --flag",
            start_time=123,
            open_files=["a", "b"],
        )

    def test_info_chunked_response_merges_frames(self) -> None:
        # The daemon's layout: frame 1 has command_line + 32 open_files +
        # CONTINUATION; frame 2 has 5 more open_files + info + start_time,
        # since scalars come from the final frame.  The dispatcher merges
        # these (§6.1) before ProcessNamespace sees the result.
        frame1 = process_pb2.ProcessInfoResponse(
            command_line="p --flag",
            open_files=[f"f{i}" for i in range(32)],
        )
        frame2 = process_pb2.ProcessInfoResponse(
            info=_entry(7),
            open_files=[f"f{i}" for i in range(32, 37)],
            start_time=123,
        )
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_INFO,
                frame1.SerializeToString(),
                flags=common_pb2.FRAME_FLAG_CONTINUATION,
            ),
            _response_bytes(common_pb2.OPCODE_PROCESS_INFO, frame2.SerializeToString()),
        )
        detail = client.process.info(7)
        assert detail.open_files == [f"f{i}" for i in range(37)]
        assert detail.start_time == 123
        assert detail.pid == 7
        assert detail.command_line == "p --flag"

    def test_info_error_raises_taz_error(self) -> None:
        err = _error_bytes(
            common_pb2.OPCODE_PROCESS_INFO,
            common_pb2.ERROR_CODE_NOT_FOUND,
            "no such process",
        )
        client = _connected_client(err)
        with pytest.raises(TazError) as exc_info:
            client.process.info(999999)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_info_forwards_per_call_keepalive(self) -> None:
        client = _connected_client(
            _response_bytes(
                common_pb2.OPCODE_PROCESS_INFO, _detail().SerializeToString()
            )
        )
        kv = Keepalive(idle=60.0, timeout=10.0)
        with patch.object(
            client._dispatcher, "recv_response", wraps=client._dispatcher.recv_response
        ) as mock_recv:
            client.process.info(7, keepalive=kv)
        assert mock_recv.call_args.args[1] is kv


# ---------------------------------------------------------------------------
# ProcessInfo / ProcessDetail
# ---------------------------------------------------------------------------


class TestProcessDataclasses:
    def test_process_info_is_frozen_and_slotted(self) -> None:
        info = ProcessInfo(
            pid=1,
            name="p",
            user="u",
            cpu_percent=0.0,
            memory_bytes=0,
            state="running",
        )
        with pytest.raises(AttributeError):
            info.pid = 2  # type: ignore[misc]
        assert not hasattr(info, "__dict__")

    def test_process_detail_is_a_process_info(self) -> None:
        detail = ProcessDetail(
            pid=1,
            name="p",
            user="u",
            cpu_percent=0.0,
            memory_bytes=0,
            state="running",
            command_line="p --flag",
            start_time=123,
            open_files=["scratch/a"],
        )
        assert isinstance(detail, ProcessInfo)
        assert detail.command_line == "p --flag"
        assert detail.start_time == 123
        assert detail.open_files == ["scratch/a"]


class TestProcessExports:
    def test_process_info_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.ProcessInfo is ProcessInfo

    def test_process_detail_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.ProcessDetail is ProcessDetail

    def test_process_info_in_all(self) -> None:
        import taz.c3

        assert "ProcessInfo" in taz.c3.__all__

    def test_process_detail_in_all(self) -> None:
        import taz.c3

        assert "ProcessDetail" in taz.c3.__all__
