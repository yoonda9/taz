"""DirectoryNamespace: the DIR_MAKE/LIST/REMOVE client API (``client.directory``)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

from taz.c3.file import Kind
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, file_pb2

if TYPE_CHECKING:
    # Avoid a client.py <-> directory.py import cycle: TazClient constructs a
    # DirectoryNamespace, so directory.py can only see TazClient's type, not
    # its runtime module.
    from taz.c3.client import TazClient


@dataclass(frozen=True, slots=True)
class DirEntry:
    """One entry of a ``DIR_LIST`` call."""

    name: str
    kind: Kind
    size: int


class DirectoryNamespace:
    """Namespace for directory operations, exposed as ``client.directory``."""

    def __init__(self, client: TazClient) -> None:
        self._client = client

    def make(
        self,
        path: str,
        permissions: int = 0,
        parents: bool = False,
        keepalive: Keepalive | None = None,
    ) -> None:
        """Create the directory at ``path``.

        ``permissions`` of 0 falls back to the daemon's default (``0755``).
        ``parents=True`` creates missing parent directories (``mkdir -p``).
        """
        req = file_pb2.DirMakeRequest(
            path=path, permissions=permissions, parents=parents
        )
        self._client._call(
            common_pb2.OPCODE_DIR_MAKE,
            req.SerializeToString(),
            file_pb2.DirMakeResponse,
            keepalive,
        )

    def list(
        self,
        path: str,
        include_hidden: bool = False,
        keepalive: Keepalive | None = None,
    ) -> list[DirEntry]:
        """Return the entries of the directory at ``path``.

        ``include_hidden=False`` omits dot-files.
        """
        req = file_pb2.DirListRequest(path=path, include_hidden=include_hidden)
        resp = self._client._call(
            common_pb2.OPCODE_DIR_LIST,
            req.SerializeToString(),
            file_pb2.DirListResponse,
            keepalive,
        )
        return [
            DirEntry(name=entry.name, kind=Kind(entry.kind), size=entry.size)
            for entry in resp.entries
        ]

    def remove(
        self,
        path: str,
        recursive: bool = False,
        keepalive: Keepalive | None = None,
    ) -> None:
        """Remove the directory at ``path``.

        ``recursive=True`` removes a non-empty directory tree.
        """
        req = file_pb2.DirRemoveRequest(path=path, recursive=recursive)
        self._client._call(
            common_pb2.OPCODE_DIR_REMOVE,
            req.SerializeToString(),
            file_pb2.DirRemoveResponse,
            keepalive,
        )
