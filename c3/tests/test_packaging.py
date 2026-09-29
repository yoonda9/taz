"""Packaging smoke tests — build the wheel and assert required marker files."""

from __future__ import annotations

import subprocess
import tempfile
import zipfile
from pathlib import Path

import pytest


@pytest.mark.packaging
def test_wheel_contains_py_typed() -> None:
    """taz/py.typed must be present in the wheel for PEP 561 inline-types support."""
    pkg_root = Path(__file__).parent.parent
    with tempfile.TemporaryDirectory() as out_dir:
        result = subprocess.run(  # noqa: S603
            ["uv", "build", "--wheel", "--out-dir", out_dir],  # noqa: S607
            cwd=pkg_root,
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, f"uv build failed:\n{result.stderr}"

        wheels = list(Path(out_dir).glob("*.whl"))
        assert len(wheels) == 1, f"Expected one wheel, got: {wheels}"

        with zipfile.ZipFile(wheels[0]) as whl:
            assert "taz/py.typed" in whl.namelist(), (
                f"taz/py.typed missing from wheel; found: {whl.namelist()}"
            )
