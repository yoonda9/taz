"""ProcessNamespace: the PROCESS_LIST/KILL/INFO/MONITOR client API
(``client.process``).
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

from taz.c3.settings import Keepalive
from taz.c3.stream import StreamIterator
from taz.v1 import common_pb2, process_pb2

if TYPE_CHECKING:
    # Avoid a client.py <-> process.py import cycle: TazClient constructs a
    # ProcessNamespace, so process.py can only see TazClient's type, not its
    # runtime module.
    from taz.c3.client import TazClient


@dataclass(frozen=True, slots=True)
class ProcessInfo:
    """One entry of a ``PROCESS_LIST`` call."""

    pid: int
    name: str
    user: str
    cpu_percent: float
    memory_bytes: int
    state: str


@dataclass(frozen=True, slots=True)
class ProcessDetail(ProcessInfo):
    """The result of a ``PROCESS_INFO`` call."""

    command_line: str
    start_time: int
    open_files: list[str]


@dataclass(frozen=True, slots=True)
class ProcessMonitorUpdate:
    """One frame of a ``client.process.monitor()`` stream."""

    info: ProcessInfo
    exited: bool
    exit_code: int
    exit_code_known: bool
    reason: str


def _parse_monitor_update(payload: bytes) -> ProcessMonitorUpdate:
    resp = process_pb2.ProcessMonitorResponse()
    resp.ParseFromString(payload)
    info = resp.info
    return ProcessMonitorUpdate(
        info=ProcessInfo(
            pid=info.pid,
            name=info.name,
            user=info.user,
            cpu_percent=info.cpu_percent,
            memory_bytes=info.memory_bytes,
            state=info.state,
        ),
        exited=resp.exited,
        exit_code=resp.exit_code,
        exit_code_known=resp.exit_code_known,
        reason=resp.reason,
    )


class ProcessNamespace:
    """Namespace for process operations, exposed as ``client.process``."""

    def __init__(self, client: TazClient) -> None:
        self._client = client

    def list(
        self,
        filter: str | None = None,  # noqa: A002
        keepalive: Keepalive | None = None,
    ) -> list[ProcessInfo]:
        """Return the processes on the host, optionally filtered by name.

        ``filter`` is a case-sensitive substring match on the process name;
        ``None`` (the default) lists every process.
        """
        req = process_pb2.ProcessListRequest(
            filter=filter if filter is not None else ""
        )
        resp = self._client._call(
            common_pb2.OPCODE_PROCESS_LIST,
            req.SerializeToString(),
            process_pb2.ProcessListResponse,
            keepalive,
        )
        return [
            ProcessInfo(
                pid=entry.pid,
                name=entry.name,
                user=entry.user,
                cpu_percent=entry.cpu_percent,
                memory_bytes=entry.memory_bytes,
                state=entry.state,
            )
            for entry in resp.processes
        ]

    def kill(
        self,
        pid: int,
        signal: int | None = None,
        keepalive: Keepalive | None = None,
    ) -> bool:
        """Send a signal to ``pid``; ``signal`` 0 (the default) is the
        platform's SIGTERM equivalent.
        """
        req = process_pb2.ProcessKillRequest(
            pid=pid, signal=signal if signal is not None else 0
        )
        resp = self._client._call(
            common_pb2.OPCODE_PROCESS_KILL,
            req.SerializeToString(),
            process_pb2.ProcessKillResponse,
            keepalive,
        )
        return resp.success

    def info(self, pid: int, keepalive: Keepalive | None = None) -> ProcessDetail:
        """Return detailed information about ``pid``."""
        req = process_pb2.ProcessInfoRequest(pid=pid)
        resp = self._client._call(
            common_pb2.OPCODE_PROCESS_INFO,
            req.SerializeToString(),
            process_pb2.ProcessInfoResponse,
            keepalive,
        )
        info = resp.info
        return ProcessDetail(
            pid=info.pid,
            name=info.name,
            user=info.user,
            cpu_percent=info.cpu_percent,
            memory_bytes=info.memory_bytes,
            state=info.state,
            command_line=resp.command_line,
            start_time=resp.start_time,
            open_files=list(resp.open_files),
        )

    def monitor(
        self,
        pid: int,
        interval_ms: int = 1000,
        *,
        overflow: Literal["cancel", "drop_oldest"] = "cancel",
        max_wait: float | None = None,
        keepalive: Keepalive | None = None,
    ) -> StreamIterator[ProcessMonitorUpdate]:
        """Stream periodic ``ProcessMonitorUpdate``s for ``pid``.

        Returns the stream without reading anything: a nonexistent ``pid``
        (or any other request-time failure) surfaces as a ``TazError`` from
        the first iteration, also inside a ``with`` block. Raises
        ``TazError(NOT_SUPPORTED)`` without sending anything if the daemon
        does not advertise PROCESS_MONITOR, and ``ValueError`` without
        sending anything for a negative ``pid``.
        """
        client = self._client
        kv = keepalive if keepalive is not None else client._keepalive
        req = process_pb2.ProcessMonitorRequest(pid=pid, interval_ms=interval_ms)
        stream_id = client._conn.send_request(
            common_pb2.OPCODE_PROCESS_MONITOR, req.SerializeToString()
        )
        return StreamIterator(
            client,
            stream_id,
            common_pb2.OPCODE_PROCESS_MONITOR,
            _parse_monitor_update,
            keepalive=kv,
            max_wait=max_wait,
            overflow=overflow,
        )
