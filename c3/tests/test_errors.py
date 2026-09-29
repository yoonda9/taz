"""Tests for the client exception types."""

from __future__ import annotations

import copy
import pickle
from typing import cast

import pytest
from taz.c3.errors import (
    TazConnectionError,
    TazConnectionLost,
    TazError,
    TazProtocolError,
)
from taz.v1 import common_pb2


@pytest.mark.parametrize(
    "error",
    [
        TazError(common_pb2.ERROR_CODE_NOT_FOUND, "no such file", "missing.txt"),
        TazConnectionLost("network died", "host=localhost"),
        TazProtocolError("oversized frame", "type=0x01"),
    ],
)
def test_error_survives_pickle_and_copy(error: TazError) -> None:
    """Errors cross process boundaries (pickle) and copy intact."""
    pickled = pickle.loads(pickle.dumps(error))  # noqa: S301 - our own bytes
    for clone in (pickled, copy.copy(error)):
        assert type(clone) is type(error)
        assert (clone.code, clone.message, clone.detail) == (
            error.code,
            error.message,
            error.detail,
        )
        assert str(clone) == str(error)


def test_repr_tolerates_unknown_error_code() -> None:
    """A code newer than this client (proto3 enums are open) still reprs."""
    future_code = cast(common_pb2.ErrorCode, 99)  # as parsed from a newer daemon
    assert repr(TazError(future_code, "future")) == "TazError(99, 'future')"
    assert repr(TazError(common_pb2.ERROR_CODE_TIMEOUT, "slow")) == (
        "TazError(ERROR_CODE_TIMEOUT, 'slow')"
    )


def test_connection_error_hierarchy() -> None:
    """TazConnectionLost and TazProtocolError are TazConnectionError and TazError."""
    lost = TazConnectionLost("network died")
    proto = TazProtocolError("bad frame")
    for err in (lost, proto):
        assert isinstance(err, TazConnectionError)
        assert isinstance(err, TazError)


def test_connection_lost_uses_code_10() -> None:
    """TazConnectionLost carries ERROR_CODE_CONNECTION_LOST (10)."""
    err = TazConnectionLost("network died")
    assert err.code == common_pb2.ERROR_CODE_CONNECTION_LOST
    assert err.code == 10


def test_protocol_error_uses_code_11() -> None:
    """TazProtocolError carries ERROR_CODE_PROTOCOL_ERROR (11)."""
    err = TazProtocolError("bad frame")
    assert err.code == common_pb2.ERROR_CODE_PROTOCOL_ERROR
    assert err.code == 11


def test_protocol_error_message_starts_with_prefix() -> None:
    """TazProtocolError prepends 'protocol violation:' to the caller's message."""
    err = TazProtocolError("bad frame type")
    assert str(err).startswith("protocol violation:")
    assert "bad frame type" in str(err)


def test_repr_uses_base_format_for_all_classes() -> None:
    """repr shows class name and error-code name for every class in the family."""
    lost = TazConnectionLost("network died")
    proto = TazProtocolError("bad frame", "type=0x01")
    assert repr(lost) == "TazConnectionLost(ERROR_CODE_CONNECTION_LOST, 'network died')"
    assert repr(proto) == (
        "TazProtocolError(ERROR_CODE_PROTOCOL_ERROR, "
        "'protocol violation: bad frame', 'type=0x01')"
    )


def test_protocol_error_subclass_repr_uses_its_own_name() -> None:
    class TazFramingError(TazProtocolError):
        pass

    assert repr(TazFramingError("bad")) == (
        "TazFramingError(ERROR_CODE_PROTOCOL_ERROR, 'protocol violation: bad')"
    )
    assert TazFramingError("bad").code == common_pb2.ERROR_CODE_PROTOCOL_ERROR
