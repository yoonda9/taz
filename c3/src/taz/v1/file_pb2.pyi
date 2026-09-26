from taz.v1 import common_pb2 as _common_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class FilePutRequest(_message.Message):
    __slots__ = ("dest", "size", "permissions", "overwrite")
    DEST_FIELD_NUMBER: _ClassVar[int]
    SIZE_FIELD_NUMBER: _ClassVar[int]
    PERMISSIONS_FIELD_NUMBER: _ClassVar[int]
    OVERWRITE_FIELD_NUMBER: _ClassVar[int]
    dest: str
    size: int
    permissions: int
    overwrite: bool
    def __init__(self, dest: _Optional[str] = ..., size: _Optional[int] = ..., permissions: _Optional[int] = ..., overwrite: _Optional[bool] = ...) -> None: ...

class FilePutResponse(_message.Message):
    __slots__ = ("ack", "confirm")
    class Ack(_message.Message):
        __slots__ = ("ready",)
        READY_FIELD_NUMBER: _ClassVar[int]
        ready: bool
        def __init__(self, ready: _Optional[bool] = ...) -> None: ...
    class Confirmation(_message.Message):
        __slots__ = ("bytes_written", "checksum")
        BYTES_WRITTEN_FIELD_NUMBER: _ClassVar[int]
        CHECKSUM_FIELD_NUMBER: _ClassVar[int]
        bytes_written: int
        checksum: bytes
        def __init__(self, bytes_written: _Optional[int] = ..., checksum: _Optional[bytes] = ...) -> None: ...
    ACK_FIELD_NUMBER: _ClassVar[int]
    CONFIRM_FIELD_NUMBER: _ClassVar[int]
    ack: FilePutResponse.Ack
    confirm: FilePutResponse.Confirmation
    def __init__(self, ack: _Optional[_Union[FilePutResponse.Ack, _Mapping]] = ..., confirm: _Optional[_Union[FilePutResponse.Confirmation, _Mapping]] = ...) -> None: ...

class FileGetRequest(_message.Message):
    __slots__ = ("src",)
    SRC_FIELD_NUMBER: _ClassVar[int]
    src: str
    def __init__(self, src: _Optional[str] = ...) -> None: ...

class FileGetResponse(_message.Message):
    __slots__ = ("size", "permissions", "checksum")
    SIZE_FIELD_NUMBER: _ClassVar[int]
    PERMISSIONS_FIELD_NUMBER: _ClassVar[int]
    CHECKSUM_FIELD_NUMBER: _ClassVar[int]
    size: int
    permissions: int
    checksum: bytes
    def __init__(self, size: _Optional[int] = ..., permissions: _Optional[int] = ..., checksum: _Optional[bytes] = ...) -> None: ...

class FileCreateRequest(_message.Message):
    __slots__ = ("path", "content", "permissions")
    PATH_FIELD_NUMBER: _ClassVar[int]
    CONTENT_FIELD_NUMBER: _ClassVar[int]
    PERMISSIONS_FIELD_NUMBER: _ClassVar[int]
    path: str
    content: bytes
    permissions: int
    def __init__(self, path: _Optional[str] = ..., content: _Optional[bytes] = ..., permissions: _Optional[int] = ...) -> None: ...

class FileCreateResponse(_message.Message):
    __slots__ = ("success",)
    SUCCESS_FIELD_NUMBER: _ClassVar[int]
    success: bool
    def __init__(self, success: _Optional[bool] = ...) -> None: ...

class FileDeleteRequest(_message.Message):
    __slots__ = ("path",)
    PATH_FIELD_NUMBER: _ClassVar[int]
    path: str
    def __init__(self, path: _Optional[str] = ...) -> None: ...

class FileDeleteResponse(_message.Message):
    __slots__ = ("success",)
    SUCCESS_FIELD_NUMBER: _ClassVar[int]
    success: bool
    def __init__(self, success: _Optional[bool] = ...) -> None: ...

class FileStatRequest(_message.Message):
    __slots__ = ("path",)
    PATH_FIELD_NUMBER: _ClassVar[int]
    path: str
    def __init__(self, path: _Optional[str] = ...) -> None: ...

class FileStatResponse(_message.Message):
    __slots__ = ("size", "permissions", "owner", "modified", "created", "kind", "link_target")
    SIZE_FIELD_NUMBER: _ClassVar[int]
    PERMISSIONS_FIELD_NUMBER: _ClassVar[int]
    OWNER_FIELD_NUMBER: _ClassVar[int]
    MODIFIED_FIELD_NUMBER: _ClassVar[int]
    CREATED_FIELD_NUMBER: _ClassVar[int]
    KIND_FIELD_NUMBER: _ClassVar[int]
    LINK_TARGET_FIELD_NUMBER: _ClassVar[int]
    size: int
    permissions: int
    owner: str
    modified: int
    created: int
    kind: _common_pb2.Kind
    link_target: str
    def __init__(self, size: _Optional[int] = ..., permissions: _Optional[int] = ..., owner: _Optional[str] = ..., modified: _Optional[int] = ..., created: _Optional[int] = ..., kind: _Optional[_Union[_common_pb2.Kind, str]] = ..., link_target: _Optional[str] = ...) -> None: ...

class FileChmodRequest(_message.Message):
    __slots__ = ("path", "permissions")
    PATH_FIELD_NUMBER: _ClassVar[int]
    PERMISSIONS_FIELD_NUMBER: _ClassVar[int]
    path: str
    permissions: int
    def __init__(self, path: _Optional[str] = ..., permissions: _Optional[int] = ...) -> None: ...

class FileChmodResponse(_message.Message):
    __slots__ = ("success",)
    SUCCESS_FIELD_NUMBER: _ClassVar[int]
    success: bool
    def __init__(self, success: _Optional[bool] = ...) -> None: ...

class DirMakeRequest(_message.Message):
    __slots__ = ("path", "permissions", "parents")
    PATH_FIELD_NUMBER: _ClassVar[int]
    PERMISSIONS_FIELD_NUMBER: _ClassVar[int]
    PARENTS_FIELD_NUMBER: _ClassVar[int]
    path: str
    permissions: int
    parents: bool
    def __init__(self, path: _Optional[str] = ..., permissions: _Optional[int] = ..., parents: _Optional[bool] = ...) -> None: ...

class DirMakeResponse(_message.Message):
    __slots__ = ("success",)
    SUCCESS_FIELD_NUMBER: _ClassVar[int]
    success: bool
    def __init__(self, success: _Optional[bool] = ...) -> None: ...

class DirListRequest(_message.Message):
    __slots__ = ("path", "include_hidden")
    PATH_FIELD_NUMBER: _ClassVar[int]
    INCLUDE_HIDDEN_FIELD_NUMBER: _ClassVar[int]
    path: str
    include_hidden: bool
    def __init__(self, path: _Optional[str] = ..., include_hidden: _Optional[bool] = ...) -> None: ...

class DirEntry(_message.Message):
    __slots__ = ("name", "kind", "size")
    NAME_FIELD_NUMBER: _ClassVar[int]
    KIND_FIELD_NUMBER: _ClassVar[int]
    SIZE_FIELD_NUMBER: _ClassVar[int]
    name: str
    kind: _common_pb2.Kind
    size: int
    def __init__(self, name: _Optional[str] = ..., kind: _Optional[_Union[_common_pb2.Kind, str]] = ..., size: _Optional[int] = ...) -> None: ...

class DirListResponse(_message.Message):
    __slots__ = ("entries",)
    ENTRIES_FIELD_NUMBER: _ClassVar[int]
    entries: _containers.RepeatedCompositeFieldContainer[DirEntry]
    def __init__(self, entries: _Optional[_Iterable[_Union[DirEntry, _Mapping]]] = ...) -> None: ...

class DirRemoveRequest(_message.Message):
    __slots__ = ("path", "recursive")
    PATH_FIELD_NUMBER: _ClassVar[int]
    RECURSIVE_FIELD_NUMBER: _ClassVar[int]
    path: str
    recursive: bool
    def __init__(self, path: _Optional[str] = ..., recursive: _Optional[bool] = ...) -> None: ...

class DirRemoveResponse(_message.Message):
    __slots__ = ("success",)
    SUCCESS_FIELD_NUMBER: _ClassVar[int]
    success: bool
    def __init__(self, success: _Optional[bool] = ...) -> None: ...
