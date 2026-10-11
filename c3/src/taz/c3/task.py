"""TaskNamespace: the DETACH/TASK_STATUS client API (``client.task``)."""

from __future__ import annotations

import enum
from collections.abc import Callable
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, Literal, overload

from taz.c3.command import CommandResult
from taz.c3.errors import TazError
from taz.c3.settings import Keepalive
from taz.c3.stream import StreamIterator
from taz.v1 import advanced_pb2, command_pb2, common_pb2

if TYPE_CHECKING:
    # Avoid a client.py <-> task.py import cycle: TazClient constructs a
    # TaskNamespace, so task.py can only see TazClient's type, not its
    # runtime module.
    from taz.c3.client import TazClient

_ERROR_PRIORITIES: dict[str, common_pb2.ErrorPriority] = {
    "none": common_pb2.ERROR_PRIORITY_NONE,
    "critical": common_pb2.ERROR_PRIORITY_CRITICAL,
}


@dataclass(frozen=True, slots=True)
class DetachedTask:
    """Result of a ``DETACH`` call."""

    task_id: int
    pid: int


class TaskState(enum.IntEnum):
    """Lifecycle state of a detached task, mirroring ``advanced_pb2.TaskState``."""

    UNSPECIFIED = advanced_pb2.TASK_STATE_UNSPECIFIED
    PENDING = advanced_pb2.TASK_STATE_PENDING
    RUNNING = advanced_pb2.TASK_STATE_RUNNING
    COMPLETED = advanced_pb2.TASK_STATE_COMPLETED
    FAILED = advanced_pb2.TASK_STATE_FAILED
    CANCELLED = advanced_pb2.TASK_STATE_CANCELLED

    @classmethod
    def _missing_(cls, value: object) -> TaskState:
        # proto3 enums are open: a newer daemon may send a state this client
        # lacks, which should degrade to UNSPECIFIED rather than raise.
        return cls.UNSPECIFIED


@dataclass(frozen=True, slots=True)
class TaskStatus:
    """Result of a ``client.task.status()`` call, or one frame of a subscription."""

    state: TaskState
    pid: int
    progress: int
    progress_total: int
    result: CommandResult | None
    error: TazError | None


def _parse_command_result(data: bytes) -> CommandResult:
    resp = command_pb2.CommandExecResponse()
    resp.ParseFromString(data)
    return CommandResult(
        exit_code=resp.exit_code,
        stdout=resp.stdout_data,
        stderr=resp.stderr_data,
        timed_out=resp.timed_out,
        truncated=resp.truncated,
    )


_TERMINAL_RESULT_STATES = frozenset((TaskState.COMPLETED, TaskState.FAILED))


def _task_status_from_response(
    resp: advanced_pb2.TaskStatusResponse,
    *,
    attach_result: bool,
) -> TaskStatus:
    state = TaskState(resp.state)
    # `result` is a plain (presence-less) proto3 `bytes` field: an
    # all-default underlying CommandExecResponse (e.g. exit 0, no output)
    # serializes to zero bytes, indistinguishable on the wire from
    # "absent". A streamed frame (``attach_result=False``) never carries a
    # result even on its terminal frame, so this can't be decided from the
    # bytes alone - the caller already knows, from subscribe vs not, which
    # contract applies.
    return TaskStatus(
        state=state,
        pid=resp.pid,
        progress=resp.progress,
        progress_total=resp.progress_total,
        result=(
            _parse_command_result(resp.result)
            if attach_result and state in _TERMINAL_RESULT_STATES
            else None
        ),
        error=(
            TazError(resp.error.code, resp.error.message, resp.error.detail)
            if resp.HasField("error")
            else None
        ),
    )


def _parse_task_status(payload: bytes) -> TaskStatus:
    resp = advanced_pb2.TaskStatusResponse()
    resp.ParseFromString(payload)
    return _task_status_from_response(resp, attach_result=False)


def _error_priority(value: str) -> common_pb2.ErrorPriority:
    try:
        return _ERROR_PRIORITIES[value]
    except KeyError:
        raise ValueError(
            f"unknown error_priority {value!r}; expected one of "
            f"{sorted(_ERROR_PRIORITIES)}"
        ) from None


class TaskNamespace:
    """Namespace for detached-task operations, exposed as ``client.task``."""

    def __init__(self, client: TazClient) -> None:
        self._client = client

    def detach(
        self,
        operation: Callable[..., Any],
        *args: Any,
        error_priority: str = "none",
        monitor: bool = True,
        keepalive: Keepalive | None = None,
        **kwargs: Any,
    ) -> DetachedTask:
        """Start ``operation`` on the daemon detached from this call.

        Returns a ``DetachedTask`` as soon as the daemon has spawned it,
        instead of waiting for it to finish. Only ``client.command.exec`` is
        accepted as ``operation``; anything else raises
        ``NotImplementedError`` locally, without sending anything. ``args``
        and ``kwargs`` are ``operation``'s own arguments (its ``keepalive``,
        if any, is ignored here; this call's own ``keepalive`` governs the
        DETACH request instead). ``monitor=True`` (the default) keeps the
        task pollable with ``client.task.status()`` after it finishes.
        ``error_priority`` is ``"none"`` (the default) or ``"critical"``.
        """
        client = self._client
        if operation != client.command.exec:
            raise NotImplementedError(
                "client.task.detach() only supports client.command.exec"
            )
        kwargs.pop("keepalive", None)
        inner = client.command.build_exec_request(*args, **kwargs)
        priority = _error_priority(error_priority)
        req = advanced_pb2.DetachRequest(monitor=monitor, error_priority=priority)
        req.exec.CopyFrom(inner)
        stream_id = 0

        def _capture_stream_id(sid: int) -> None:
            nonlocal stream_id
            stream_id = sid

        resp = client._call(
            common_pb2.OPCODE_DETACH,
            req.SerializeToString(),
            advanced_pb2.DetachResponse,
            keepalive,
            on_stream_id=_capture_stream_id,
        )
        # Only a CRITICAL task can escalate, so only it needs attributing.
        if priority == common_pb2.ERROR_PRIORITY_CRITICAL:
            client._dispatcher.register_priority_task(stream_id, resp.task_id)
        return DetachedTask(task_id=resp.task_id, pid=resp.pid)

    @overload
    def status(
        self,
        task_id: int,
        subscribe: Literal[False] = False,
        interval_ms: int = 1000,
        *,
        overflow: Literal["cancel", "drop_oldest"] = "cancel",
        max_wait: float | None = None,
        keepalive: Keepalive | None = None,
    ) -> TaskStatus: ...

    @overload
    def status(
        self,
        task_id: int,
        subscribe: Literal[True],
        interval_ms: int = 1000,
        *,
        overflow: Literal["cancel", "drop_oldest"] = "cancel",
        max_wait: float | None = None,
        keepalive: Keepalive | None = None,
    ) -> StreamIterator[TaskStatus]: ...

    def status(
        self,
        task_id: int,
        subscribe: bool = False,
        interval_ms: int = 1000,
        *,
        overflow: Literal["cancel", "drop_oldest"] = "cancel",
        max_wait: float | None = None,
        keepalive: Keepalive | None = None,
    ) -> TaskStatus | StreamIterator[TaskStatus]:
        """Return (or stream) a detached task's status.

        Non-subscribed (the default), sends one ``TASK_STATUS`` request and
        returns a single ``TaskStatus``; a chunked ``result`` (COMPLETED or
        FAILED) is merged first. With ``subscribe=True``, returns a
        ``StreamIterator`` of ``TaskStatus`` updates (``interval_ms`` apart,
        clamped to the daemon's own floor). No streamed update carries a
        ``result``, not even the final one, which ends the stream with the
        task's terminal state (poll once more for the result). An unknown
        ``task_id`` surfaces as ``TazError(NOT_FOUND)``, from this call
        directly or from the stream's first iteration.
        """
        client = self._client
        if subscribe:
            kv = keepalive if keepalive is not None else client._keepalive
            req = advanced_pb2.TaskStatusRequest(
                task_id=task_id, subscribe=True, interval_ms=interval_ms
            )
            stream_id = client._conn.send_request(
                common_pb2.OPCODE_TASK_STATUS, req.SerializeToString()
            )
            return StreamIterator(
                client,
                stream_id,
                common_pb2.OPCODE_TASK_STATUS,
                _parse_task_status,
                keepalive=kv,
                max_wait=max_wait,
                overflow=overflow,
            )
        req = advanced_pb2.TaskStatusRequest(task_id=task_id, subscribe=False)
        resp = client._call(
            common_pb2.OPCODE_TASK_STATUS,
            req.SerializeToString(),
            advanced_pb2.TaskStatusResponse,
            keepalive,
        )
        return _task_status_from_response(resp, attach_result=True)

    def cancel(
        self,
        task_id: int,
        signal: int | None = None,
        *,
        keepalive: Keepalive | None = None,
    ) -> bool:
        """Cancel a detached task.

        ``signal=None`` (the default) sends ``signal=0``, killing the task's
        whole tree. A nonzero ``signal`` is sent to the task's process group
        instead (POSIX; Windows ignores the value and terminates the job
        unconditionally). Returns whether the daemon accepted the request
        (``False`` if the task had already finished); an unknown ``task_id``
        raises ``TazError(NOT_FOUND)``.
        """
        client = self._client
        req = advanced_pb2.TaskCancelRequest(
            task_id=task_id, signal=signal if signal is not None else 0
        )
        resp = client._call(
            common_pb2.OPCODE_TASK_CANCEL,
            req.SerializeToString(),
            advanced_pb2.TaskCancelResponse,
            keepalive,
        )
        return resp.accepted
