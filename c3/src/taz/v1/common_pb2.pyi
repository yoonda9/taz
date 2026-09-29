from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class Opcode(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    OPCODE_UNSPECIFIED: _ClassVar[Opcode]
    OPCODE_PING: _ClassVar[Opcode]
    OPCODE_VERSION: _ClassVar[Opcode]
    OPCODE_CONFIGURATION_GET: _ClassVar[Opcode]
    OPCODE_CONFIGURATION_UPDATE: _ClassVar[Opcode]
    OPCODE_RESTART: _ClassVar[Opcode]
    OPCODE_UPDATE: _ClassVar[Opcode]
    OPCODE_COMMAND_EXEC: _ClassVar[Opcode]
    OPCODE_SHELL_OPEN: _ClassVar[Opcode]
    OPCODE_SHELL_INPUT: _ClassVar[Opcode]
    OPCODE_SHELL_CLOSE: _ClassVar[Opcode]
    OPCODE_PROCESS_LIST: _ClassVar[Opcode]
    OPCODE_PROCESS_KILL: _ClassVar[Opcode]
    OPCODE_PROCESS_INFO: _ClassVar[Opcode]
    OPCODE_PROCESS_MONITOR: _ClassVar[Opcode]
    OPCODE_FILE_PUT: _ClassVar[Opcode]
    OPCODE_FILE_GET: _ClassVar[Opcode]
    OPCODE_FILE_CREATE: _ClassVar[Opcode]
    OPCODE_FILE_DELETE: _ClassVar[Opcode]
    OPCODE_FILE_STAT: _ClassVar[Opcode]
    OPCODE_FILE_CHMOD: _ClassVar[Opcode]
    OPCODE_DIR_MAKE: _ClassVar[Opcode]
    OPCODE_DIR_LIST: _ClassVar[Opcode]
    OPCODE_DIR_REMOVE: _ClassVar[Opcode]
    OPCODE_RUN_AS: _ClassVar[Opcode]
    OPCODE_TIMEOUT_SET: _ClassVar[Opcode]
    OPCODE_LOG: _ClassVar[Opcode]
    OPCODE_DETACH: _ClassVar[Opcode]
    OPCODE_TASK_STATUS: _ClassVar[Opcode]
    OPCODE_TASK_CANCEL: _ClassVar[Opcode]
    OPCODE_CANCEL: _ClassVar[Opcode]
    OPCODE_PIPELINE: _ClassVar[Opcode]

class FrameType(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    FRAME_TYPE_UNSPECIFIED: _ClassVar[FrameType]
    FRAME_TYPE_REQUEST: _ClassVar[FrameType]
    FRAME_TYPE_RESPONSE: _ClassVar[FrameType]
    FRAME_TYPE_FILE_CHUNK: _ClassVar[FrameType]
    FRAME_TYPE_ERROR: _ClassVar[FrameType]
    FRAME_TYPE_PING: _ClassVar[FrameType]
    FRAME_TYPE_PONG: _ClassVar[FrameType]
    FRAME_TYPE_CAPABILITY: _ClassVar[FrameType]

class FrameFlag(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    FRAME_FLAG_NONE: _ClassVar[FrameFlag]
    FRAME_FLAG_CONTINUATION: _ClassVar[FrameFlag]
    FRAME_FLAG_COMPRESSED: _ClassVar[FrameFlag]
    FRAME_FLAG_PRIORITY: _ClassVar[FrameFlag]

class ErrorCode(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    ERROR_CODE_UNKNOWN: _ClassVar[ErrorCode]
    ERROR_CODE_NOT_FOUND: _ClassVar[ErrorCode]
    ERROR_CODE_PERMISSION_DENIED: _ClassVar[ErrorCode]
    ERROR_CODE_ALREADY_EXISTS: _ClassVar[ErrorCode]
    ERROR_CODE_TIMEOUT: _ClassVar[ErrorCode]
    ERROR_CODE_NOT_SUPPORTED: _ClassVar[ErrorCode]
    ERROR_CODE_INVALID_REQUEST: _ClassVar[ErrorCode]
    ERROR_CODE_INTERNAL: _ClassVar[ErrorCode]
    ERROR_CODE_BUSY: _ClassVar[ErrorCode]
    ERROR_CODE_CANCELLED: _ClassVar[ErrorCode]
    ERROR_CODE_CONNECTION_LOST: _ClassVar[ErrorCode]
    ERROR_CODE_PROTOCOL_ERROR: _ClassVar[ErrorCode]

class ErrorPriority(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    ERROR_PRIORITY_NONE: _ClassVar[ErrorPriority]
    ERROR_PRIORITY_CRITICAL: _ClassVar[ErrorPriority]

class Kind(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    KIND_UNSPECIFIED: _ClassVar[Kind]
    KIND_FILE: _ClassVar[Kind]
    KIND_DIR: _ClassVar[Kind]
    KIND_SYMLINK: _ClassVar[Kind]
    KIND_OTHER: _ClassVar[Kind]
OPCODE_UNSPECIFIED: Opcode
OPCODE_PING: Opcode
OPCODE_VERSION: Opcode
OPCODE_CONFIGURATION_GET: Opcode
OPCODE_CONFIGURATION_UPDATE: Opcode
OPCODE_RESTART: Opcode
OPCODE_UPDATE: Opcode
OPCODE_COMMAND_EXEC: Opcode
OPCODE_SHELL_OPEN: Opcode
OPCODE_SHELL_INPUT: Opcode
OPCODE_SHELL_CLOSE: Opcode
OPCODE_PROCESS_LIST: Opcode
OPCODE_PROCESS_KILL: Opcode
OPCODE_PROCESS_INFO: Opcode
OPCODE_PROCESS_MONITOR: Opcode
OPCODE_FILE_PUT: Opcode
OPCODE_FILE_GET: Opcode
OPCODE_FILE_CREATE: Opcode
OPCODE_FILE_DELETE: Opcode
OPCODE_FILE_STAT: Opcode
OPCODE_FILE_CHMOD: Opcode
OPCODE_DIR_MAKE: Opcode
OPCODE_DIR_LIST: Opcode
OPCODE_DIR_REMOVE: Opcode
OPCODE_RUN_AS: Opcode
OPCODE_TIMEOUT_SET: Opcode
OPCODE_LOG: Opcode
OPCODE_DETACH: Opcode
OPCODE_TASK_STATUS: Opcode
OPCODE_TASK_CANCEL: Opcode
OPCODE_CANCEL: Opcode
OPCODE_PIPELINE: Opcode
FRAME_TYPE_UNSPECIFIED: FrameType
FRAME_TYPE_REQUEST: FrameType
FRAME_TYPE_RESPONSE: FrameType
FRAME_TYPE_FILE_CHUNK: FrameType
FRAME_TYPE_ERROR: FrameType
FRAME_TYPE_PING: FrameType
FRAME_TYPE_PONG: FrameType
FRAME_TYPE_CAPABILITY: FrameType
FRAME_FLAG_NONE: FrameFlag
FRAME_FLAG_CONTINUATION: FrameFlag
FRAME_FLAG_COMPRESSED: FrameFlag
FRAME_FLAG_PRIORITY: FrameFlag
ERROR_CODE_UNKNOWN: ErrorCode
ERROR_CODE_NOT_FOUND: ErrorCode
ERROR_CODE_PERMISSION_DENIED: ErrorCode
ERROR_CODE_ALREADY_EXISTS: ErrorCode
ERROR_CODE_TIMEOUT: ErrorCode
ERROR_CODE_NOT_SUPPORTED: ErrorCode
ERROR_CODE_INVALID_REQUEST: ErrorCode
ERROR_CODE_INTERNAL: ErrorCode
ERROR_CODE_BUSY: ErrorCode
ERROR_CODE_CANCELLED: ErrorCode
ERROR_CODE_CONNECTION_LOST: ErrorCode
ERROR_CODE_PROTOCOL_ERROR: ErrorCode
ERROR_PRIORITY_NONE: ErrorPriority
ERROR_PRIORITY_CRITICAL: ErrorPriority
KIND_UNSPECIFIED: Kind
KIND_FILE: Kind
KIND_DIR: Kind
KIND_SYMLINK: Kind
KIND_OTHER: Kind

class ErrorInfo(_message.Message):
    __slots__ = ("code", "message", "detail")
    CODE_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    DETAIL_FIELD_NUMBER: _ClassVar[int]
    code: ErrorCode
    message: str
    detail: str
    def __init__(self, code: _Optional[_Union[ErrorCode, str]] = ..., message: _Optional[str] = ..., detail: _Optional[str] = ...) -> None: ...

class KeyValue(_message.Message):
    __slots__ = ("key", "value")
    KEY_FIELD_NUMBER: _ClassVar[int]
    VALUE_FIELD_NUMBER: _ClassVar[int]
    key: str
    value: str
    def __init__(self, key: _Optional[str] = ..., value: _Optional[str] = ...) -> None: ...

class UInt32Pair(_message.Message):
    __slots__ = ("key", "value")
    KEY_FIELD_NUMBER: _ClassVar[int]
    VALUE_FIELD_NUMBER: _ClassVar[int]
    key: int
    value: int
    def __init__(self, key: _Optional[int] = ..., value: _Optional[int] = ...) -> None: ...
