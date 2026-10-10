"""LogEntry: the result type of ``TazClient.log()``."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class LogEntry:
    """One entry of a ``LOG`` call."""

    timestamp: int
    level: str
    message: str
