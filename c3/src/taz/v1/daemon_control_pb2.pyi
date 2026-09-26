from taz.v1 import common_pb2 as _common_pb2
from google.protobuf.internal import containers as _containers
from google.protobuf import descriptor as _descriptor
from google.protobuf import message as _message
from collections.abc import Iterable as _Iterable, Mapping as _Mapping
from typing import ClassVar as _ClassVar, Optional as _Optional, Union as _Union

DESCRIPTOR: _descriptor.FileDescriptor

class VersionRequest(_message.Message):
    __slots__ = ()
    def __init__(self) -> None: ...

class VersionResponse(_message.Message):
    __slots__ = ("version", "build", "platform")
    VERSION_FIELD_NUMBER: _ClassVar[int]
    BUILD_FIELD_NUMBER: _ClassVar[int]
    PLATFORM_FIELD_NUMBER: _ClassVar[int]
    version: str
    build: str
    platform: str
    def __init__(self, version: _Optional[str] = ..., build: _Optional[str] = ..., platform: _Optional[str] = ...) -> None: ...

class CapabilityPayload(_message.Message):
    __slots__ = ("protocol_major", "protocol_minor", "operations", "max_payload_sizes", "compression")
    PROTOCOL_MAJOR_FIELD_NUMBER: _ClassVar[int]
    PROTOCOL_MINOR_FIELD_NUMBER: _ClassVar[int]
    OPERATIONS_FIELD_NUMBER: _ClassVar[int]
    MAX_PAYLOAD_SIZES_FIELD_NUMBER: _ClassVar[int]
    COMPRESSION_FIELD_NUMBER: _ClassVar[int]
    protocol_major: int
    protocol_minor: int
    operations: _containers.RepeatedScalarFieldContainer[int]
    max_payload_sizes: _containers.RepeatedCompositeFieldContainer[_common_pb2.UInt32Pair]
    compression: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, protocol_major: _Optional[int] = ..., protocol_minor: _Optional[int] = ..., operations: _Optional[_Iterable[int]] = ..., max_payload_sizes: _Optional[_Iterable[_Union[_common_pb2.UInt32Pair, _Mapping]]] = ..., compression: _Optional[_Iterable[str]] = ...) -> None: ...

class ConfigurationGetRequest(_message.Message):
    __slots__ = ("keys",)
    KEYS_FIELD_NUMBER: _ClassVar[int]
    keys: _containers.RepeatedScalarFieldContainer[str]
    def __init__(self, keys: _Optional[_Iterable[str]] = ...) -> None: ...

class ConfigurationGetResponse(_message.Message):
    __slots__ = ("config",)
    CONFIG_FIELD_NUMBER: _ClassVar[int]
    config: _containers.RepeatedCompositeFieldContainer[_common_pb2.KeyValue]
    def __init__(self, config: _Optional[_Iterable[_Union[_common_pb2.KeyValue, _Mapping]]] = ...) -> None: ...

class ConfigurationUpdateRequest(_message.Message):
    __slots__ = ("config",)
    CONFIG_FIELD_NUMBER: _ClassVar[int]
    config: _containers.RepeatedCompositeFieldContainer[_common_pb2.KeyValue]
    def __init__(self, config: _Optional[_Iterable[_Union[_common_pb2.KeyValue, _Mapping]]] = ...) -> None: ...

class RejectedKey(_message.Message):
    __slots__ = ("key", "reason")
    KEY_FIELD_NUMBER: _ClassVar[int]
    REASON_FIELD_NUMBER: _ClassVar[int]
    key: str
    reason: str
    def __init__(self, key: _Optional[str] = ..., reason: _Optional[str] = ...) -> None: ...

class ConfigurationUpdateResponse(_message.Message):
    __slots__ = ("applied", "rejected")
    APPLIED_FIELD_NUMBER: _ClassVar[int]
    REJECTED_FIELD_NUMBER: _ClassVar[int]
    applied: _containers.RepeatedScalarFieldContainer[str]
    rejected: _containers.RepeatedCompositeFieldContainer[RejectedKey]
    def __init__(self, applied: _Optional[_Iterable[str]] = ..., rejected: _Optional[_Iterable[_Union[RejectedKey, _Mapping]]] = ...) -> None: ...

class RestartRequest(_message.Message):
    __slots__ = ("delay_ms",)
    DELAY_MS_FIELD_NUMBER: _ClassVar[int]
    delay_ms: int
    def __init__(self, delay_ms: _Optional[int] = ...) -> None: ...

class RestartResponse(_message.Message):
    __slots__ = ("acknowledged", "effective_delay_ms")
    ACKNOWLEDGED_FIELD_NUMBER: _ClassVar[int]
    EFFECTIVE_DELAY_MS_FIELD_NUMBER: _ClassVar[int]
    acknowledged: bool
    effective_delay_ms: int
    def __init__(self, acknowledged: _Optional[bool] = ..., effective_delay_ms: _Optional[int] = ...) -> None: ...
