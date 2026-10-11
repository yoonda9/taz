from typing import Any

from taz.v1 import common_pb2


def _code_name(code: int) -> str:
    # proto3 enums are open: a newer daemon may send a code this client lacks.
    if code in common_pb2.ErrorCode.values():
        return common_pb2.ErrorCode.Name(code)
    return str(code)


class TazError(Exception):
    def __init__(
        self,
        code: common_pb2.ErrorCode,
        message: str,
        detail: str = "",
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.detail = detail

    def __reduce__(self) -> tuple[Any, ...]:
        # The default reduce calls cls(*self.args), which does not match the
        # constructor signatures here, so rebuild from the attributes instead
        # (keeps pickle, copy and multiprocessing working for subclasses).
        return (type(self).__new__, (type(self), self.message), self.__dict__)

    def __repr__(self) -> str:
        cls = type(self).__name__
        name = _code_name(self.code)
        if self.detail:
            return f"{cls}({name}, {self.message!r}, {self.detail!r})"
        return f"{cls}({name}, {self.message!r})"


class TazConnectionError(TazError):
    """The connection is unusable; reconnect."""


class TazConnectionLost(TazConnectionError):  # noqa: N818
    def __init__(self, message: str, detail: str = "") -> None:
        super().__init__(common_pb2.ERROR_CODE_CONNECTION_LOST, message, detail)


class TazProtocolError(TazConnectionError):
    def __init__(self, message: str, detail: str = "") -> None:
        super().__init__(
            common_pb2.ERROR_CODE_PROTOCOL_ERROR,
            f"protocol violation: {message}",
            detail,
        )


class TazChecksumError(TazError):
    """A file transfer's CRC32C checksum did not match."""

    def __init__(self, expected: bytes, actual: bytes) -> None:
        super().__init__(
            common_pb2.ERROR_CODE_INTERNAL,
            f"checksum mismatch: expected {expected.hex()} got {actual.hex()}",
        )
        self.expected = expected
        self.actual = actual


class TazPriorityError(Exception):
    """A detached task's CRITICAL-priority FAILED escalation (api.md §5.4).

    Delivered out of band on a FRAME_FLAG_PRIORITY ERROR frame (protocol.md
    §10.2) for the task's originating DETACH stream. Under the default
    ``priority_policy="raise"`` this is raised from a discrete call;
    ``"queue"`` instead appends it to ``TazClient.priority_errors``.
    ``error`` is the ``TazError`` the frame's ``ErrorInfo`` decodes to.
    """

    def __init__(self, task_id: int, stream_id: int, error: TazError) -> None:
        super().__init__(f"task {task_id} (stream {stream_id}): {error}")
        self.task_id = task_id
        self.stream_id = stream_id
        self.error = error
