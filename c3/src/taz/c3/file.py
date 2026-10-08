"""FileNamespace: the FILE_CREATE/DELETE/STAT/CHMOD client API (``client.file``)."""

from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import TYPE_CHECKING

from taz.c3.protocol.frame import DEFAULT_MAX_PAYLOAD
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, file_pb2

if TYPE_CHECKING:
    # Avoid a client.py <-> file.py import cycle: TazClient constructs a
    # FileNamespace, so file.py can only see TazClient's type, not its
    # runtime module.
    from taz.c3.client import TazClient

_REQUEST_LIMIT = DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST]


class Kind(enum.IntEnum):
    """Filesystem entry kind, mirroring ``taz.v1.common_pb2.Kind``."""

    UNSPECIFIED = common_pb2.KIND_UNSPECIFIED
    FILE = common_pb2.KIND_FILE
    DIR = common_pb2.KIND_DIR
    SYMLINK = common_pb2.KIND_SYMLINK
    OTHER = common_pb2.KIND_OTHER

    @classmethod
    def _missing_(cls, value: object) -> Kind:
        # proto3 enums are open: a newer daemon may send a kind this client
        # lacks, which should degrade to OTHER rather than raise.
        return cls.OTHER


@dataclass(frozen=True, slots=True)
class FileTransfer:
    """Result of a ``file.put``/``file.get`` chunked transfer."""

    size: int
    checksum: bytes


@dataclass(frozen=True, slots=True)
class FileStat:
    """Result of a ``FILE_STAT`` call."""

    size: int
    permissions: int
    owner: str
    modified: int
    created: int
    kind: Kind
    link_target: str


class FileNamespace:
    """Namespace for file metadata operations, exposed as ``client.file``."""

    def __init__(self, client: TazClient) -> None:
        self._client = client

    def create(
        self,
        path: str,
        content: bytes = b"",
        permissions: int = 0,
        keepalive: Keepalive | None = None,
    ) -> None:
        """Create ``path`` with optional inline ``content``.

        ``content`` must fit in a single REQUEST frame; larger payloads raise
        ``ValueError`` locally (no round trip) and point callers at
        ``file.put`` for chunked transfer. ``permissions`` of 0 falls back to
        the daemon's default (``0644``).
        """
        req = file_pb2.FileCreateRequest(
            path=path, content=content, permissions=permissions
        )
        payload = req.SerializeToString()
        if len(payload) > _REQUEST_LIMIT:
            raise ValueError(
                f"content of {len(content)} bytes exceeds the REQUEST "
                f"payload limit ({_REQUEST_LIMIT}); use file.put"
            )
        self._client._call(
            common_pb2.OPCODE_FILE_CREATE,
            payload,
            file_pb2.FileCreateResponse,
            keepalive,
        )

    def delete(self, path: str, keepalive: Keepalive | None = None) -> None:
        """Delete the file at ``path``. Never follows or removes symlinks' targets."""
        req = file_pb2.FileDeleteRequest(path=path)
        self._client._call(
            common_pb2.OPCODE_FILE_DELETE,
            req.SerializeToString(),
            file_pb2.FileDeleteResponse,
            keepalive,
        )

    def stat(self, path: str, keepalive: Keepalive | None = None) -> FileStat:
        """Return metadata about ``path`` (never follows a symlink at ``path``)."""
        req = file_pb2.FileStatRequest(path=path)
        resp = self._client._call(
            common_pb2.OPCODE_FILE_STAT,
            req.SerializeToString(),
            file_pb2.FileStatResponse,
            keepalive,
        )
        return FileStat(
            size=resp.size,
            permissions=resp.permissions,
            owner=resp.owner,
            modified=resp.modified,
            created=resp.created,
            kind=Kind(resp.kind),
            link_target=resp.link_target,
        )

    def chmod(
        self,
        path: str,
        permissions: int,
        keepalive: Keepalive | None = None,
    ) -> None:
        """Change ``path``'s POSIX mode bits to ``permissions``."""
        req = file_pb2.FileChmodRequest(path=path, permissions=permissions)
        self._client._call(
            common_pb2.OPCODE_FILE_CHMOD,
            req.SerializeToString(),
            file_pb2.FileChmodResponse,
            keepalive,
        )
