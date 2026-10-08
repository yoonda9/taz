"""FileNamespace: the FILE_CREATE/DELETE/STAT/CHMOD/PUT client API (``client.file``)."""

from __future__ import annotations

import enum
import os
from dataclasses import dataclass
from typing import TYPE_CHECKING

import crc32c

from taz.c3.errors import TazChecksumError, TazError, TazProtocolError
from taz.c3.protocol.frame import DEFAULT_MAX_PAYLOAD, Frame
from taz.c3.settings import Keepalive
from taz.v1 import common_pb2, file_pb2

if TYPE_CHECKING:
    # Avoid a client.py <-> file.py import cycle: TazClient constructs a
    # FileNamespace, so file.py can only see TazClient's type, not its
    # runtime module.
    from taz.c3.client import TazClient

_REQUEST_LIMIT = DEFAULT_MAX_PAYLOAD[common_pb2.FRAME_TYPE_REQUEST]


def _raise_if_error(frame: Frame) -> None:
    if frame.type != common_pb2.FRAME_TYPE_ERROR:
        return
    info = common_pb2.ErrorInfo()
    info.ParseFromString(frame.payload)
    raise TazError(info.code, info.message, info.detail)


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

    def put(
        self,
        local_path: str,
        remote_path: str,
        overwrite: bool = False,
        permissions: int = 0o644,
        keepalive: Keepalive | None = None,
    ) -> FileTransfer:
        """Upload ``local_path`` to ``remote_path`` in FILE_CHUNK-sized pieces.

        Chunk size is the daemon's advertised FILE_CHUNK limit, read once for
        this call (not a hard-coded size); if the daemon advertises 0, raises
        ``TazError(NOT_SUPPORTED)`` before sending anything. Raises
        ``TazChecksumError`` if the daemon's reported checksum does not match
        the data as sent.
        """
        client = self._client
        kv = keepalive if keepalive is not None else client._keepalive
        chunk_limit = client._conn.limits[common_pb2.FRAME_TYPE_FILE_CHUNK]
        if chunk_limit == 0:
            raise TazError(
                common_pb2.ERROR_CODE_NOT_SUPPORTED,
                "daemon accepts no FILE_CHUNK payload",
            )
        size = os.stat(local_path).st_size

        req = file_pb2.FilePutRequest(
            dest=remote_path,
            size=size,
            permissions=permissions,
            overwrite=overwrite,
        )
        stream_id = client._conn.send_request(
            common_pb2.OPCODE_FILE_PUT, req.SerializeToString()
        )

        frame = client._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_PUT
        )
        _raise_if_error(frame)
        ack_resp = file_pb2.FilePutResponse()
        ack_resp.ParseFromString(frame.payload)
        if ack_resp.WhichOneof("phase") != "ack" or not ack_resp.ack.ready:
            raise TazProtocolError("FILE_PUT: expected Ack{ready=true}")

        crc = 0
        sent = 0
        try:
            with open(local_path, "rb") as f:
                while True:
                    data = f.read(chunk_limit)
                    sent += len(data)
                    last = sent >= size
                    if len(data) < chunk_limit and not last:
                        # A short read below the requested chunk size is
                        # real EOF, not a scheduling artifact - regular
                        # files only return less than requested at EOF. If
                        # that happens before our pre-transfer os.stat()
                        # size is reached, the file shrank underneath us;
                        # looping again would just resend empty chunks
                        # forever since `sent` can never reach `size`.
                        raise OSError(
                            f"local file shrank during upload: read {sent}"
                            f" of {size} declared bytes"
                        )
                    crc = crc32c.crc32c(data, crc)
                    client._conn.send_file_chunk(stream_id, data, last=last)
                    if last:
                        break
        except OSError:
            if client._dispatcher._cancel_advertised():
                client.cancel(stream_id, keepalive=kv)
            raise

        frame = client._dispatcher.recv_response(
            stream_id, kv, expected_opcode=common_pb2.OPCODE_FILE_PUT
        )
        _raise_if_error(frame)
        confirm_resp = file_pb2.FilePutResponse()
        confirm_resp.ParseFromString(frame.payload)
        if confirm_resp.WhichOneof("phase") != "confirm":
            raise TazProtocolError("FILE_PUT: expected Confirmation")
        if confirm_resp.confirm.bytes_written != size:
            raise TazProtocolError(
                f"FILE_PUT: daemon wrote {confirm_resp.confirm.bytes_written}"
                f" bytes, announced {size}"
            )
        checksum = crc.to_bytes(4, "little")
        if confirm_resp.confirm.checksum != checksum:
            raise TazChecksumError(checksum, confirm_resp.confirm.checksum)
        return FileTransfer(size=size, checksum=checksum)
