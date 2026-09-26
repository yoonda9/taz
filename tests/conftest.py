"""Integration test fixtures.

The daemon fixture (launch with ``--port 0``, read ``LISTENING port=<N>`` from
stdout, yield a connected client) lands in Step 3 together with the TCP
server. Until then, integration tests are skipped unless a daemon binary is
supplied explicitly.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--daemon",
        action="store",
        default=os.environ.get("TAZER_DAEMON"),
        help="Path to a tazer daemon binary (or set TAZER_DAEMON).",
    )


@pytest.fixture(scope="session")
def daemon_binary(request: pytest.FixtureRequest) -> Path:
    value = request.config.getoption("--daemon")
    if not value:
        pytest.skip("no daemon binary: pass --daemon or set TAZER_DAEMON")
    path = Path(value)
    if not path.exists():
        pytest.fail(f"daemon binary not found: {path}")
    return path
