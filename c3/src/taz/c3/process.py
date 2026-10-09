"""ProcessNamespace: the PROCESS_LIST/KILL/INFO client API (``client.process``)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

from taz.c3.settings import Keepalive
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
