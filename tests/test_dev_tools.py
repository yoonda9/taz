"""Unit tests for tools/dev.py's release tooling: the musl toolchain fetch,
the musl Conan home, verify-static and release-bundle.

`tools/` is not a package (no `__init__.py`), so the module is loaded
directly from its file path.
"""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import shutil
import subprocess
import tarfile
from collections.abc import Callable
from pathlib import Path, PureWindowsPath
from types import ModuleType
from typing import Any

import pytest

_SPEC = importlib.util.spec_from_file_location(
    "dev", Path(__file__).resolve().parent.parent / "tools" / "dev.py"
)
assert _SPEC is not None and _SPEC.loader is not None
dev: ModuleType = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(dev)


def _no_network(*_args: Any, **_kwargs: Any) -> None:
    raise AssertionError("no network access expected")


# --- cache root precedence ---------------------------------------------------


def test_cache_root_prefers_taz_cache_dir(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    explicit = tmp_path / "explicit"
    monkeypatch.setenv("TAZ_CACHE_DIR", str(explicit))
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "xdg"))
    assert dev.cache_root() == explicit
    assert explicit.is_dir()


def test_cache_root_falls_back_to_xdg_cache_home(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("TAZ_CACHE_DIR", raising=False)
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "xdg"))
    assert dev.cache_root() == tmp_path / "xdg" / "taz"


def test_cache_root_falls_back_to_home(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("TAZ_CACHE_DIR", raising=False)
    monkeypatch.delenv("XDG_CACHE_HOME", raising=False)
    monkeypatch.setattr(dev.Path, "home", lambda: tmp_path)
    assert dev.cache_root() == tmp_path / ".cache" / "taz"


# --- valid install present -> no network -------------------------------------


def _x86_64_toolchain() -> Any:
    return dev.MUSL_TOOLCHAINS[dev.DEFAULT_MUSL_ARCH]


def _patch_x86_64_toolchain(monkeypatch: pytest.MonkeyPatch, **changes: str) -> Any:
    """Replace the x86_64 table entry with one carrying `changes` applied."""
    patched = _x86_64_toolchain()._replace(**changes)
    monkeypatch.setitem(dev.MUSL_TOOLCHAINS, dev.DEFAULT_MUSL_ARCH, patched)
    return patched


def test_ensure_musl_toolchain_fast_path_needs_no_network(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    toolchain = _x86_64_toolchain()
    directory = tmp_path / "toolchain"
    (directory / "bin").mkdir(parents=True)
    (directory / "bin" / f"{toolchain.bin_prefix}-gcc").write_text("#!/bin/sh\n")
    (directory / ".taz-sha256").write_text(toolchain.sha256)

    monkeypatch.setattr(dev, "musl_toolchain_dir", lambda arch=None: directory)
    monkeypatch.setattr(dev.urllib.request, "urlopen", _no_network)

    assert dev.ensure_musl_toolchain() == directory


def test_ensure_musl_toolchain_stale_stamp_is_not_valid(tmp_path: Path) -> None:
    toolchain = _x86_64_toolchain()
    directory = tmp_path / "toolchain"
    (directory / "bin").mkdir(parents=True)
    (directory / "bin" / f"{toolchain.bin_prefix}-gcc").write_text("#!/bin/sh\n")
    (directory / ".taz-sha256").write_text("stale")

    assert dev._musl_toolchain_valid(directory) is False


# --- download ----------------------------------------------------------------


def _make_tarball(path: Path, payload: bytes) -> None:
    with tarfile.open(path, "w:xz") as tar:
        info = tarfile.TarInfo("README")
        info.size = len(payload)
        tar.addfile(info, io.BytesIO(payload))


def test_download_musl_toolchain_hash_mismatch_installs_nothing(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    tarball = tmp_path / "src" / "fake.tar.xz"
    tarball.parent.mkdir()
    _make_tarball(tarball, b"not the real toolchain")

    wrong_sha = "0" * 64
    _patch_x86_64_toolchain(
        monkeypatch, url=tarball.resolve().as_uri(), sha256=wrong_sha
    )

    directory = tmp_path / "install" / "toolchain"

    with pytest.raises(SystemExit) as exc_info:
        dev._download_musl_toolchain(directory)

    message = str(exc_info.value)
    assert wrong_sha in message
    assert hashlib.sha256(tarball.read_bytes()).hexdigest() in message
    assert not directory.exists()
    assert list(directory.parent.iterdir()) == []


def _make_toolchain_tarball(path: Path, toolchain: Any) -> bytes:
    """A tarball shaped like Bootlin's: one top-level directory holding
    bin/<prefix>-gcc and relocate-sdk.sh. Returns its bytes."""
    top = toolchain.name
    with tarfile.open(path, "w:xz") as tar:
        names = (f"{top}/bin/{toolchain.bin_prefix}-gcc", f"{top}/relocate-sdk.sh")
        for name in names:
            info = tarfile.TarInfo(name)
            info.size = 0
            info.mode = 0o755
            tar.addfile(info, io.BytesIO(b""))
    return path.read_bytes()


def test_download_musl_toolchain_installs_relocates_and_stamps(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    tarball = tmp_path / "src" / "toolchain.tar.xz"
    tarball.parent.mkdir()
    data = _make_toolchain_tarball(tarball, _x86_64_toolchain())
    toolchain = _patch_x86_64_toolchain(
        monkeypatch,
        url=tarball.resolve().as_uri(),
        sha256=hashlib.sha256(data).hexdigest(),
    )
    relocations: list[list[str]] = []

    def fake_run(cmd: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
        relocations.append(cmd)
        return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

    monkeypatch.setattr(dev.subprocess, "run", fake_run)
    directory = tmp_path / "install" / toolchain.name

    dev._download_musl_toolchain(directory, quiet=True)

    assert dev._musl_toolchain_valid(directory)
    # The single top-level directory is flattened into `directory`, and the
    # SDK is told its final location before it is moved there.
    assert (directory / "relocate-sdk.sh").is_file()
    assert len(relocations) == 1
    assert Path(relocations[0][0]).name == "relocate-sdk.sh"
    assert relocations[0][1:] == [str(directory)]
    # Nothing else is left next to it: no .part file, no temp directory.
    assert list(directory.parent.iterdir()) == [directory]


def test_download_musl_toolchain_keeps_a_concurrently_installed_tree(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    tarball = tmp_path / "src" / "toolchain.tar.xz"
    tarball.parent.mkdir()
    data = _make_toolchain_tarball(tarball, _x86_64_toolchain())
    toolchain = _patch_x86_64_toolchain(
        monkeypatch,
        url=tarball.resolve().as_uri(),
        sha256=hashlib.sha256(data).hexdigest(),
    )
    monkeypatch.setattr(
        dev.subprocess,
        "run",
        lambda cmd, **_kwargs: subprocess.CompletedProcess(
            cmd, 0, stdout="", stderr=""
        ),
    )
    directory = tmp_path / "install" / toolchain.name
    (directory / "bin").mkdir(parents=True)
    (directory / "bin" / f"{toolchain.bin_prefix}-gcc").write_text("")
    (directory / ".taz-sha256").write_text(toolchain.sha256)
    marker = directory / "in-use"
    marker.write_text("")

    dev._download_musl_toolchain(directory, quiet=True)

    assert marker.exists()
    assert list(directory.parent.iterdir()) == [directory]


# --- --print-key format ------------------------------------------------------


def test_musl_toolchain_print_key_format(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    fake_sha = "ab" * 32
    _patch_x86_64_toolchain(monkeypatch, sha256=fake_sha)
    monkeypatch.setattr(dev.urllib.request, "urlopen", _no_network)

    dev.cmd_musl_toolchain(print_only=False, print_key=True)

    assert capsys.readouterr().out.strip() == f"musl-toolchain-{fake_sha}"


# --- host profile chains unchanged except for musl ---------------------------


def test_host_profiles_for_existing_presets_unchanged() -> None:
    profiles_dir = dev.PROFILES_DIR
    assert dev.host_profiles_for("linux-release") == [
        "default",
        str(profiles_dir / "linux-static"),
    ]
    assert dev.host_profiles_for("linux-debug") == [
        "default",
        str(profiles_dir / "linux-static"),
    ]
    assert dev.host_profiles_for("linux-clang-debug") == [
        "clang",
        str(profiles_dir / "linux-static"),
        str(profiles_dir / "linux-clang"),
    ]
    assert dev.host_profiles_for("linux-fuzz") == [
        "clang",
        str(profiles_dir / "linux-static"),
        str(profiles_dir / "linux-clang"),
    ]
    assert dev.host_profiles_for("windows-debug") == [
        "default",
        str(profiles_dir / "windows-static"),
    ]
    assert dev.host_profiles_for("windows-release") == [
        "default",
        str(profiles_dir / "windows-static"),
    ]


def test_host_profiles_for_musl_is_a_standalone_complete_profile() -> None:
    assert dev.host_profiles_for("linux-musl-release") == [
        str(dev.PROFILES_DIR / "linux-musl")
    ]


def test_host_profiles_for_musl_i686_and_aarch64_pick_their_own_profile() -> None:
    assert dev.host_profiles_for("linux-musl-i686-release") == [
        str(dev.PROFILES_DIR / "linux-musl-i686")
    ]
    assert dev.host_profiles_for("linux-musl-aarch64-release") == [
        str(dev.PROFILES_DIR / "linux-musl-aarch64")
    ]


def test_is_musl_preset() -> None:
    assert dev.is_musl_preset("linux-musl-release") is True
    assert dev.is_musl_preset("linux-musl-i686-release") is True
    assert dev.is_musl_preset("linux-musl-aarch64-release") is True
    assert dev.is_musl_preset("linux-release") is False
    assert dev.is_musl_preset("linux-debug") is False


def test_musl_arch_for_preset_defaults_to_x86_64() -> None:
    assert dev.musl_arch_for_preset("linux-musl-release") == "x86_64"


def test_musl_arch_for_preset_resolves_i686_and_aarch64() -> None:
    assert dev.musl_arch_for_preset("linux-musl-i686-release") == "i686"
    assert dev.musl_arch_for_preset("linux-musl-aarch64-release") == "aarch64"


# --- conan_env: musl gets its own Conan home ---------------------------------


def test_conan_env_identity_for_non_musl_preset(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("SOME_MARKER", "1")
    assert dev.conan_env("linux-release") == dict(os.environ)


def test_conan_env_sets_own_conan_home_and_toolchain_for_musl(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    toolchain = tmp_path / "toolchains" / "x"
    monkeypatch.setattr(dev, "cache_root", lambda: tmp_path)
    monkeypatch.setattr(dev, "musl_toolchain_dir", lambda arch=None: toolchain)

    env = dev.conan_env("linux-musl-release")

    assert env["CONAN_HOME"] == str(tmp_path / "conan-musl")
    assert env["TAZ_MUSL_TOOLCHAIN"] == str(toolchain)


def test_conan_env_gives_i686_and_aarch64_their_own_conan_home(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(dev, "cache_root", lambda: tmp_path)
    monkeypatch.setattr(dev, "musl_toolchain_dir", lambda arch: tmp_path / arch)

    i686_env = dev.conan_env("linux-musl-i686-release")
    aarch64_env = dev.conan_env("linux-musl-aarch64-release")

    assert i686_env["CONAN_HOME"] == str(tmp_path / "conan-musl-i686")
    assert i686_env["TAZ_MUSL_TOOLCHAIN"] == str(tmp_path / "i686")
    assert aarch64_env["CONAN_HOME"] == str(tmp_path / "conan-musl-aarch64")
    assert aarch64_env["TAZ_MUSL_TOOLCHAIN"] == str(tmp_path / "aarch64")


# --- conan-cache-key ---------------------------------------------------------


def test_conan_cache_key_digest_includes_musl_sha_only_for_musl_preset(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    fake_profile = json.dumps(
        {
            "host": {
                "settings": {
                    "os": "Linux",
                    "compiler": "gcc",
                    "compiler.version": "15",
                    "build_type": "Release",
                }
            }
        }
    )

    def fake_run(cmd: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
        if "detect" in cmd:
            # What `conan profile detect` prints on a fresh Conan home.
            stdout = "[settings]\narch=x86_64\nos=Linux\n"
        elif "--version" in cmd:
            stdout = "2.x.y\n"
        else:
            stdout = fake_profile
        return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr="")

    monkeypatch.setattr(dev, "tool", lambda name: name)
    monkeypatch.setattr(dev.subprocess, "run", fake_run)
    # A fresh musl Conan home (as on every CI runner): no default profile, so
    # each musl key also runs `conan profile detect`.
    monkeypatch.setattr(dev, "cache_root", lambda: tmp_path)

    def key_for(preset: str) -> str:
        dev.cmd_conan_cache_key(preset)
        out = capsys.readouterr().out
        # CI writes this stdout into $GITHUB_OUTPUT: exactly one line.
        assert out.count("\n") == 1, out
        return out.strip()

    glibc_key = key_for("linux-release")
    _patch_x86_64_toolchain(monkeypatch, sha256="a" * 64)
    musl_key_a = key_for("linux-musl-release")
    _patch_x86_64_toolchain(monkeypatch, sha256="b" * 64)
    musl_key_b = key_for("linux-musl-release")

    for key in (glibc_key, musl_key_a, musl_key_b):
        assert key.startswith("conan-Linux-gcc-15-Release-")
    assert musl_key_a != musl_key_b
    assert glibc_key != musl_key_a


# --- verify-static -----------------------------------------------------------


def test_daemon_binary_name_matches_platform() -> None:
    expected = "tazd.exe" if dev.IS_WINDOWS else "tazd"
    assert dev.daemon_binary("linux-musl-release").name == expected


def test_nss_calls_flags_known_identifiers_and_ignores_taz_helpers(
    tmp_path: Path,
) -> None:
    src = tmp_path / "src"
    include = tmp_path / "include"
    src.mkdir()
    include.mkdir()
    example = src / "example.c"
    example.write_text(
        "// reads /etc/passwd via getpwnam, but this line is only a comment\n"
        "void f(void) {\n"
        "    struct passwd *p = getpwnam(name);\n"
        "    uv_os_homedir (buf, &len);\n"
        "    taz_passwd_lookup_by_name(name);\n"
        "}\n"
    )

    hits = dev.nss_calls((src, include))

    assert hits == [f"{example}:3", f"{example}:4"]


def test_nss_calls_ignores_calls_written_inside_comments(tmp_path: Path) -> None:
    src = tmp_path / "src"
    include = tmp_path / "include"
    src.mkdir()
    include.mkdir()
    example = src / "example.c"
    example.write_text(
        "// must never call getaddrinfo(...) here\n"
        "/* getpwnam(name) is also off limits\n"
        "   so is uv_getaddrinfo(loop, &req, cb, node, service, hints) */\n"
        "void f(void) {\n"
        "    int x = 1; /* inline initgroups(name, gid) */ int y = 2;\n"
        "    (void)x;\n"
        "    (void)y;\n"
        "}\n"
    )

    hits = dev.nss_calls((src, include))

    assert hits == []


def test_nss_calls_ignores_calls_written_inside_string_literals(
    tmp_path: Path,
) -> None:
    src = tmp_path / "src"
    include = tmp_path / "include"
    src.mkdir()
    include.mkdir()
    example = src / "example.c"
    example.write_text(
        'const char *msg = "getaddrinfo(host, port) fails";\n'
        'const char *quip = "a \\" quote then getpwnam(name)";\n'
        "void f(void) {\n"
        "    (void)msg;\n"
        "    (void)quip;\n"
        "}\n"
    )

    hits = dev.nss_calls((src, include))

    assert hits == []


def test_nss_calls_catches_uses_a_line_based_scan_would_miss(tmp_path: Path) -> None:
    src = tmp_path / "src"
    src.mkdir()
    example = src / "example.c"
    example.write_text(
        "#if 0\n"
        "it's disabled\n"  # a stray quote must not swallow the code below
        "#endif\n"
        "void f(void) {\n"
        "    getaddrinfo\n"
        "        (node, service, &hints, &res);\n"
        "    lookup_fn fn = &getpwnam;\n"
        "    getnameinfo(sa, len, host, hostlen, NULL, 0, 0);\n"
        "    getgroups(0, NULL);\n"  # a plain syscall, not NSS
        "}\n"
        "/* \u2190 \u2010 non-cp1252 UTF-8 in a comment */\n",
        encoding="utf-8",
    )

    hits = dev.nss_calls((src,))

    assert hits == [f"{example}:5", f"{example}:7", f"{example}:8"]


_READELF_HEADERS_DYNAMIC = """
Program Headers:
  Type           Offset             VirtAddr           PhysAddr
  PHDR           0x0000000000000040 0x0000000000400040 0x0000000000400040
  INTERP         0x0000000000000238 0x0000000000400238 0x0000000000400238
      [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
  LOAD           0x0000000000000000 0x0000000000400000 0x0000000000400000
"""

_READELF_HEADERS_STATIC = """
Program Headers:
  Type           Offset   VirtAddr   PhysAddr
  LOAD           0x000000 0x00400000 0x00400000
  NOTE           0x000238 0x00400238 0x00400238
"""

_READELF_DYNAMIC_SECTION = """
Dynamic section at offset 0xe00 contains 20 entries:
  Tag        Type                         Name/Value
 0x0000000000000001 (NEEDED)             Shared library: [libc.so.6]
 0x000000000000000c (INIT)               0x401000
"""

_READELF_NO_DYNAMIC_SECTION = "\nThere is no dynamic section in this file.\n"


def test_elf_has_interp() -> None:
    assert dev.elf_has_interp(_READELF_HEADERS_DYNAMIC) is True
    assert dev.elf_has_interp(_READELF_HEADERS_STATIC) is False


def test_elf_needed() -> None:
    assert dev.elf_needed(_READELF_DYNAMIC_SECTION) == ["libc.so.6"]
    assert dev.elf_needed(_READELF_NO_DYNAMIC_SECTION) == []


def _fake_readelf(
    headers: str, dynamic: str
) -> Callable[..., subprocess.CompletedProcess[str]]:
    def fake_run(cmd: list[str], **_kwargs: Any) -> subprocess.CompletedProcess[str]:
        stdout = headers if "-lW" in cmd else dynamic
        return subprocess.CompletedProcess(cmd, 0, stdout=stdout, stderr="")

    return fake_run


def _patch_verify_static(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, headers: str, dynamic: str
) -> list[Path]:
    """Point cmd_verify_static's Linux path at a fake binary and readelf;
    returns the list the smoke run (stubbed) appends its binary to."""
    binary = tmp_path / "tazd"
    binary.write_bytes(b"\x7fELF")
    clean_src = tmp_path / "clean-src"
    clean_src.mkdir()
    smoked: list[Path] = []
    monkeypatch.setattr(dev, "IS_WINDOWS", False)
    monkeypatch.setattr(dev, "daemon_binary", lambda _preset: binary)
    monkeypatch.setattr(dev, "DAEMON_C_DIRS", (clean_src,))
    monkeypatch.setattr(dev, "tool", lambda name: name)
    monkeypatch.setattr(dev.subprocess, "run", _fake_readelf(headers, dynamic))
    monkeypatch.setattr(dev, "_verify_static_smoke", smoked.append)
    return smoked


def test_cmd_verify_static_rejects_a_binary_with_an_interpreter(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    smoked = _patch_verify_static(
        tmp_path, monkeypatch, _READELF_HEADERS_DYNAMIC, _READELF_DYNAMIC_SECTION
    )

    with pytest.raises(SystemExit, match="INTERP"):
        dev.cmd_verify_static("linux-musl-release")
    assert smoked == []


def test_cmd_verify_static_rejects_needed_entries(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    smoked = _patch_verify_static(
        tmp_path, monkeypatch, _READELF_HEADERS_STATIC, _READELF_DYNAMIC_SECTION
    )

    with pytest.raises(SystemExit, match=r"libc\.so\.6"):
        dev.cmd_verify_static("linux-musl-release")
    assert smoked == []


def test_cmd_verify_static_rejects_nss_calls_in_daemon_sources(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _patch_verify_static(
        tmp_path, monkeypatch, _READELF_HEADERS_STATIC, _READELF_NO_DYNAMIC_SECTION
    )
    src = tmp_path / "nss-src"
    src.mkdir()
    (src / "user.c").write_text("void f(void) { getpwnam(name); }\n")
    monkeypatch.setattr(dev, "DAEMON_C_DIRS", (src,))

    with pytest.raises(SystemExit, match=r"user\.c:1"):
        dev.cmd_verify_static("linux-musl-release")


def test_cmd_verify_static_passes_a_static_binary_on_to_the_smoke_run(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    smoked = _patch_verify_static(
        tmp_path, monkeypatch, _READELF_HEADERS_STATIC, _READELF_NO_DYNAMIC_SECTION
    )

    dev.cmd_verify_static("linux-musl-release")

    assert smoked == [tmp_path / "tazd"]
    assert "static link verified" in capsys.readouterr().out


def test_cmd_verify_static_no_smoke_runs_readelf_but_skips_the_smoke_run(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    # The aarch64 lane cross-builds on x86_64 and can't execute the binary, so
    # --no-smoke keeps the (cross-arch) readelf checks but skips the run smoke.
    smoked = _patch_verify_static(
        tmp_path, monkeypatch, _READELF_HEADERS_STATIC, _READELF_NO_DYNAMIC_SECTION
    )

    dev.cmd_verify_static("linux-musl-release", smoke=False)

    assert smoked == []
    assert "static link verified" in capsys.readouterr().out


def test_verify_static_smoke_missing_docker_warns_locally_fails_in_ci(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    binary = tmp_path / "tazd"
    binary.write_bytes(b"\x7fELF")
    monkeypatch.setattr(dev.shutil, "which", lambda _name: None)

    monkeypatch.setattr(dev, "IN_CI", False)
    dev._verify_static_smoke(binary)
    assert "skipping" in capsys.readouterr().out

    monkeypatch.setattr(dev, "IN_CI", True)
    with pytest.raises(SystemExit):
        dev._verify_static_smoke(binary)


# --- release-bundle ----------------------------------------------------------


def test_release_artifacts_cover_the_new_musl_arches() -> None:
    assert dev.RELEASE_ARTIFACTS["linux-musl-i686-release"] == "taz-linux-i686"
    assert dev.RELEASE_ARTIFACTS["linux-musl-aarch64-release"] == "taz-linux-aarch64"


def test_release_bundle_rejects_non_release_preset(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(dev, "ROOT", tmp_path)

    with pytest.raises(SystemExit, match="not a release preset"):
        dev.cmd_release_bundle("linux-debug")


def test_release_bundle_writes_binary_sha256_license_and_notices(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.setattr(dev, "ROOT", tmp_path)
    monkeypatch.setattr(dev, "IS_WINDOWS", False)
    binary_dir = tmp_path / "daemon" / "build" / "linux-musl-release"
    binary_dir.mkdir(parents=True)
    binary = binary_dir / "tazd"
    binary.write_bytes(b"\x7fELF fake static binary")
    (tmp_path / "LICENSE").write_text("licence text\n")
    (tmp_path / "THIRD_PARTY_NOTICES").write_text("notices text\n")

    out_dir = tmp_path / "out" / "release" / "taz-linux-x64"
    out_dir.mkdir(parents=True)
    stale = out_dir / "stale.txt"
    stale.write_text("leftover from a previous bundle\n")

    dev.cmd_release_bundle("linux-musl-release")

    assert not stale.exists()
    assert (out_dir / "tazd").read_bytes() == binary.read_bytes()
    assert (out_dir / "LICENSE").read_text() == "licence text\n"
    assert (out_dir / "THIRD_PARTY_NOTICES").read_text() == "notices text\n"

    expected_digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    sha_line = f"{expected_digest}  tazd\n".encode()
    assert (out_dir / "tazd.sha256").read_bytes() == sha_line

    sha256sum = shutil.which("sha256sum")
    if sha256sum is not None:
        subprocess.run(
            [sha256sum, "-c", "tazd.sha256"],
            cwd=out_dir,
            check=True,
            capture_output=True,
        )


def test_release_bundle_names_windows_artifacts_tazd_exe(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(dev, "ROOT", tmp_path)
    monkeypatch.setattr(dev, "IS_WINDOWS", True)
    binary_dir = tmp_path / "daemon" / "build" / "windows-release"
    binary_dir.mkdir(parents=True)
    binary = binary_dir / "tazd.exe"
    binary.write_bytes(b"MZ fake static binary")
    (tmp_path / "LICENSE").write_text("licence text\n")
    (tmp_path / "THIRD_PARTY_NOTICES").write_text("notices text\n")

    dev.cmd_release_bundle("windows-release")

    out_dir = tmp_path / "out" / "release" / "taz-windows-x64"
    assert (out_dir / "tazd.exe").read_bytes() == binary.read_bytes()

    expected_digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    # LF even when written on Windows: coreutils 8.x `sha256sum -c` rejects CRLF.
    sha_line = f"{expected_digest}  tazd.exe\n".encode()
    assert (out_dir / "tazd.exe.sha256").read_bytes() == sha_line


# --- verify-release-bundle ---------------------------------------------------


def _bundle_fixture(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Build a real linux-musl-release bundle via cmd_release_bundle, so the
    verify tests exercise the exact artifact CI would upload."""
    monkeypatch.setattr(dev, "ROOT", tmp_path)
    monkeypatch.setattr(dev, "IS_WINDOWS", False)
    binary_dir = tmp_path / "daemon" / "build" / "linux-musl-release"
    binary_dir.mkdir(parents=True)
    (binary_dir / "tazd").write_bytes(b"\x7fELF fake static binary")
    (tmp_path / "LICENSE").write_text("licence text\n")
    (tmp_path / "THIRD_PARTY_NOTICES").write_text("notices text\n")

    dev.cmd_release_bundle("linux-musl-release")
    return tmp_path / "out" / "release" / "taz-linux-x64"


def test_verify_release_bundle_rejects_non_release_preset(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(dev, "ROOT", tmp_path)

    with pytest.raises(SystemExit, match="not a release preset"):
        dev.cmd_verify_release_bundle("linux-debug")


def test_verify_release_bundle_passes_on_a_freshly_written_bundle(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    _bundle_fixture(tmp_path, monkeypatch)

    dev.cmd_verify_release_bundle("linux-musl-release")

    assert "verified" in capsys.readouterr().out


def test_verify_release_bundle_fails_on_missing_sha256(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    out_dir = _bundle_fixture(tmp_path, monkeypatch)
    (out_dir / "tazd.sha256").unlink()

    with pytest.raises(SystemExit, match="sha256"):
        dev.cmd_verify_release_bundle("linux-musl-release")


def test_verify_release_bundle_fails_on_digest_mismatch(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    out_dir = _bundle_fixture(tmp_path, monkeypatch)
    # Simulate a binary that was rebuilt after the digest was written.
    (out_dir / "tazd").write_bytes(b"\x7fELF a different binary entirely")

    with pytest.raises(SystemExit, match="does not match"):
        dev.cmd_verify_release_bundle("linux-musl-release")


def test_verify_release_bundle_fails_on_stale_notices(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    out_dir = _bundle_fixture(tmp_path, monkeypatch)
    # Simulate THIRD_PARTY_NOTICES changing in the repo after the bundle shipped.
    (tmp_path / "THIRD_PARTY_NOTICES").write_text("updated notices text\n")

    with pytest.raises(SystemExit, match="THIRD_PARTY_NOTICES"):
        dev.cmd_verify_release_bundle("linux-musl-release")
    assert out_dir.exists()


def test_verify_release_bundle_fails_on_missing_license(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    out_dir = _bundle_fixture(tmp_path, monkeypatch)
    (out_dir / "LICENSE").unlink()

    with pytest.raises(SystemExit, match="LICENSE"):
        dev.cmd_verify_release_bundle("linux-musl-release")


def test_justfile_verify_release_bundle_does_not_rebundle() -> None:
    """CI runs `just release-bundle` then `just verify-release-bundle` as two
    separate steps so verify checks the artifact the first step produced. If
    the `verify-release-bundle` recipe itself depends on `release-bundle`, it
    silently re-bundles (and re-configures/re-builds) before checking,
    tautologically verifying its own fresh copy instead of the one CI already
    uploaded from the prior step -- defeating the point of a separate check."""
    just = shutil.which("just")
    assert just is not None, "just must be on PATH to run this test"
    result = subprocess.run(
        [just, "--dry-run", "verify-release-bundle", "linux-musl-release"],
        cwd=dev.ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    # `just --dry-run` prints the commands it would run to stderr, not stdout.
    planned = result.stderr

    assert "configure" not in planned
    assert "cmake --build" not in planned
    assert " release-bundle " not in f" {planned} "


# --- THIRD_PARTY_NOTICES covers every musl toolchain -------------------------


def test_third_party_notices_mentions_every_musl_toolchain() -> None:
    notices = (dev.ROOT / "THIRD_PARTY_NOTICES").read_text()
    for toolchain in dev.MUSL_TOOLCHAINS.values():
        assert toolchain.name in notices, (
            f"{toolchain.name} missing from THIRD_PARTY_NOTICES"
        )


# --- test-py-system (SYSTEM lane) --------------------------------------------


def test_random_password_has_every_class_and_never_repeats() -> None:
    passwords = {dev.random_password() for _ in range(50)}
    assert len(passwords) == 50
    for password in passwords:
        assert len(password) == 24
        assert any(c.islower() for c in password)
        assert any(c.isupper() for c in password)
        assert any(c.isdigit() for c in password)
        assert any(not c.isalnum() for c in password)


@pytest.mark.parametrize(
    ("returncode", "output", "state"),
    [
        (2, "The user name could not be found.\n", "missing"),
        (0, "User name  taz-runas\nAccount active  Yes\n", "exists"),
        (0, "User name  taz-runas\nAccount active  No\n", "inactive"),
        (0, "User name  taz-runas\n", "exists"),
    ],
)
def test_net_user_state(returncode: int, output: str, state: str) -> None:
    assert dev.net_user_state(returncode, output) == state


def _wrapper(*, ci: bool, args: tuple[str, ...] = ()) -> str:
    return str(
        dev.system_lane_wrapper(
            root=PureWindowsPath(r"C:\taz"),
            python=PureWindowsPath(r"C:\taz\.venv\Scripts\python.exe"),
            daemon=PureWindowsPath(r"C:\taz\daemon\build\windows-debug\tazd.exe"),
            out_dir=PureWindowsPath(r"C:\taz\out\test-py-system"),
            user="taz-runas",
            pytest_args=args,
            ci=ci,
        )
    )


def test_system_lane_wrapper_sets_the_environment_and_writes_exit_last() -> None:
    text = _wrapper(ci=False, args=("-k", "run as"))
    lines = text.split("\r\n")
    assert lines[0] == "@echo off"
    assert "set CI=1" not in lines
    assert "set TAZ_TEST_USER=taz-runas" in lines
    assert "set TAZ_DAEMON=C:\\taz\\daemon\\build\\windows-debug\\tazd.exe" in lines
    assert "set TEMP=C:\\taz\\out\\test-py-system\\tmp" in lines
    assert "set PYTHONDONTWRITEBYTECODE=1" in lines
    pytest_line = next(line for line in lines if "-m pytest" in line)
    assert pytest_line.startswith(
        r"C:\taz\.venv\Scripts\python.exe -m pytest -m system"
    )
    assert '-k "run as"' in pytest_line
    assert pytest_line.endswith(r'> "C:\taz\out\test-py-system\log.txt" 2>&1')
    assert lines[-2] == r'echo %ERRORLEVEL% > "C:\taz\out\test-py-system\exit.txt"'


def test_system_lane_wrapper_forwards_ci() -> None:
    assert "set CI=1" in _wrapper(ci=True).split("\r\n")


JUNIT_NO_SKIPS = (
    "<testsuites><testsuite>"
    '<testcase classname="a.B" name="t1"/>'
    "</testsuite></testsuites>"
)
JUNIT_TWO_SKIPS = (
    "<testsuites>"
    "<testsuite>"
    '<testcase classname="a.B" name="t1"><skipped message="x"/></testcase>'
    "</testsuite>"
    "<testsuite><testsuite>"
    '<testcase classname="a.C" name="t2"><skipped/></testcase>'
    '<testcase classname="a.C" name="t3"/>'
    "</testsuite></testsuite>"
    "</testsuites>"
)


def test_skipped_rows() -> None:
    assert dev.skipped_rows(JUNIT_NO_SKIPS) == []
    assert dev.skipped_rows(JUNIT_TWO_SKIPS) == ["a.B::t1", "a.C::t2"]
    assert dev.skipped_rows(
        "<testsuite><testcase name='x'><skipped/></testcase></testsuite>"
    ) == ["::x"]


@pytest.mark.skipif(os.name == "nt", reason="asserts the off-Windows refusal")
def test_test_py_system_refuses_off_windows(capsys: pytest.CaptureFixture[str]) -> None:
    with pytest.raises(SystemExit) as raised:
        dev.cmd_test_py_system(None, 1, [])
    assert raised.value.code == 2
    assert "Windows-only" in capsys.readouterr().err
