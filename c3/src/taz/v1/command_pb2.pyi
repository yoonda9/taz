from taz.v1 import common_pb2 as _common_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class CommandExecRequest(_message.Message):
    __slots__ = ("command", "args", "env", "working_dir", "timeout_ms", "as_user")
    COMMAND_FIELD_NUMBER: _ClassVar[int]
    ARGS_FIELD_NUMBER: _ClassVar[int]
    ENV_FIELD_NUMBER: _ClassVar[int]
    WORKING_DIR_FIELD_NUMBER: _ClassVar[int]
    TIMEOUT_MS_FIELD_NUMBER: _ClassVar[int]
    AS_USER_FIELD_NUMBER: _ClassVar[int]
    command: str
    args: _containers.RepeatedScalarFieldContainer[str]
    env: _containers.RepeatedCompositeFieldContainer[_common_pb2.KeyValue]
    working_dir: str
    timeout_ms: int
    as_user: str
    def __init__(self, command: _Optional[str] = ..., args: _Optional[_Iterable[str]] = ..., env: _Optional[_Iterable[_Union[_common_pb2.KeyValue, _Mapping]]] = ..., working_dir: _Optional[str] = ..., timeout_ms: _Optional[int] = ..., as_user: _Optional[str] = ...) -> None: ...

class CommandExecResponse(_message.Message):
    __slots__ = ("exit_code", "stdout", "stderr", "timed_out", "truncated")
    EXIT_CODE_FIELD_NUMBER: _ClassVar[int]
    STDOUT_FIELD_NUMBER: _ClassVar[int]
    STDERR_FIELD_NUMBER: _ClassVar[int]
    TIMED_OUT_FIELD_NUMBER: _ClassVar[int]
    TRUNCATED_FIELD_NUMBER: _ClassVar[int]
    exit_code: int
    stdout: bytes
    stderr: bytes
    timed_out: bool
    truncated: bool
    def __init__(self, exit_code: _Optional[int] = ..., stdout: _Optional[bytes] = ..., stderr: _Optional[bytes] = ..., timed_out: _Optional[bool] = ..., truncated: _Optional[bool] = ...) -> None: ...
