from taz.v1 import common_pb2


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

    def __repr__(self) -> str:
        name = common_pb2.ErrorCode.Name(self.code)
        if self.detail:
            return f"TazError({name}, {self.message!r}, {self.detail!r})"
        return f"TazError({name}, {self.message!r})"


class TazProtocolError(TazError):
    """Raised when the peer violates the wire protocol; the connection is unusable."""

    def __init__(self, message: str, detail: str = "") -> None:
        super().__init__(common_pb2.ERROR_CODE_INTERNAL, message, detail)

    def __repr__(self) -> str:
        if self.detail:
            return f"TazProtocolError({self.message!r}, {self.detail!r})"
        return f"TazProtocolError({self.message!r})"
