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


class TazProtocolError(TazError):
    """Raised when the peer violates the wire protocol; the connection is unusable."""

    def __init__(self, message: str, detail: str = "") -> None:
        super().__init__(common_pb2.ERROR_CODE_INTERNAL, message, detail)

    def __repr__(self) -> str:
        cls = type(self).__name__
        if self.detail:
            return f"{cls}({self.message!r}, {self.detail!r})"
        return f"{cls}({self.message!r})"
