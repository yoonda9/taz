from __future__ import annotations

import dataclasses
from typing import ClassVar


@dataclasses.dataclass(frozen=True, slots=True)
class Keepalive:
    """Keepalive probe timings.

    Use ``Keepalive.OFF`` to disable probing entirely (never send PING,
    wait indefinitely for frames).
    """

    idle: float = 30.0
    timeout: float = 30.0

    # Excluded from __slots__ and __init__ by the dataclass machinery.
    OFF: ClassVar[Keepalive]


# Assigned after class body because the class must exist before it can be
# referenced as the type of its own ClassVar sentinel.
Keepalive.OFF = Keepalive(idle=float("inf"), timeout=float("inf"))


@dataclasses.dataclass(frozen=True, slots=True)
class Backlog:
    """Bounds on the per-connection receive backlog."""

    max_frames: int = 256
    max_bytes: int = 16 * 2**20
