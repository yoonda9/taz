"""Tests for the client exception types."""

from __future__ import annotations

import copy
import pickle
from typing import cast

import pytest
from taz.c3.errors import TazError, TazProtocolError
from taz.v1 import common_pb2


@pytest.mark.parametrize(
    "error",
    [
        TazError(common_pb2.ERROR_CODE_NOT_FOUND, "no such file", "missing.txt"),
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
    future_code = cast(common_pb2.ErrorCode, 11)  # as parsed from a newer daemon
    assert repr(TazError(future_code, "future")) == "TazError(11, 'future')"
    assert repr(TazError(common_pb2.ERROR_CODE_TIMEOUT, "slow")) == (
        "TazError(ERROR_CODE_TIMEOUT, 'slow')"
    )


def test_protocol_error_subclass_repr_uses_its_own_name() -> None:
    class TazFramingError(TazProtocolError):
        pass

    assert repr(TazFramingError("bad")) == "TazFramingError('bad')"
    assert TazFramingError("bad").code == common_pb2.ERROR_CODE_INTERNAL
