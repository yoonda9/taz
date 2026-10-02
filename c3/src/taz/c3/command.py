"""CommandNamespace: the COMMAND_EXEC client API (``client.command.exec(...)``)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

from taz.c3.settings import Keepalive
from taz.v1 import command_pb2, common_pb2

if TYPE_CHECKING:
    # Avoid a client.py <-> command.py import cycle: TazClient constructs a
    # CommandNamespace, so command.py can only see TazClient's type, not its
    # runtime module.
    from taz.c3.client import TazClient


@dataclass(frozen=True, slots=True)
class CommandResult:
    """Result of a ``COMMAND_EXEC`` call."""

    exit_code: int
    stdout: bytes
    stderr: bytes
    timed_out: bool
    truncated: bool


class CommandNamespace:
    """Namespace for command-execution operations, exposed as ``client.command``."""

    def __init__(self, client: TazClient) -> None:
        self._client = client

    def exec(
        self,
        command: str,
        args: list[str] | None = None,
        env: dict[str, str] | None = None,
        working_dir: str | None = None,
        timeout_ms: int = 0,
        keepalive: Keepalive | None = None,
    ) -> CommandResult:
        """Run ``command`` on the daemon and wait for it to finish.

        ``timeout_ms`` of 0 falls back to the connection's default timeout.
        ``env`` entries are merged into (not a replacement for) the daemon's
        own environment.
        """
        req = command_pb2.CommandExecRequest(command=command, timeout_ms=timeout_ms)
        if args:
            req.args.extend(args)
        if env:
            for key, value in env.items():
                kv = req.env.add()
                kv.key = key
                kv.value = value
        if working_dir:
            req.working_dir = working_dir
        resp = self._client._call(
            common_pb2.OPCODE_COMMAND_EXEC,
            req.SerializeToString(),
            command_pb2.CommandExecResponse,
            keepalive,
        )
        return CommandResult(
            exit_code=resp.exit_code,
            stdout=resp.stdout_data,
            stderr=resp.stderr_data,
            timed_out=resp.timed_out,
            truncated=resp.truncated,
        )
