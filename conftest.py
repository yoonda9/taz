"""Command-line options shared by every test directory.

pytest only registers options from conftest.py files it loads before parsing
the command line, picked from the command-line paths (or testpaths): this
rootdir one for any path inside the repo, tests/conftest.py only for paths
under tests/. Registered in tests/conftest.py, `pytest --daemon PATH` failed:
PATH was taken for a test path, which hid --daemon.

A daemon binary outside the repo still leads pytest away from it, so pass
that one as --daemon=PATH or TAZ_DAEMON=PATH instead.
"""

from __future__ import annotations

import os

import pytest


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--daemon",
        action="store",
        default=os.environ.get("TAZ_DAEMON"),
        help="Path to a tazd binary (or set TAZ_DAEMON).",
    )
