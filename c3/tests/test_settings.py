"""Tests for Keepalive and Backlog settings types."""

from __future__ import annotations

import dataclasses

import pytest
from taz.c3.settings import Backlog, Keepalive


class TestKeepalive:
    def test_defaults(self) -> None:
        k = Keepalive()
        assert k.idle == 30.0
        assert k.timeout == 30.0

    def test_frozen(self) -> None:
        k = Keepalive()
        with pytest.raises(dataclasses.FrozenInstanceError):
            k.idle = 5.0  # type: ignore[misc]

    def test_slots(self) -> None:
        assert "__slots__" in Keepalive.__dict__

    def test_off_is_singleton(self) -> None:
        assert Keepalive.OFF is Keepalive.OFF

    def test_off_identity_check(self) -> None:
        sentinel = Keepalive.OFF
        assert sentinel is Keepalive.OFF
        assert Keepalive() is not Keepalive.OFF

    def test_off_is_keepalive_instance(self) -> None:
        assert isinstance(Keepalive.OFF, Keepalive)

    def test_off_has_infinite_timings(self) -> None:
        assert Keepalive.OFF.idle == float("inf")
        assert Keepalive.OFF.timeout == float("inf")

    def test_custom_values(self) -> None:
        k = Keepalive(idle=10.0, timeout=5.0)
        assert k.idle == 10.0
        assert k.timeout == 5.0


class TestBacklog:
    def test_defaults(self) -> None:
        b = Backlog()
        assert b.max_frames == 256
        assert b.max_bytes == 16 * 2**20

    def test_frozen(self) -> None:
        b = Backlog()
        with pytest.raises(dataclasses.FrozenInstanceError):
            b.max_frames = 10  # type: ignore[misc]

    def test_slots(self) -> None:
        assert "__slots__" in Backlog.__dict__

    def test_custom_values(self) -> None:
        b = Backlog(max_frames=64, max_bytes=1024)
        assert b.max_frames == 64
        assert b.max_bytes == 1024


class TestExports:
    def test_keepalive_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.Keepalive is Keepalive

    def test_backlog_exported_from_taz_c3(self) -> None:
        import taz.c3

        assert taz.c3.Backlog is Backlog

    def test_both_in_all(self) -> None:
        import taz.c3

        assert "Keepalive" in taz.c3.__all__
        assert "Backlog" in taz.c3.__all__
