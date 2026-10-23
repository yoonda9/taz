# PyYAML ships no type information. Declares only what tools/dev.py uses.
from typing import Any

def safe_load(stream: str | bytes) -> Any: ...
