"""Tests for TaskNamespace.detach()/status() and DetachedTask/TaskStatus."""

from __future__ import annotations

import socket
from unittest.mock import MagicMock, patch

import pytest
import taz.c3
from taz.c3.client import TazClient
from taz.c3.command import CommandResult
from taz.c3.errors import TazError, TazPriorityError
from taz.c3.settings import Keepalive
from taz.c3.stream import StreamIterator
from taz.c3.task import DetachedTask, TaskState, TaskStatus
from taz.v1 import advanced_pb2, command_pb2, common_pb2

# ---------------------------------------------------------------------------
# Helpers (same pattern as test_command.py)
# ---------------------------------------------------------------------------

_DETACH_OPS = [
    common_pb2.OPCODE_PING,
    common_pb2.OPCODE_COMMAND_EXEC,
    common_pb2.OPCODE_DETACH,
    common_pb2.OPCODE_TASK_STATUS,
    common_pb2.OPCODE_TASK_CANCEL,
]


def _capability_bytes(operations: list[int] | None = None) -> bytes:
    from taz.c3.protocol.frame import Frame, pack_header
    from taz.v1 import daemon_control_pb2

    ops = operations if operations is not None else _DETACH_OPS
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
    from taz.c3.protocol.frame import Frame, pack_header

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


def _connected_client_with_sock(
    *extra_chunks: bytes, operations: list[int] | None = None
) -> tuple[TazClient, MagicMock]:
    mock_sock = _mock_sock(_capability_bytes(operations=operations), *extra_chunks)
    client = TazClient("127.0.0.1", 5555, keepalive=Keepalive.OFF)
    with patch(_PATCH_CC, return_value=mock_sock):
        client.connect()
    return client, mock_sock


def _detach_response(task_id: int = 7, pid: int = 1234) -> bytes:
    resp = advanced_pb2.DetachResponse(task_id=task_id, pid=pid)
    return _response_bytes(common_pb2.OPCODE_DETACH, resp.SerializeToString())


def _exec_response_bytes(
    exit_code: int = 0,
    stdout: bytes = b"",
    stderr: bytes = b"",
    timed_out: bool = False,
    truncated: bool = False,
) -> bytes:
    resp = command_pb2.CommandExecResponse(
        exit_code=exit_code,
        stdout_data=stdout,
        stderr_data=stderr,
        timed_out=timed_out,
        truncated=truncated,
    )
    return resp.SerializeToString()


def _task_cancel_response(accepted: bool = True) -> bytes:
    resp = advanced_pb2.TaskCancelResponse(accepted=accepted)
    return _response_bytes(common_pb2.OPCODE_TASK_CANCEL, resp.SerializeToString())


def _task_status_response_bytes(
    *,
    state: advanced_pb2.TaskState = advanced_pb2.TASK_STATE_RUNNING,
    pid: int = 1234,
    progress: int = 0,
    progress_total: int = 0,
    result: bytes = b"",
    error: common_pb2.ErrorInfo | None = None,
    stream_id: int = 1,
    flags: int = 0,
) -> bytes:
    resp = advanced_pb2.TaskStatusResponse(
        state=state,
        pid=pid,
        progress=progress,
        progress_total=progress_total,
        result=result,
    )
    if error is not None:
        resp.error.CopyFrom(error)
    return _response_bytes(
        common_pb2.OPCODE_TASK_STATUS,
        resp.SerializeToString(),
        stream_id=stream_id,
        flags=flags,
    )


def _error_bytes(
    code: common_pb2.ErrorCode,
    message: str = "boom",
    *,
    opcode: int,
    stream_id: int = 1,
    flags: int = 0,
) -> bytes:
    from taz.c3.protocol.frame import Frame, pack_header

    info = common_pb2.ErrorInfo(code=code, message=message)
    payload = info.SerializeToString()
    header = pack_header(
        Frame(
            type=common_pb2.FRAME_TYPE_ERROR,
            flags=flags,
            opcode=opcode,
            length=len(payload),
            stream_id=stream_id,
        )
    )
    return header + payload


def _priority_error_bytes(
    stream_id: int,
    *,
    code: common_pb2.ErrorCode = common_pb2.ERROR_CODE_INTERNAL,
    message: str = "exit code 1",
) -> bytes:
    """A FRAME_FLAG_PRIORITY ERROR frame for the DETACH stream (§10.2)."""
    return _error_bytes(
        code,
        message,
        opcode=common_pb2.OPCODE_DETACH,
        stream_id=stream_id,
        flags=common_pb2.FRAME_FLAG_PRIORITY,
    )


# ---------------------------------------------------------------------------
# task.detach()
# ---------------------------------------------------------------------------


class TestTaskDetach:
    def test_detach_returns_detached_task(self) -> None:
        client, _ = _connected_client_with_sock(_detach_response(task_id=7, pid=1234))
        task = client.task.detach(client.command.exec, "sleep", args=["5"])
        assert task == DetachedTask(task_id=7, pid=1234)

    def test_detach_sends_exec_fields_wrapped_in_detach_request(self) -> None:
        client, mock_sock = _connected_client_with_sock(_detach_response())
        client.task.detach(
            client.command.exec,
            "cmd",
            args=["a", "b"],
            env={"FOO": "bar"},
            working_dir="/srv/app",
            timeout_ms=5000,
            as_user="bob",
        )
        from taz.c3.protocol.frame import HEADER_SIZE

        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.DetachRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.WhichOneof("task") == "exec"
        assert req.exec.command == "cmd"
        assert list(req.exec.args) == ["a", "b"]
        assert [(kv.key, kv.value) for kv in req.exec.env] == [("FOO", "bar")]
        assert req.exec.working_dir == "/srv/app"
        assert req.exec.timeout_ms == 5000
        assert req.exec.as_user == "bob"

    def test_detach_defaults_monitor_true_and_error_priority_none(self) -> None:
        client, mock_sock = _connected_client_with_sock(_detach_response())
        client.task.detach(client.command.exec, "cmd")
        from taz.c3.protocol.frame import HEADER_SIZE

        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.DetachRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.monitor is True
        assert req.error_priority == common_pb2.ERROR_PRIORITY_NONE

    def test_detach_sends_monitor_false(self) -> None:
        client, mock_sock = _connected_client_with_sock(_detach_response())
        client.task.detach(client.command.exec, "cmd", monitor=False)
        from taz.c3.protocol.frame import HEADER_SIZE

        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.DetachRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.monitor is False

    def test_detach_maps_error_priority_critical(self) -> None:
        client, mock_sock = _connected_client_with_sock(_detach_response())
        client.task.detach(client.command.exec, "cmd", error_priority="critical")
        from taz.c3.protocol.frame import HEADER_SIZE

        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.DetachRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.error_priority == common_pb2.ERROR_PRIORITY_CRITICAL

    def test_detach_unknown_error_priority_raises_value_error_locally(self) -> None:
        client, mock_sock = _connected_client_with_sock(_detach_response())
        with pytest.raises(ValueError):
            client.task.detach(client.command.exec, "cmd", error_priority="urgent")
        assert mock_sock.sent == []

    def test_detach_non_exec_operation_raises_not_implemented_and_sends_nothing(
        self,
    ) -> None:
        client, mock_sock = _connected_client_with_sock(_detach_response())
        with pytest.raises(NotImplementedError):
            client.task.detach(client.file.put, "/local", "/remote")
        assert mock_sock.sent == []

    def test_detach_plain_function_raises_not_implemented(self) -> None:
        client, mock_sock = _connected_client_with_sock(_detach_response())

        def not_an_operation() -> None:
            raise AssertionError("must not be called")

        with pytest.raises(NotImplementedError):
            client.task.detach(not_an_operation)
        assert mock_sock.sent == []

    def test_detach_forwards_per_call_keepalive(self) -> None:
        client, _ = _connected_client_with_sock(_detach_response())
        # Should not raise even with an explicit per-call keepalive override.
        client.task.detach(
            client.command.exec, "cmd", keepalive=Keepalive(idle=60.0, timeout=10.0)
        )

    def test_detach_unadvertised_opcode_raises_not_supported_locally(self) -> None:
        client, _ = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING, common_pb2.OPCODE_COMMAND_EXEC]
        )
        from taz.c3.errors import TazError

        with pytest.raises(TazError) as exc_info:
            client.task.detach(client.command.exec, "cmd")
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        assert not client._conn.closed


class TestDetachPriorityWiring:
    def test_detach_registers_real_task_id_for_priority_attribution(self) -> None:
        """detach() must register the *actual* DetachResponse.task_id, not a
        placeholder, so a later PRIORITY frame on the DETACH stream attributes
        correctly."""
        client, _ = _connected_client_with_sock(
            _detach_response(task_id=55, pid=999),
            _priority_error_bytes(stream_id=1),
        )
        task = client.task.detach(
            client.command.exec, "sleep", args=["5"], error_priority="critical"
        )
        assert task.task_id == 55

        with pytest.raises(TazPriorityError) as exc_info:
            client.task.status(task.task_id)

        assert exc_info.value.task_id == 55
        assert exc_info.value.stream_id == 1

    def test_non_critical_detach_registers_nothing(self) -> None:
        """Only a CRITICAL task can escalate, so a default detach leaves no
        attribution entry behind for the life of the client."""
        client, _ = _connected_client_with_sock(_detach_response(task_id=8))
        client.task.detach(client.command.exec, "true")
        assert client._dispatcher._priority_tasks == {}

    def test_detach_error_response_registers_nothing(self) -> None:
        """A spawn failure (ERROR on the DETACH itself) creates no task, so a
        later PRIORITY frame reusing that stream_id must not misattribute."""
        client, _ = _connected_client_with_sock(
            _error_bytes(
                common_pb2.ERROR_CODE_INTERNAL,
                "spawn failed",
                opcode=common_pb2.OPCODE_DETACH,
            ),
            _priority_error_bytes(stream_id=2),
        )
        with pytest.raises(TazError):
            client.task.detach(client.command.exec, "cmd")

        with pytest.raises(TazPriorityError) as exc_info:
            client.task.status(1)

        assert exc_info.value.task_id == 0


# ---------------------------------------------------------------------------
# task.status()
# ---------------------------------------------------------------------------


class TestTaskStatus:
    def test_status_sends_task_id_and_subscribe_false(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _task_status_response_bytes(state=advanced_pb2.TASK_STATE_RUNNING)
        )
        client.task.status(99)
        from taz.c3.protocol.frame import HEADER_SIZE

        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.TaskStatusRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.task_id == 99
        assert req.subscribe is False

    def test_status_non_subscribed_maps_running_with_no_result_or_error(
        self,
    ) -> None:
        client, _ = _connected_client_with_sock(
            _task_status_response_bytes(
                state=advanced_pb2.TASK_STATE_RUNNING, pid=4321, progress=3
            )
        )
        status = client.task.status(1)
        assert status == TaskStatus(
            state=TaskState.RUNNING,
            pid=4321,
            progress=3,
            progress_total=0,
            result=None,
            error=None,
        )

    def test_status_completed_result_decodes_to_command_exec_response(self) -> None:
        client, _ = _connected_client_with_sock(
            _task_status_response_bytes(
                state=advanced_pb2.TASK_STATE_COMPLETED,
                result=_exec_response_bytes(exit_code=0, stdout=b"hi"),
            )
        )
        status = client.task.status(1)
        assert status.state == TaskState.COMPLETED
        assert status.result == CommandResult(
            exit_code=0, stdout=b"hi", stderr=b"", timed_out=False, truncated=False
        )
        assert status.error is None

    def test_status_chunked_result_merged_across_frames(self) -> None:
        full_stdout = b"x" * 5000
        result_bytes = _exec_response_bytes(exit_code=0, stdout=full_stdout)
        split = len(result_bytes) // 2
        first = advanced_pb2.TaskStatusResponse(
            state=advanced_pb2.TASK_STATE_COMPLETED,
            pid=1234,
            result=result_bytes[:split],
        )
        second = advanced_pb2.TaskStatusResponse(
            state=advanced_pb2.TASK_STATE_COMPLETED,
            pid=1234,
            result=result_bytes[split:],
        )
        client, _ = _connected_client_with_sock(
            _response_bytes(
                common_pb2.OPCODE_TASK_STATUS,
                first.SerializeToString(),
                flags=common_pb2.FRAME_FLAG_CONTINUATION,
            ),
            _response_bytes(
                common_pb2.OPCODE_TASK_STATUS,
                second.SerializeToString(),
            ),
        )
        status = client.task.status(1)
        assert status.result is not None
        assert status.result.stdout == full_stdout

    def test_status_failed_carries_error(self) -> None:
        client, _ = _connected_client_with_sock(
            _task_status_response_bytes(
                state=advanced_pb2.TASK_STATE_FAILED,
                result=_exec_response_bytes(exit_code=1),
                error=common_pb2.ErrorInfo(
                    code=common_pb2.ERROR_CODE_INTERNAL, message="exit code 1"
                ),
            )
        )
        status = client.task.status(1)
        assert status.state == TaskState.FAILED
        assert status.error is not None
        assert status.error.code == common_pb2.ERROR_CODE_INTERNAL
        assert status.error.message == "exit code 1"
        assert status.result is not None
        assert status.result.exit_code == 1

    def test_status_unknown_task_id_raises_not_found(self) -> None:
        client, _ = _connected_client_with_sock(
            _error_bytes(
                common_pb2.ERROR_CODE_NOT_FOUND,
                "unknown task",
                opcode=common_pb2.OPCODE_TASK_STATUS,
            )
        )
        with pytest.raises(TazError) as exc_info:
            client.task.status(404)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_status_subscribe_sends_subscribe_true_and_interval(self) -> None:
        client, mock_sock = _connected_client_with_sock(
            _task_status_response_bytes(
                state=advanced_pb2.TASK_STATE_COMPLETED,
                flags=0,
            )
        )
        it = client.task.status(1, subscribe=True, interval_ms=250)
        assert isinstance(it, StreamIterator)
        from taz.c3.protocol.frame import HEADER_SIZE

        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.TaskStatusRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.subscribe is True
        assert req.interval_ms == 250
        list(it)

    def test_status_subscribe_returns_updates_until_final_frame(self) -> None:
        client, _ = _connected_client_with_sock(
            _task_status_response_bytes(
                state=advanced_pb2.TASK_STATE_RUNNING,
                flags=common_pb2.FRAME_FLAG_CONTINUATION,
            ),
            _task_status_response_bytes(
                state=advanced_pb2.TASK_STATE_COMPLETED,
                flags=0,
            ),
        )
        updates = list(client.task.status(1, subscribe=True))
        assert [u.state for u in updates] == [TaskState.RUNNING, TaskState.COMPLETED]

    def test_status_forwards_per_call_keepalive(self) -> None:
        client, _ = _connected_client_with_sock(
            _task_status_response_bytes(state=advanced_pb2.TASK_STATE_RUNNING)
        )
        client.task.status(1, keepalive=Keepalive(idle=60.0, timeout=10.0))

    def test_status_unadvertised_opcode_raises_not_supported_locally(self) -> None:
        client, _ = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING, common_pb2.OPCODE_DETACH]
        )
        with pytest.raises(TazError) as exc_info:
            client.task.status(1)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        assert not client._conn.closed


# ---------------------------------------------------------------------------
# task.cancel()
# ---------------------------------------------------------------------------


class TestTaskCancel:
    def test_cancel_default_sends_signal_zero(self) -> None:
        client, mock_sock = _connected_client_with_sock(_task_cancel_response())
        client.task.cancel(99)
        from taz.c3.protocol.frame import HEADER_SIZE

        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.TaskCancelRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.task_id == 99
        assert req.signal == 0

    def test_cancel_explicit_signal_is_sent(self) -> None:
        client, mock_sock = _connected_client_with_sock(_task_cancel_response())
        client.task.cancel(1, signal=15)
        from taz.c3.protocol.frame import HEADER_SIZE

        sent = b"".join(mock_sock.sent)
        req = advanced_pb2.TaskCancelRequest()
        req.ParseFromString(sent[HEADER_SIZE:])
        assert req.signal == 15

    def test_cancel_returns_accepted_true(self) -> None:
        client, _ = _connected_client_with_sock(_task_cancel_response(accepted=True))
        assert client.task.cancel(1) is True

    def test_cancel_returns_accepted_false(self) -> None:
        client, _ = _connected_client_with_sock(_task_cancel_response(accepted=False))
        assert client.task.cancel(1) is False

    def test_cancel_unknown_task_id_raises_not_found(self) -> None:
        client, _ = _connected_client_with_sock(
            _error_bytes(
                common_pb2.ERROR_CODE_NOT_FOUND,
                "unknown task",
                opcode=common_pb2.OPCODE_TASK_CANCEL,
            )
        )
        with pytest.raises(TazError) as exc_info:
            client.task.cancel(404)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_FOUND

    def test_cancel_forwards_per_call_keepalive(self) -> None:
        client, _ = _connected_client_with_sock(_task_cancel_response())
        client.task.cancel(1, keepalive=Keepalive(idle=60.0, timeout=10.0))

    def test_cancel_unadvertised_opcode_raises_not_supported_locally(self) -> None:
        client, _ = _connected_client_with_sock(
            operations=[common_pb2.OPCODE_PING, common_pb2.OPCODE_DETACH]
        )
        with pytest.raises(TazError) as exc_info:
            client.task.cancel(1)
        assert exc_info.value.code == common_pb2.ERROR_CODE_NOT_SUPPORTED
        assert not client._conn.closed


class TestTaskPackaging:
    def test_detached_task_exported(self) -> None:
        assert "DetachedTask" in taz.c3.__all__
        assert taz.c3.DetachedTask is DetachedTask

    def test_task_status_exported(self) -> None:
        assert "TaskStatus" in taz.c3.__all__
        assert taz.c3.TaskStatus is TaskStatus

    def test_task_state_exported(self) -> None:
        assert "TaskState" in taz.c3.__all__
        assert taz.c3.TaskState is TaskState
