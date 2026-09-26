from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class ProcessInfo(_message.Message):
    __slots__ = ("pid", "name", "user", "cpu_percent", "memory_bytes", "state")
    PID_FIELD_NUMBER: _ClassVar[int]
    NAME_FIELD_NUMBER: _ClassVar[int]
    USER_FIELD_NUMBER: _ClassVar[int]
    CPU_PERCENT_FIELD_NUMBER: _ClassVar[int]
    MEMORY_BYTES_FIELD_NUMBER: _ClassVar[int]
    STATE_FIELD_NUMBER: _ClassVar[int]
    pid: int
    name: str
    user: str
    cpu_percent: float
    memory_bytes: int
    state: str
    def __init__(self, pid: _Optional[int] = ..., name: _Optional[str] = ..., user: _Optional[str] = ..., cpu_percent: _Optional[float] = ..., memory_bytes: _Optional[int] = ..., state: _Optional[str] = ...) -> None: ...

class ProcessListRequest(_message.Message):
    __slots__ = ("filter",)
    FILTER_FIELD_NUMBER: _ClassVar[int]
    filter: str
    def __init__(self, filter: _Optional[str] = ...) -> None: ...

class ProcessListResponse(_message.Message):
    __slots__ = ("processes",)
    PROCESSES_FIELD_NUMBER: _ClassVar[int]
    processes: _containers.RepeatedCompositeFieldContainer[ProcessInfo]
    def __init__(self, processes: _Optional[_Iterable[_Union[ProcessInfo, _Mapping]]] = ...) -> None: ...

class ProcessKillRequest(_message.Message):
    __slots__ = ("pid", "signal")
    PID_FIELD_NUMBER: _ClassVar[int]
    SIGNAL_FIELD_NUMBER: _ClassVar[int]
    pid: int
    signal: int
    def __init__(self, pid: _Optional[int] = ..., signal: _Optional[int] = ...) -> None: ...

class ProcessKillResponse(_message.Message):
    __slots__ = ("success",)
    SUCCESS_FIELD_NUMBER: _ClassVar[int]
    success: bool
    def __init__(self, success: _Optional[bool] = ...) -> None: ...

class ProcessInfoRequest(_message.Message):
    __slots__ = ("pid",)
    PID_FIELD_NUMBER: _ClassVar[int]
    pid: int
    def __init__(self, pid: _Optional[int] = ...) -> None: ...

class ProcessInfoResponse(_message.Message):
    __slots__ = ("info", "command_line", "start_time", "open_files")
    INFO_FIELD_NUMBER: _ClassVar[int]
    COMMAND_LINE_FIELD_NUMBER: _ClassVar[int]
    START_TIME_FIELD_NUMBER: _ClassVar[int]
    OPEN_FILES_FIELD_NUMBER: _ClassVar[int]
    info: ProcessInfo
    command_line: str
    start_time: int
    open_files: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, info: _Optional[_Union[ProcessInfo, _Mapping]] = ..., command_line: _Optional[str] = ..., start_time: _Optional[int] = ..., open_files: _Optional[_Iterable[str]] = ...) -> None: ...

class ProcessMonitorRequest(_message.Message):
    __slots__ = ("pid", "interval_ms")
    PID_FIELD_NUMBER: _ClassVar[int]
    INTERVAL_MS_FIELD_NUMBER: _ClassVar[int]
    pid: int
    interval_ms: int
    def __init__(self, pid: _Optional[int] = ..., interval_ms: _Optional[int] = ...) -> None: ...

class ProcessMonitorResponse(_message.Message):
    __slots__ = ("info", "exited", "exit_code", "reason", "exit_code_known")
    INFO_FIELD_NUMBER: _ClassVar[int]
    EXITED_FIELD_NUMBER: _ClassVar[int]
    EXIT_CODE_FIELD_NUMBER: _ClassVar[int]
    REASON_FIELD_NUMBER: _ClassVar[int]
    EXIT_CODE_KNOWN_FIELD_NUMBER: _ClassVar[int]
    info: ProcessInfo
    exited: bool
    exit_code: int
    reason: str
    exit_code_known: bool
    def __init__(self, info: _Optional[_Union[ProcessInfo, _Mapping]] = ..., exited: _Optional[bool] = ..., exit_code: _Optional[int] = ..., reason: _Optional[str] = ..., exit_code_known: _Optional[bool] = ...) -> None: ...
