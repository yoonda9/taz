from taz.v1 import command_pb2 as _command_pb2
from taz.v1 import common_pb2 as _common_pb2
from taz.v1 import file_pb2 as _file_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf.internal import enum_type_wrapper as _enum_type_wrapper
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class TaskState(int, metaclass=_enum_type_wrapper.EnumTypeWrapper):
    __slots__ = ()
    TASK_STATE_UNSPECIFIED: _ClassVar[TaskState]
    TASK_STATE_PENDING: _ClassVar[TaskState]
    TASK_STATE_RUNNING: _ClassVar[TaskState]
    TASK_STATE_COMPLETED: _ClassVar[TaskState]
    TASK_STATE_FAILED: _ClassVar[TaskState]
    TASK_STATE_CANCELLED: _ClassVar[TaskState]
TASK_STATE_UNSPECIFIED: TaskState
TASK_STATE_PENDING: TaskState
TASK_STATE_RUNNING: TaskState
TASK_STATE_COMPLETED: TaskState
TASK_STATE_FAILED: TaskState
TASK_STATE_CANCELLED: TaskState

class RunAsRequest(_message.Message):
    __slots__ = ("user",)
    USER_FIELD_NUMBER: _ClassVar[int]
    user: str
    def __init__(self, user: _Optional[str] = ...) -> None: ...

class RunAsResponse(_message.Message):
    __slots__ = ("success", "effective_user")
    SUCCESS_FIELD_NUMBER: _ClassVar[int]
    EFFECTIVE_USER_FIELD_NUMBER: _ClassVar[int]
    success: bool
    effective_user: str
    def __init__(self, success: _Optional[bool] = ..., effective_user: _Optional[str] = ...) -> None: ...

class TimeoutSetRequest(_message.Message):
    __slots__ = ("timeout_ms",)
    TIMEOUT_MS_FIELD_NUMBER: _ClassVar[int]
    timeout_ms: int
    def __init__(self, timeout_ms: _Optional[int] = ...) -> None: ...

class TimeoutSetResponse(_message.Message):
    __slots__ = ("previous_ms",)
    PREVIOUS_MS_FIELD_NUMBER: _ClassVar[int]
    previous_ms: int
    def __init__(self, previous_ms: _Optional[int] = ...) -> None: ...

class LogRequest(_message.Message):
    __slots__ = ("lines", "since", "level")
    LINES_FIELD_NUMBER: _ClassVar[int]
    SINCE_FIELD_NUMBER: _ClassVar[int]
    LEVEL_FIELD_NUMBER: _ClassVar[int]
    lines: int
    since: int
    level: str
    def __init__(self, lines: _Optional[int] = ..., since: _Optional[int] = ..., level: _Optional[str] = ...) -> None: ...

class LogEntry(_message.Message):
    __slots__ = ("timestamp", "level", "message")
    TIMESTAMP_FIELD_NUMBER: _ClassVar[int]
    LEVEL_FIELD_NUMBER: _ClassVar[int]
    MESSAGE_FIELD_NUMBER: _ClassVar[int]
    timestamp: int
    level: str
    message: str
    def __init__(self, timestamp: _Optional[int] = ..., level: _Optional[str] = ..., message: _Optional[str] = ...) -> None: ...

class LogResponse(_message.Message):
    __slots__ = ("entries",)
    ENTRIES_FIELD_NUMBER: _ClassVar[int]
    entries: _containers.RepeatedCompositeFieldContainer[LogEntry]
    def __init__(self, entries: _Optional[_Iterable[_Union[LogEntry, _Mapping]]] = ...) -> None: ...

class DetachRequest(_message.Message):
    __slots__ = ("exec", "file_put", "file_get", "monitor", "error_priority")
    EXEC_FIELD_NUMBER: _ClassVar[int]
    FILE_PUT_FIELD_NUMBER: _ClassVar[int]
    FILE_GET_FIELD_NUMBER: _ClassVar[int]
    MONITOR_FIELD_NUMBER: _ClassVar[int]
    ERROR_PRIORITY_FIELD_NUMBER: _ClassVar[int]
    exec: _command_pb2.CommandExecRequest
    file_put: _file_pb2.FilePutRequest
    file_get: _file_pb2.FileGetRequest
    monitor: bool
    error_priority: _common_pb2.ErrorPriority
    def __init__(self, exec: _Optional[_Union[_command_pb2.CommandExecRequest, _Mapping]] = ..., file_put: _Optional[_Union[_file_pb2.FilePutRequest, _Mapping]] = ..., file_get: _Optional[_Union[_file_pb2.FileGetRequest, _Mapping]] = ..., monitor: _Optional[bool] = ..., error_priority: _Optional[_Union[_common_pb2.ErrorPriority, str]] = ...) -> None: ...

class DetachResponse(_message.Message):
    __slots__ = ("task_id", "pid")
    TASK_ID_FIELD_NUMBER: _ClassVar[int]
    PID_FIELD_NUMBER: _ClassVar[int]
    task_id: int
    pid: int
    def __init__(self, task_id: _Optional[int] = ..., pid: _Optional[int] = ...) -> None: ...

class TaskStatusRequest(_message.Message):
    __slots__ = ("task_id", "subscribe", "interval_ms")
    TASK_ID_FIELD_NUMBER: _ClassVar[int]
    SUBSCRIBE_FIELD_NUMBER: _ClassVar[int]
    INTERVAL_MS_FIELD_NUMBER: _ClassVar[int]
    task_id: int
    subscribe: bool
    interval_ms: int
    def __init__(self, task_id: _Optional[int] = ..., subscribe: _Optional[bool] = ..., interval_ms: _Optional[int] = ...) -> None: ...

class TaskStatusResponse(_message.Message):
    __slots__ = ("state", "pid", "progress", "progress_total", "result", "error")
    STATE_FIELD_NUMBER: _ClassVar[int]
    PID_FIELD_NUMBER: _ClassVar[int]
    PROGRESS_FIELD_NUMBER: _ClassVar[int]
    PROGRESS_TOTAL_FIELD_NUMBER: _ClassVar[int]
    RESULT_FIELD_NUMBER: _ClassVar[int]
    ERROR_FIELD_NUMBER: _ClassVar[int]
    state: TaskState
    pid: int
    progress: int
    progress_total: int
    result: bytes
    error: _common_pb2.ErrorInfo
    def __init__(self, state: _Optional[_Union[TaskState, str]] = ..., pid: _Optional[int] = ..., progress: _Optional[int] = ..., progress_total: _Optional[int] = ..., result: _Optional[bytes] = ..., error: _Optional[_Union[_common_pb2.ErrorInfo, _Mapping]] = ...) -> None: ...

class TaskCancelRequest(_message.Message):
    __slots__ = ("task_id", "signal")
    TASK_ID_FIELD_NUMBER: _ClassVar[int]
    SIGNAL_FIELD_NUMBER: _ClassVar[int]
    task_id: int
    signal: int
    def __init__(self, task_id: _Optional[int] = ..., signal: _Optional[int] = ...) -> None: ...

class TaskCancelResponse(_message.Message):
    __slots__ = ("accepted",)
    ACCEPTED_FIELD_NUMBER: _ClassVar[int]
    accepted: bool
    def __init__(self, accepted: _Optional[bool] = ...) -> None: ...

class CancelRequest(_message.Message):
    __slots__ = ("target_stream_id",)
    TARGET_STREAM_ID_FIELD_NUMBER: _ClassVar[int]
    target_stream_id: int
    def __init__(self, target_stream_id: _Optional[int] = ...) -> None: ...

class CancelResponse(_message.Message):
    __slots__ = ("cancelled",)
    CANCELLED_FIELD_NUMBER: _ClassVar[int]
    cancelled: bool
    def __init__(self, cancelled: _Optional[bool] = ...) -> None: ...
