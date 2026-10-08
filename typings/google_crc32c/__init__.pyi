# google-crc32c re-exports these from a C extension that ships no stubs, so
# without this file mypy sees them as Any. Declares only what TAZ uses.
from collections.abc import Buffer
from typing import Final

implementation: Final[str]

def value(chunk: Buffer, /) -> int: ...
def extend(crc: int, chunk: Buffer, /) -> int: ...
