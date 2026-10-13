#!/usr/bin/env python3
"""Cross-platform helpers behind the `just` recipes.

Anything that would otherwise need shell globbing, per-OS branching, or more
than one line lives here so the justfile stays a flat list of commands that
read identically on Linux, macOS and Windows.

Subcommands:
    conan-profiles       create the detected Conan base profiles (default, clang)
    musl-toolchain [--print] [--print-key]   fetch/verify the pinned Bootlin
                         musl toolchain used by linux-musl-release
    deps PRESET          conan install (preset's profiles, daemon/conan.lock)
    lock-deps PRESET     re-resolve daemon/conan.lock to the newest Conan revisions
    configure PRESET     deps + cmake --preset
    conan-cache-key PRESET   CI cache key for the Conan packages a preset needs
    conan-cache-trim [PRESET]   drop what a restored CI package cache never uses
    cmake ARGS...        run cmake; on Windows, load the MSVC environment first
    ctest ARGS...        run ctest; same MSVC environment handling
    proto [--check]      regenerate Python + C protobuf code (or verify no diff)
    clang-format [--check]
    clang-tidy --build-dir DIR
    cppcheck
    cmake-format [--check]
    proto-breaking       buf breaking against main (skips if main has no schemas)
    valgrind --build-dir DIR   run the C unit tests under Valgrind memcheck
    valgrind-py --daemon PATH   run the marked Valgrind integration subset (pytest)
    coverage --build-dir DIR   gcovr report for a coverage-instrumented build
    doctor               report tools mise cannot provide / missing pinned tools
    clean                remove daemon build directories
    verify-static PRESET check a preset's already-built daemon binary is
                         fully static (ELF/dumpbin checks, NSS source guard,
                         and on Linux a FROM-scratch docker smoke run)
    release-bundle PRESET   copy a release preset's binary, sha256 file,
                         LICENSE and THIRD_PARTY_NOTICES into out/release/
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import urllib.request
from collections.abc import Iterable, Sequence
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RPC_DIR = ROOT / "rpc"
PROTO_PACKAGE_DIR = Path("taz") / "v1"
PY_OUT = ROOT / "c3" / "src"
C_OUT = ROOT / "daemon" / "generated"
NANOPB_GENERATOR = (
    ROOT / "daemon" / "third_party" / "nanopb" / "generator" / "nanopb_generator.py"
)
DAEMON_C_DIRS = (ROOT / "daemon" / "src", ROOT / "daemon" / "include")
# Fuzz harnesses use the libFuzzer API (LLVMFuzzerTestOneInput), which has a
# fixed external-linkage name that cannot conform to the project naming rules.
# They are formatted with clang-format but excluded from clang-tidy.
DAEMON_FUZZ_DIRS = (ROOT / "daemon" / "fuzz",)
DAEMON_CXX_DIRS = (ROOT / "daemon" / "tests",)
CMAKE_FILES = (
    ROOT / "daemon" / "CMakeLists.txt",
    ROOT / "daemon" / "tests" / "CMakeLists.txt",
    ROOT / "daemon" / "fuzz" / "CMakeLists.txt",
)
GENERATED_PATHS = (PY_OUT / PROTO_PACKAGE_DIR, C_OUT)

IS_WINDOWS = platform.system() == "Windows"
IN_CI = bool(os.environ.get("CI"))


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def run(
    cmd: Sequence[str | Path],
    *,
    cwd: Path = ROOT,
    check: bool = True,
    env: dict[str, str] | None = None,
) -> int:
    printable = " ".join(str(c) for c in cmd)
    print(f"$ {printable}", flush=True)
    result = subprocess.run([str(c) for c in cmd], cwd=cwd, check=False, env=env)
    if check and result.returncode != 0:
        sys.exit(result.returncode)
    return result.returncode


def files_under(dirs: Iterable[Path], suffixes: tuple[str, ...]) -> list[Path]:
    found: list[Path] = []
    for directory in dirs:
        if directory.is_dir():
            found.extend(
                p for p in sorted(directory.rglob("*")) if p.suffix in suffixes
            )
    return found


def proto_files() -> list[Path]:
    return sorted((RPC_DIR / PROTO_PACKAGE_DIR).glob("*.proto"))


def relative_protos() -> list[str]:
    return [str(p.relative_to(RPC_DIR).as_posix()) for p in proto_files()]


def tool(name: str) -> str:
    """Resolve a tool from PATH, failing with a hint if it is missing."""
    path = shutil.which(name)
    if path is None:
        sys.exit(
            f"error: '{name}' not found on PATH. Run `mise install` / `just setup`, "
            "or see `just doctor`."
        )
    return path


# ---------------------------------------------------------------------------
# musl toolchain (Bootlin, for linux-musl-release)
# ---------------------------------------------------------------------------
# Pinned exactly like cppcheck on Windows (mise.toml's conda:cppcheck version,
# ci.yml's CPPCHECK_VERSION/CPPCHECK_SHA256): one place in the repo holds the
# URL and hash (see AGENTS.md's "Pinned versions" table), and this module
# downloads, verifies and caches it. Never re-host the tarball: Bootlin's
# server is its only source, and mirroring a GPL toolchain would make this
# repo a distributor of it.
MUSL_TOOLCHAIN_NAME = "x86-64--musl--stable-2026.08-1"
MUSL_TOOLCHAIN_URL = (
    "https://toolchains.bootlin.com/downloads/releases/toolchains/x86-64/"
    f"tarballs/{MUSL_TOOLCHAIN_NAME}.tar.xz"
)
MUSL_TOOLCHAIN_SHA256 = (
    "78d3a4683d6ac47b5ee73bd5bce210b55eb93dff1b137c61298af97eb0d2b5a6"
)


def cache_root() -> Path:
    """Root for caches that must live outside the repo and outside /tmp.

    /tmp on some hosts (including CI) is a tmpfs with a per-user quota far
    smaller than a toolchain or a from-source Conan cache. CI sets
    TAZ_CACHE_DIR to the `~/...` path its cache steps save, so both agree.
    """
    env = os.environ.get("TAZ_CACHE_DIR")
    if env:
        root = Path(env).expanduser()
    else:
        xdg = os.environ.get("XDG_CACHE_HOME")
        root = Path(xdg) / "taz" if xdg else Path.home() / ".cache" / "taz"
    root.mkdir(parents=True, exist_ok=True)
    return root


def musl_toolchain_dir() -> Path:
    return cache_root() / "toolchains" / MUSL_TOOLCHAIN_NAME


def _musl_toolchain_stamp(directory: Path) -> Path:
    return directory / ".taz-sha256"


def _musl_toolchain_valid(directory: Path) -> bool:
    stamp = _musl_toolchain_stamp(directory)
    gcc = directory / "bin" / "x86_64-linux-gcc"
    return (
        stamp.is_file()
        and stamp.read_text().strip() == MUSL_TOOLCHAIN_SHA256
        and gcc.exists()
    )


def _download_musl_toolchain(directory: Path, *, quiet: bool = False) -> None:
    """Download, verify and unpack the pinned toolchain into `directory`.

    Streams to a `.part` file of its own next to it (never under /tmp),
    verifies the sha256 before extracting, extracts into a sibling temp dir
    with `filter="data"` (no symlinks escaping the tree), points a Buildroot
    `relocate-sdk.sh` (if the tarball carries one) at `directory`, then
    renames the tree into place and stamps it. A concurrent fetch that
    finished first is kept as it is.
    """
    root = directory.parent
    root.mkdir(parents=True, exist_ok=True)
    fd, part_name = tempfile.mkstemp(
        dir=root, prefix=f"{MUSL_TOOLCHAIN_NAME}.", suffix=".tar.xz.part"
    )
    part = Path(part_name)
    tmp: Path | None = None
    try:
        if not quiet:
            print(f"downloading {MUSL_TOOLCHAIN_URL}")
        digest = hashlib.sha256()
        # MUSL_TOOLCHAIN_URL is the constant pinned above, never user input.
        # The timeout bounds each socket operation: a stalled server fails the
        # fetch instead of hanging until the CI job's own timeout.
        with (
            os.fdopen(fd, "wb") as out,
            urllib.request.urlopen(MUSL_TOOLCHAIN_URL, timeout=60) as response,  # noqa: S310
        ):
            while chunk := response.read(1024 * 1024):
                out.write(chunk)
                digest.update(chunk)
        actual = digest.hexdigest()
        if actual != MUSL_TOOLCHAIN_SHA256:
            sys.exit(
                f"error: {MUSL_TOOLCHAIN_NAME} checksum mismatch: "
                f"expected {MUSL_TOOLCHAIN_SHA256}, got {actual}. Nothing installed."
            )
        tmp = Path(tempfile.mkdtemp(dir=root, prefix=f"{MUSL_TOOLCHAIN_NAME}."))
        if not quiet:
            print(f"extracting to {directory}")
        with tarfile.open(part) as tar:
            tar.extractall(tmp, filter="data")
        entries = list(tmp.iterdir())
        extracted = entries[0] if len(entries) == 1 and entries[0].is_dir() else tmp
        relocate = extracted / "relocate-sdk.sh"
        if relocate.exists():
            # Rewrites the build-time prefix in the SDK's text files to the
            # final location (its "set location without moving" mode).
            result = subprocess.run(
                [str(relocate), str(directory)],
                cwd=extracted,
                capture_output=True,
                text=True,
                check=False,
            )
            if result.returncode != 0:
                sys.exit(
                    f"error: {relocate.name} failed (exit {result.returncode}): "
                    f"{result.stdout}{result.stderr}"
                )
        if _musl_toolchain_valid(directory):
            return
        if directory.exists():
            shutil.rmtree(directory)
        extracted.rename(directory)
        _musl_toolchain_stamp(directory).write_text(MUSL_TOOLCHAIN_SHA256 + "\n")
    finally:
        part.unlink(missing_ok=True)
        if tmp is not None:
            shutil.rmtree(tmp, ignore_errors=True)


def ensure_musl_toolchain(*, quiet: bool = False) -> Path:
    """Return the pinned toolchain's directory, fetching it if necessary."""
    directory = musl_toolchain_dir()
    if _musl_toolchain_valid(directory):
        return directory
    _download_musl_toolchain(directory, quiet=quiet)
    if not _musl_toolchain_valid(directory):
        sys.exit(
            f"error: {directory} invalid after fetch "
            "(missing bin/x86_64-linux-gcc or a matching stamp)"
        )
    return directory


def cmd_musl_toolchain(*, print_only: bool, print_key: bool) -> None:
    if print_key:
        print(f"musl-toolchain-{MUSL_TOOLCHAIN_SHA256}")
        return
    directory = ensure_musl_toolchain(quiet=print_only)
    print(directory)


# ---------------------------------------------------------------------------
# MSVC environment (Windows only)
# ---------------------------------------------------------------------------
def ensure_msvc_env(*, required: bool = True) -> bool:
    """Make cl.exe, INCLUDE and LIB available in this process on Windows.

    Ninja + MSVC needs the environment that a Developer Command Prompt sets
    up. Instead of requiring one, locate the newest Visual Studio (or Build
    Tools) with vswhere, run vcvarsall.bat in a child cmd.exe, and import the
    resulting environment. No-op when cl.exe is already on PATH or off Windows.
    """
    if not IS_WINDOWS or shutil.which("cl"):
        return True
    # Windows environment variable names are case-insensitive.
    program_files_x86 = os.environ.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)")
    vswhere = (
        Path(program_files_x86)
        / "Microsoft Visual Studio"
        / "Installer"
        / "vswhere.exe"
    )
    if not vswhere.exists():
        message = (
            "Visual Studio not found (vswhere.exe missing). Install Visual Studio "
            "Build Tools with the 'Desktop development with C++' workload."
        )
        if required:
            sys.exit(f"error: {message}")
        print(f"warning: {message}")
        return False
    install_path = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    vcvars = Path(install_path) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
    if not install_path or not vcvars.exists():
        message = "no Visual Studio installation with the C++ toolset was found"
        if required:
            sys.exit(f"error: {message}")
        print(f"warning: {message}")
        return False
    arch = "arm64" if platform.machine().lower() in {"arm64", "aarch64"} else "x64"
    # Hand cmd.exe one command string: passing a list makes Python escape the
    # inner quotes (\"...\") and cmd.exe then cannot find the batch file. With
    # /s, cmd strips the outermost pair of quotes and runs what is inside.
    dump = subprocess.run(
        f'cmd.exe /s /c ""{vcvars}" {arch} >nul && set"',
        capture_output=True,
        text=True,
        check=False,
    )
    if dump.returncode != 0:
        sys.exit(f"error: vcvarsall.bat failed:\n{dump.stdout}{dump.stderr}")
    for line in dump.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            os.environ[key] = value
    print(f"loaded MSVC environment from {install_path} ({arch})")
    return True


# ---------------------------------------------------------------------------
# Conan + CMake configure
# ---------------------------------------------------------------------------
PROFILES_DIR = ROOT / "daemon" / "profiles"
# Pins every Conan recipe revision, and the versions that recipes request as
# ranges (e.g. libuv's CMake tool); `conan install` fails on anything unlisted.
CONAN_LOCKFILE = ROOT / "daemon" / "conan.lock"
# Set by CI to the preset its Conan package cache is keyed on (see
# cmd_conan_cache_key); `deps` then refuses presets that need other packages.
CI_CONAN_PRESET_VAR = "TAZ_CI_CONAN_PRESET"


def cmd_conan_profiles() -> None:
    """Create the detected base profiles that the presets compose with.

    `default` is the user's own detected profile (never overwritten). `clang`
    is ours, detected with CC/CXX pointed at clang, and refreshed each time so
    it tracks the installed compiler; it is only created when clang exists.
    """
    conan = tool("conan")
    run([conan, "profile", "detect", "--exist-ok"])
    if not IS_WINDOWS and shutil.which("clang") and shutil.which("clang++"):
        env = {**os.environ, "CC": "clang", "CXX": "clang++"}
        print("$ CC=clang CXX=clang++ conan profile detect --name clang --force")
        subprocess.run(
            [conan, "profile", "detect", "--name", "clang", "--force"],
            cwd=ROOT,
            env=env,
            check=True,
        )


def is_musl_preset(preset: str) -> bool:
    return preset.startswith("linux-musl")


def host_profiles_for(preset: str) -> list[str]:
    """Host profile chain for a preset: detected base first, then our partials."""
    if is_musl_preset(preset):
        # Complete profile: the Bootlin toolchain is unknown to `default`.
        return [str(PROFILES_DIR / "linux-musl")]
    if preset.startswith("windows-"):
        return ["default", str(PROFILES_DIR / "windows-static")]
    if preset.startswith("linux-fuzz") or preset.startswith("linux-clang"):
        return [
            "clang",
            str(PROFILES_DIR / "linux-static"),
            str(PROFILES_DIR / "linux-clang"),
        ]
    return ["default", str(PROFILES_DIR / "linux-static")]


def conan_env(preset: str) -> dict[str, str]:
    """Environment for a Conan invocation on `preset`.

    Identity for every preset except musl, which gets its own Conan home
    (so its from-source packages never collide with the glibc ones in
    ~/.conan2 — package ids cover neither the libc nor [conf] keys such as
    compiler_executables) and TAZ_MUSL_TOOLCHAIN for the profile's jinja
    template to read the toolchain's compiler paths from.
    """
    if not is_musl_preset(preset):
        return dict(os.environ)
    return {
        **os.environ,
        "CONAN_HOME": str(cache_root() / "conan-musl"),
        "TAZ_MUSL_TOOLCHAIN": str(musl_toolchain_dir()),
    }


def ensure_musl_conan_home(env: dict[str, str]) -> None:
    """Detect a `default` build profile in the musl preset's own Conan home.

    `--profile:build` defaults to `default`, resolved inside whatever
    CONAN_HOME is active; `just setup`'s `conan-profiles` only ever
    populates the main one. Its output goes to stderr: conan-cache-key's
    stdout must be the key alone (CI captures it into a step output).
    """
    profile = Path(env["CONAN_HOME"]) / "profiles" / "default"
    if profile.is_file():
        return
    cmd = [tool("conan"), "profile", "detect", "--exist-ok"]
    print(f"$ {' '.join(cmd)}", file=sys.stderr, flush=True)
    result = subprocess.run(
        cmd, cwd=ROOT, env=env, capture_output=True, text=True, check=False
    )
    sys.stderr.write(result.stdout + result.stderr)
    if result.returncode != 0:
        sys.exit(result.returncode)


def conan_profile_args(preset: str) -> list[str]:
    """Conan arguments selecting a preset's settings and host profile chain."""
    build_type = "Release" if preset.endswith("-release") else "Debug"
    args = ["-s", f"build_type={build_type}"]
    for profile in host_profiles_for(preset):
        args += ["--profile:host", profile]
    return args


def cmd_deps(preset: str) -> None:
    ensure_msvc_env()
    env = conan_env(preset)
    if is_musl_preset(preset):
        ensure_musl_toolchain()
        ensure_musl_conan_home(env)
    cached = os.environ.get(CI_CONAN_PRESET_VAR)
    if cached and conan_profile_args(preset) != conan_profile_args(cached):
        sys.exit(
            f"error: {preset} needs other Conan packages than {cached}, which this "
            "CI job's package cache is keyed on. Build it in its own matrix entry."
        )
    output = ROOT / "daemon" / "build" / preset / "conan"
    run(
        [
            tool("conan"),
            "install",
            ROOT / "daemon",
            f"--output-folder={output}",
            f"--lockfile={CONAN_LOCKFILE}",
            "--build=missing",
            *conan_profile_args(preset),
        ],
        env=env,
    )


def cmd_lock_deps(preset: str) -> None:
    """Rewrite daemon/conan.lock with the newest revisions on the remotes.

    The recipes' requirements do not depend on the platform, so any one
    preset's profiles resolve the graph for every preset.
    """
    env = conan_env(preset)
    if is_musl_preset(preset):
        ensure_musl_toolchain()
        ensure_musl_conan_home(env)
    run(
        [
            tool("conan"),
            "lock",
            "create",
            ROOT / "daemon",
            "--lockfile=",  # re-resolve instead of reading the current lock
            "--update",
            f"--lockfile-out={CONAN_LOCKFILE}",
            *conan_profile_args(preset),
        ],
        env=env,
    )


def cmd_configure(preset: str) -> None:
    cmd_deps(preset)
    run([tool("cmake"), "-S", ROOT / "daemon", "--preset", preset])


def cmd_conan_cache_key(preset: str) -> None:
    """Print the CI cache key for the Conan packages a preset needs.

    The digest covers everything that decides those binaries: the Conan
    version, the resolved host and build profiles (so a runner image with a
    new compiler starts a fresh cache), conanfile.py and conan.lock, and (for
    musl presets) the pinned toolchain hash, so a toolchain bump starts a
    fresh cache too. CI restores exact matches only: package ids ignore
    [conf] such as compiler_executables, so a cache filled under other
    profiles can satisfy `conan install` with binaries the current profiles
    would not produce.
    """
    conan = tool("conan")
    env = conan_env(preset)
    if is_musl_preset(preset):
        ensure_musl_conan_home(env)
    version = subprocess.run(
        [conan, "--version"], capture_output=True, text=True, check=True, env=env
    ).stdout
    profiles = subprocess.run(
        [conan, "profile", "show", *conan_profile_args(preset), "--format=json"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
        env=env,
    ).stdout
    digest = hashlib.sha256()
    parts = [
        version,
        profiles,
        (ROOT / "daemon" / "conanfile.py").read_text(),
        CONAN_LOCKFILE.read_text(),
    ]
    if is_musl_preset(preset):
        parts.append(MUSL_TOOLCHAIN_SHA256)
    for part in parts:
        digest.update(part.encode())
    host = json.loads(profiles)["host"]["settings"]
    fields = ("os", "compiler", "compiler.version", "build_type")
    print("-".join(["conan", *(host[f] for f in fields), digest.hexdigest()[:16]]))


def cmd_conan_cache_trim(preset: str) -> None:
    """Shrink the Conan package cache before CI saves it.

    Build, source and download folders are never reused, and the CMake tool
    package (about 200 MB) is only needed to build libuv from source, which a
    restored cache never does. Its recipe stays so the graph still resolves
    offline.
    """
    conan = tool("conan")
    env = conan_env(preset)
    run([conan, "cache", "clean"], env=env)
    run([conan, "remove", "cmake/*:*", "--confirm"], env=env)


def cmd_passthrough(program: str, args: Sequence[str]) -> None:
    """Run cmake/ctest with the MSVC environment loaded on Windows.

    ctest needs it too: MSVC's AddressSanitizer runtime is a DLL that lives in
    the MSVC bin directory, which only vcvarsall puts on PATH.
    """
    ensure_msvc_env()
    run([tool(program), *args])


# ---------------------------------------------------------------------------
# proto
# ---------------------------------------------------------------------------
def cmd_proto(check: bool) -> None:
    protos = relative_protos()
    if not protos:
        sys.exit(f"error: no .proto files under {RPC_DIR / PROTO_PACKAGE_DIR}")

    (PY_OUT / PROTO_PACKAGE_DIR).mkdir(parents=True, exist_ok=True)
    (C_OUT / PROTO_PACKAGE_DIR).mkdir(parents=True, exist_ok=True)

    # Python: protoc from grpcio-tools, output mirrors the taz/v1 package.
    run(
        [
            sys.executable,
            "-m",
            "grpc_tools.protoc",
            f"-I{RPC_DIR}",
            f"--python_out={PY_OUT}",
            f"--pyi_out={PY_OUT}",
            *protos,
        ]
    )
    init_py = PY_OUT / PROTO_PACKAGE_DIR / "__init__.py"
    init_py.write_text(
        '"""Generated protobuf modules for TAZ protocol v1 (see rpc/)."""\n'
    )

    # C: nanopb generator from the submodule (same release as the runtime).
    if not NANOPB_GENERATOR.exists():
        sys.exit(
            f"error: {NANOPB_GENERATOR} missing. "
            "Run: git submodule update --init --recursive"
        )
    run(
        [
            sys.executable,
            NANOPB_GENERATOR,
            "--quiet",
            "--no-timestamp",
            f"-I{RPC_DIR}",
            f"-D{C_OUT}",
            *protos,
        ]
    )

    # Generators write CRLF on Windows; .gitattributes only applies on
    # checkout, so normalize here or git sees the files as modified.
    for root in GENERATED_PATHS:
        for path in root.rglob("*"):
            if path.suffix in {".py", ".pyi", ".c", ".h"}:
                data = path.read_bytes()
                if b"\r\n" in data:
                    path.write_bytes(data.replace(b"\r\n", b"\n"))

    if check:
        paths = [str(p.relative_to(ROOT)) for p in GENERATED_PATHS]
        dirty = run(
            ["git", "diff", "--quiet", "--exit-code", "--", *paths], check=False
        )
        untracked = subprocess.run(
            ["git", "ls-files", "--others", "--exclude-standard", "--", *paths],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        ).stdout.strip()
        if dirty or untracked:
            run(["git", "--no-pager", "status", "--short", "--", *paths], check=False)
            sys.exit(
                "error: generated code is out of date. Run `just proto` and commit."
            )
        print("generated code is up to date")


# ---------------------------------------------------------------------------
# C tooling
# ---------------------------------------------------------------------------
def cmd_clang_format(check: bool) -> None:
    sources = (
        files_under(DAEMON_C_DIRS, (".c", ".h"))
        + files_under(DAEMON_FUZZ_DIRS, (".c", ".h"))
        + files_under(DAEMON_CXX_DIRS, (".cpp", ".hpp", ".h"))
    )
    if not sources:
        print("clang-format: no sources")
        return
    args = ["--dry-run", "--Werror"] if check else ["-i"]
    run([tool("clang-format"), "--style=file", *args, *sources])


def cmd_clang_tidy(build_dir: Path) -> None:
    compile_db = build_dir / "compile_commands.json"
    if not compile_db.exists():
        sys.exit(f"error: {compile_db} not found. Run `just configure` first.")
    with compile_db.open() as fh:
        db_files = {Path(e["file"]) for e in json.load(fh)}
    all_sources = files_under(DAEMON_C_DIRS, (".c",)) + files_under(
        DAEMON_CXX_DIRS, (".cpp",)
    )
    # Skip files absent from the compile DB; platform-specific sources for
    # another OS (e.g. platform_win32.c on Linux) are not compiled here.
    sources = [s for s in all_sources if s in db_files]
    if not sources:
        print("clang-tidy: no sources")
        return
    clang_tidy = tool("clang-tidy")

    def one(src: Path) -> tuple[Path, int, str]:
        proc = subprocess.run(
            [clang_tidy, "-p", str(build_dir), "--quiet", str(src)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        return src, proc.returncode, proc.stdout + proc.stderr

    failed = False
    with ThreadPoolExecutor() as pool:
        for src, code, output in pool.map(one, sources):
            status = "ok" if code == 0 else "FAIL"
            print(f"clang-tidy: {src.relative_to(ROOT)} ... {status}")
            if output.strip():
                print(output)
            failed |= code != 0
    if failed:
        sys.exit(1)


def pinned_cppcheck_version() -> str:
    """The cppcheck version in mise.toml; Windows installs must match it."""
    with (ROOT / "mise.toml").open("rb") as f:
        version: str = tomllib.load(f)["tools"]["conda:cppcheck"]["version"]
    return version


def cmd_cppcheck() -> None:
    run(
        [
            tool("cppcheck"),
            "--std=c11",
            "--enable=warning,style,performance,portability",
            "--error-exitcode=1",
            "--inline-suppr",
            "--quiet",
            "--suppress=missingIncludeSystem",
            # Report only on first-party code; headers pulled in from these
            # trees are parsed for context but their findings are not ours.
            "--suppress=*:*/daemon/generated/*",
            "--suppress=*:*/daemon/third_party/*",
            f"-I{ROOT / 'daemon' / 'include'}",
            f"-I{C_OUT}",
            f"-I{ROOT / 'daemon' / 'third_party' / 'nanopb'}",
            *DAEMON_C_DIRS,
        ]
    )


def cmd_cmake_format(check: bool) -> None:
    args = ["--check"] if check else ["-i"]
    run([tool("cmake-format"), *args, *CMAKE_FILES])


def test_binary(build_dir: Path) -> Path:
    exe = build_dir / "tests" / ("taz_tests.exe" if IS_WINDOWS else "taz_tests")
    if not exe.exists():
        sys.exit(f"error: {exe} not found. Run `just build` first.")
    return exe


def cmd_valgrind(build_dir: Path) -> None:
    valgrind = shutil.which("valgrind")
    if valgrind is None:
        message = "valgrind not found on PATH (system package; see `just doctor`)"
        if IN_CI:
            sys.exit(f"error: {message}")
        print(f"warning: {message}; skipping")
        return
    if (build_dir / "CMakeCache.txt").exists() and "TAZ_SANITIZER:STRING=none" not in (
        build_dir / "CMakeCache.txt"
    ).read_text():
        sys.exit("error: run valgrind on a plain build, not a sanitizer build")
    run(
        [
            valgrind,
            "--tool=memcheck",
            "--error-exitcode=1",
            "--leak-check=full",
            "--show-leak-kinds=definite,indirect",
            "--errors-for-leak-kinds=definite,indirect",
            "--track-origins=yes",
            "--gen-suppressions=all",
            f"--suppressions={ROOT / 'daemon' / 'tests' / 'valgrind.supp'}",
            test_binary(build_dir),
        ]
    )


def cmd_valgrind_py(daemon: Path) -> None:
    valgrind = shutil.which("valgrind")
    if valgrind is None:
        message = "valgrind not found on PATH (system package; see `just doctor`)"
        if IN_CI:
            sys.exit(f"error: {message}")
        print(f"warning: {message}; skipping")
        return
    wrapper = shlex.join(
        [
            valgrind,
            "--tool=memcheck",
            "--error-exitcode=99",
            "--leak-check=full",
            "--show-leak-kinds=definite,indirect",
            "--errors-for-leak-kinds=definite,indirect",
            "--track-origins=yes",
            f"--suppressions={ROOT / 'daemon' / 'tests' / 'valgrind.supp'}",
            "--trace-children=yes",
            "--trace-children-skip=*python*",
            # A RESTART's execv starts the traced image's error count afresh,
            # so an error before the re-exec would never reach the exit code.
            "--exit-on-first-error=yes",
        ]
    )
    run(
        [tool("uv"), "run", "pytest", "-m", "valgrind", "tests/test_integration.py"],
        env={**os.environ, "TAZ_DAEMON": str(daemon), "TAZ_DAEMON_WRAPPER": wrapper},
    )


def cmd_coverage(build_dir: Path) -> None:
    out = ROOT / "out" / "coverage" / "c"
    out.mkdir(parents=True, exist_ok=True)
    run(
        [
            tool("gcovr"),
            "--root",
            ROOT,
            "--filter",
            "daemon/src/",
            "--filter",
            "daemon/include/",
            "--exclude-unreachable-branches",
            "--print-summary",
            "--html-details",
            out / "index.html",
            "--xml",
            out / "coverage.xml",
            build_dir,
        ]
    )
    print(f"C coverage report: {out / 'index.html'}")


# ---------------------------------------------------------------------------
# proto breaking-change check
# ---------------------------------------------------------------------------
def git_ok(*args: str) -> bool:
    return (
        subprocess.run(
            ["git", *args], cwd=ROOT, capture_output=True, check=False
        ).returncode
        == 0
    )


def cmd_proto_breaking() -> None:
    against = next(
        (
            ref
            for ref in ("refs/heads/main", "refs/remotes/origin/main")
            if git_ok("rev-parse", "--verify", "--quiet", ref)
        ),
        None,
    )
    if against is None:
        print("warning: no main ref found; skipping buf breaking")
        return
    if not git_ok("cat-file", "-e", f"{against}:rpc/buf.yaml"):
        print(f"note: {against} has no rpc/buf.yaml yet; skipping buf breaking")
        return
    run(
        [
            tool("buf"),
            "breaking",
            RPC_DIR,
            "--against",
            f"{ROOT / '.git'}#ref={against},subdir=rpc",
        ]
    )


# ---------------------------------------------------------------------------
# doctor
# ---------------------------------------------------------------------------
def sanitizer_available(flag: str) -> bool:
    """Compile and link a trivial program with -fsanitize=<flag>."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "probe.c"
        src.write_text("int main(void) { return 0; }\n")
        result = subprocess.run(
            ["cc", f"-fsanitize={flag}", str(src), "-o", str(Path(tmp) / "probe")],
            capture_output=True,
            text=True,
            check=False,
        )
        return result.returncode == 0


def cmd_doctor() -> None:
    mise_tools = ["uv", "prek", "just", "cmake", "ninja", "buf", "prettier"]
    uv_tools = [
        "ruff",
        "mypy",
        "clang-format",
        "clang-tidy",
        "cmake-format",
        "gcovr",
        "conan",
    ]
    # name -> hint. Each is needed only by the recipe it names, which fails on
    # its own when the tool is missing.
    optional_tools: dict[str, str] = {}
    if IS_WINDOWS:
        ensure_msvc_env(required=False)
        # mise installs cppcheck on Linux/macOS only (see mise.toml).
        system_tools = {
            "cl": "MSVC (Visual Studio Build Tools, C++ workload)",
            "cppcheck": f"cppcheck {pinned_cppcheck_version()} "
            "(official installer from cppcheck.sourceforge.io)",
        }
    else:
        mise_tools.append("cppcheck")
        system_tools = {"cc": "C compiler (gcc or clang)"}
        optional_tools = {
            "valgrind": "valgrind (apt/dnf; for `test-valgrind`/`test-py-valgrind`)",
            "clang": "clang (apt/dnf; for `just test-c linux-clang-debug`)",
        }

    problems: list[str] = []
    warnings: list[str] = []

    def report(name: str, hint: str, *, required: bool) -> None:
        if shutil.which(name):
            print(f"  ok       {name}")
        elif required:
            print(f"  MISSING  {name}  <- {hint}")
            problems.append(name)
        else:
            print(f"  missing  {name}  <- {hint}")
            warnings.append(name)

    print("mise-managed tools (fix with `mise install`):")
    for name in mise_tools:
        report(name, "mise install", required=True)
    print("uv-managed tools (fix with `just setup`):")
    for name in uv_tools:
        report(name, "uv sync --group dev", required=True)
    print("system tools:")
    for name, hint in system_tools.items():
        report(name, hint, required=True)
    for name, hint in optional_tools.items():
        report(name, hint, required=False)

    # One cppcheck version everywhere, or `just lint` results differ by machine.
    cppcheck = shutil.which("cppcheck")
    if cppcheck:
        pinned = pinned_cppcheck_version()
        result = subprocess.run(
            [cppcheck, "--version"], capture_output=True, text=True, check=False
        )
        found = (result.stdout.split() or ["unknown"])[-1]
        print("cppcheck version (pinned in mise.toml):")
        if found == pinned:
            print(f"  ok       {found}")
        else:
            print(f"  WRONG    {found}  <- need {pinned}")
            problems.append("cppcheck version")

    if not IS_WINDOWS and shutil.which("cc"):
        print("sanitizer runtimes (for `just test-sanitizers`):")
        for flag, hint in (
            (
                "address,undefined",
                "libasan + libubsan (dnf/apt; ships with gcc on Ubuntu)",
            ),
            ("thread", "libtsan (dnf/apt; ships with gcc on Ubuntu)"),
        ):
            if sanitizer_available(flag):
                print(f"  ok       -fsanitize={flag}")
            else:
                print(f"  missing  -fsanitize={flag}  <- {hint}")
                warnings.append(flag)

    if not IS_WINDOWS:
        print("musl toolchain (optional, for `just build linux-musl-release`):")
        toolchain_dir = musl_toolchain_dir()
        if _musl_toolchain_valid(toolchain_dir):
            print(f"  ok       {toolchain_dir}")
        else:
            print(f"  missing  {toolchain_dir}  <- run `just musl-toolchain`")
            warnings.append("musl-toolchain")

    submodules = [ROOT / "daemon" / "third_party" / "nanopb" / "pb.h"]
    print("submodules:")
    for path in submodules:
        if path.exists():
            print(f"  ok       {path.relative_to(ROOT)}")
        else:
            rel = path.relative_to(ROOT)
            print(f"  MISSING  {rel}  <- git submodule update --init")
            problems.append(str(path))

    if problems:
        sys.exit(f"doctor: {len(problems)} problem(s) found")
    if warnings:
        print(f"doctor: ok ({len(warnings)} optional tool(s) missing)")
    else:
        print("doctor: ok")


# ---------------------------------------------------------------------------
# fuzz
# ---------------------------------------------------------------------------
# libFuzzer's input-size limit per harness. Left to itself libFuzzer caps
# inputs at 4 KiB, which never reaches the end of the 64 KiB reassembly buffer.
FUZZ_FRAME_MAX = 12 + 64 * 1024  # header + largest payload
FUZZ_MAX_LEN = {
    "fuzz_reassembly": 2 + 2 * FUZZ_FRAME_MAX,  # read-size bytes + two frames
    "fuzz_decode": 1 + 64 * 1024,  # type byte + a REQUEST payload
}


def cmd_fuzz(build_dir: Path, seconds: int) -> None:
    """Run each libFuzzer harness for `seconds` from its checked-in seeds.

    New inputs go to a scratch corpus in the build directory: libFuzzer writes
    to the first corpus directory it is given and only reads the others, so
    the seeds in daemon/fuzz/corpus/ stay as committed.
    """
    seeds_dir = ROOT / "daemon" / "fuzz" / "corpus"
    for harness, max_len in FUZZ_MAX_LEN.items():
        binary = build_dir / "fuzz" / harness
        if not binary.exists():
            sys.exit(f"error: fuzz binary not found: {binary}")
        scratch = build_dir / "fuzz-corpus" / harness
        scratch.mkdir(parents=True, exist_ok=True)
        rc = run(
            [
                binary,
                scratch,
                seeds_dir / harness,
                f"-max_total_time={seconds}",
                f"-max_len={max_len}",
                "-error_exitcode=1",
            ],
            check=False,
        )
        if rc != 0:
            sys.exit(f"error: {harness} reported a failure (exit {rc})")


# ---------------------------------------------------------------------------
# clean / verify-static
# ---------------------------------------------------------------------------
def cmd_clean() -> None:
    build_dir = ROOT / "daemon" / "build"
    if build_dir.exists():
        shutil.rmtree(build_dir)
        print(f"removed {build_dir.relative_to(ROOT)}")


def daemon_binary(preset: str) -> Path:
    name = "tazd.exe" if IS_WINDOWS else "tazd"
    return ROOT / "daemon" / "build" / preset / name


# Source-level NSS guard: identifiers that read /etc/passwd, /etc/group or
# resolve names through the system's Name Service Switch, which a static
# binary cannot use (no dlopen of NSS modules). Any use in code counts (a
# call, split across lines or not, or taking the function's address), but
# not a symbol-table grep: libuv's core.c links getpwuid_r and friends into
# every build even though the daemon never calls them, and a symbol grep
# would flag that for nothing.
_NSS_IDENT_RE = re.compile(
    r"\b(?:getpw\w*|[se]etpwent|getgr(?:nam|gid|ent|ouplist)\w*|[se]etgrent"
    r"|initgroups|getspnam\w*|getaddrinfo|getnameinfo|gethostby\w*"
    r"|getservby\w*|getprotoby\w*|getnetby\w*|uv_os_get_passwd\w*"
    r"|uv_os_get_group|uv_os_homedir|uv_getaddrinfo|uv_getnameinfo)\b"
)


def _strip_c_comments(text: str) -> str:
    """Blank out `//` and `/* */` comments and string/char literals.

    Replaces their contents with spaces (newlines kept as-is) so line numbers
    in the caller's output still match the original file, and an identifier
    spelled inside a comment or a string literal never looks like code. A
    literal ends at an unescaped newline, as in C, so a stray quote (an
    apostrophe in `#if 0` prose) cannot hide the code after it.
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            quote = c
            out.append(" ")
            i += 1
            while i < n and text[i] not in (quote, "\n"):
                if text[i] == "\\" and i + 1 < n:
                    out.append(" " if text[i] != "\n" else "\n")
                    out.append(" " if text[i + 1] != "\n" else "\n")
                    i += 2
                    continue
                out.append(" ")
                i += 1
            if i < n and text[i] == quote:
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (text[i] == "*" and i + 1 < n and text[i + 1] == "/"):
                out.append(" " if text[i] != "\n" else "\n")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def nss_calls(dirs: Iterable[Path]) -> list[str]:
    """Return `path:line` for every line using an NSS-backed function."""
    hits: list[str] = []
    for path in files_under(dirs, (".c", ".h")):
        # Explicitly UTF-8: Windows' locale default (cp1252) cannot decode
        # every UTF-8 comment.
        code = _strip_c_comments(path.read_text(encoding="utf-8"))
        lines = (
            code.count("\n", 0, match.start()) + 1
            for match in _NSS_IDENT_RE.finditer(code)
        )
        hits += [f"{path}:{lineno}" for lineno in dict.fromkeys(lines)]
    return hits


def elf_has_interp(program_headers: str) -> bool:
    """Parse `readelf -lW` output for an INTERP program header."""
    return any(line.split()[:1] == ["INTERP"] for line in program_headers.splitlines())


def elf_needed(dynamic_section: str) -> list[str]:
    """Parse `readelf -d` output for NEEDED (shared library) entries."""
    needed = []
    for line in dynamic_section.splitlines():
        if "(NEEDED)" not in line:
            continue
        match = re.search(r"\[(.*?)\]", line)
        if match:
            needed.append(match.group(1))
    return needed


def _verify_static_linux(binary: Path) -> None:
    readelf = tool("readelf")
    headers = subprocess.run(
        [readelf, "-lW", str(binary)], capture_output=True, text=True, check=True
    ).stdout
    print(headers)
    if elf_has_interp(headers):
        sys.exit(f"error: {binary} has an INTERP program header (not static)")

    dynamic = subprocess.run(
        [readelf, "-d", str(binary)], capture_output=True, text=True, check=True
    ).stdout
    print(dynamic)
    needed = elf_needed(dynamic)
    if needed:
        sys.exit(f"error: {binary} has NEEDED entries: {needed}")

    _verify_static_smoke(binary)


def _verify_static_smoke(binary: Path) -> None:
    """Run `/tazd --version` inside a throwaway `FROM scratch` image.

    Proves the binary needs nothing from a base image: no libc, no /etc, no
    dynamic loader. A missing docker fails the check under CI and only warns
    (skipping this smoke run alone) for a local developer.
    """
    docker = shutil.which("docker")
    if docker is None:
        message = "docker not found on PATH; cannot run the FROM-scratch smoke test"
        if IN_CI:
            sys.exit(f"error: {message}")
        print(f"warning: {message}; skipping")
        return

    tag = f"taz-verify-static-{os.getpid()}"
    with tempfile.TemporaryDirectory(dir=binary.parent) as tmp:
        tmp_dir = Path(tmp)
        shutil.copy2(binary, tmp_dir / binary.name)
        (tmp_dir / "Dockerfile").write_text(f"FROM scratch\nCOPY {binary.name} /tazd\n")
        try:
            run([docker, "build", "-q", "-t", tag, str(tmp_dir)])
            result = subprocess.run(
                [docker, "run", "--rm", tag, "/tazd", "--version"],
                capture_output=True,
                text=True,
                check=False,
            )
            print(result.stdout, end="")
            if result.returncode != 0 or not result.stdout.startswith("tazd "):
                sys.exit(
                    f"error: `docker run {tag} /tazd --version` failed "
                    f"(exit {result.returncode}): {result.stdout}{result.stderr}"
                )
        finally:
            subprocess.run([docker, "rmi", "-f", tag], check=False)


def _verify_static_windows(binary: Path) -> None:
    ensure_msvc_env()  # dumpbin ships with MSVC
    allowed = {
        "kernel32.dll",
        "ntdll.dll",
        "advapi32.dll",
        "ws2_32.dll",
        "psapi.dll",
        "iphlpapi.dll",
        "userenv.dll",
        "shell32.dll",
        "dbghelp.dll",
        "ole32.dll",
        "user32.dll",
    }
    out = subprocess.run(
        [tool("dumpbin"), "/dependents", str(binary)],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    deps = {
        line.strip().lower()
        for line in out.splitlines()
        if line.strip().lower().endswith(".dll")
    }
    unexpected = deps - allowed
    print(f"dependencies: {sorted(deps)}")
    if unexpected:
        sys.exit(f"error: unexpected DLL dependencies: {sorted(unexpected)}")


def cmd_verify_static(preset: str) -> None:
    binary = daemon_binary(preset)
    if not binary.exists():
        sys.exit(f"error: {binary} not found (run `just build {preset}` first)")

    # Source-level: runs on every OS, independent of how this binary links.
    hits = nss_calls(DAEMON_C_DIRS)
    if hits:
        sys.exit(
            "error: daemon sources call NSS-backed functions a static binary "
            "cannot resolve:\n  " + "\n  ".join(hits)
        )

    if IS_WINDOWS:
        _verify_static_windows(binary)
    else:
        _verify_static_linux(binary)
    print("static link verified")


# ---------------------------------------------------------------------------
# release-bundle
# ---------------------------------------------------------------------------
RELEASE_ARTIFACTS = {
    "linux-musl-release": "taz-linux-x64",
    "windows-release": "taz-windows-x64",
}


def cmd_release_bundle(preset: str) -> None:
    artifact = RELEASE_ARTIFACTS.get(preset)
    if artifact is None:
        sys.exit(
            f"error: {preset} is not a release preset "
            f"(expected one of {sorted(RELEASE_ARTIFACTS)})"
        )

    binary = daemon_binary(preset)
    if not binary.exists():
        sys.exit(f"error: {binary} not found (run `just build {preset}` first)")

    out_dir = ROOT / "out" / "release" / artifact
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    shutil.copy2(binary, out_dir / binary.name)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    # Bytes, not text: on Windows text mode would write CRLF, which older
    # `sha256sum -c` (coreutils 8.x) rejects.
    (out_dir / f"{binary.name}.sha256").write_bytes(
        f"{digest}  {binary.name}\n".encode()
    )
    shutil.copy2(ROOT / "LICENSE", out_dir / "LICENSE")
    shutil.copy2(ROOT / "THIRD_PARTY_NOTICES", out_dir / "THIRD_PARTY_NOTICES")
    print(f"wrote {out_dir.relative_to(ROOT)}")


# ---------------------------------------------------------------------------
def main(argv: Sequence[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="dev.py", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("cmake", help="run cmake ARGS... (handled before argparse)")
    sub.add_parser("ctest", help="run ctest ARGS... (handled before argparse)")
    sub.add_parser("conan-profiles")
    p = sub.add_parser("musl-toolchain")
    p.add_argument("--print", dest="print_only", action="store_true")
    p.add_argument("--print-key", dest="print_key", action="store_true")
    p = sub.add_parser("deps")
    p.add_argument("preset")
    p = sub.add_parser("lock-deps")
    p.add_argument("preset")
    p = sub.add_parser("configure")
    p.add_argument("preset")
    p = sub.add_parser("conan-cache-key")
    p.add_argument("preset")
    p = sub.add_parser("conan-cache-trim")
    p.add_argument("preset", nargs="?", default="linux-debug")
    p = sub.add_parser("proto")
    p.add_argument("--check", action="store_true")
    p = sub.add_parser("clang-format")
    p.add_argument("--check", action="store_true")
    p = sub.add_parser("clang-tidy")
    p.add_argument("--build-dir", type=Path, required=True)
    sub.add_parser("cppcheck")
    p = sub.add_parser("cmake-format")
    p.add_argument("--check", action="store_true")
    sub.add_parser("proto-breaking")
    p = sub.add_parser("valgrind")
    p.add_argument("--build-dir", type=Path, required=True)
    p = sub.add_parser("valgrind-py")
    p.add_argument("--daemon", type=Path, required=True)
    p = sub.add_parser("coverage")
    p.add_argument("--build-dir", type=Path, required=True)
    p = sub.add_parser("fuzz")
    p.add_argument("--build-dir", type=Path, required=True)
    p.add_argument("--seconds", type=int, default=60)
    sub.add_parser("doctor")
    sub.add_parser("clean")
    p = sub.add_parser("verify-static")
    p.add_argument("preset")
    p = sub.add_parser("release-bundle")
    p.add_argument("preset")

    raw = list(sys.argv[1:] if argv is None else argv)
    # `cmake` forwards everything verbatim; argparse's REMAINDER would swallow
    # leading options such as `-S`, so dispatch it before parsing.
    if raw and raw[0] in {"cmake", "ctest"}:
        cmd_passthrough(raw[0], raw[1:])
        return

    args = parser.parse_args(raw)
    match args.command:
        case "conan-profiles":
            cmd_conan_profiles()
        case "musl-toolchain":
            cmd_musl_toolchain(print_only=args.print_only, print_key=args.print_key)
        case "deps":
            cmd_deps(args.preset)
        case "lock-deps":
            cmd_lock_deps(args.preset)
        case "configure":
            cmd_configure(args.preset)
        case "conan-cache-key":
            cmd_conan_cache_key(args.preset)
        case "conan-cache-trim":
            cmd_conan_cache_trim(args.preset)
        case "proto":
            cmd_proto(args.check)
        case "clang-format":
            cmd_clang_format(args.check)
        case "clang-tidy":
            cmd_clang_tidy(args.build_dir.resolve())
        case "cppcheck":
            cmd_cppcheck()
        case "cmake-format":
            cmd_cmake_format(args.check)
        case "proto-breaking":
            cmd_proto_breaking()
        case "valgrind":
            cmd_valgrind(args.build_dir.resolve())
        case "valgrind-py":
            cmd_valgrind_py(args.daemon.resolve())
        case "coverage":
            cmd_coverage(args.build_dir.resolve())
        case "fuzz":
            cmd_fuzz(args.build_dir.resolve(), args.seconds)
        case "doctor":
            cmd_doctor()
        case "clean":
            cmd_clean()
        case "verify-static":
            cmd_verify_static(args.preset)
        case "release-bundle":
            cmd_release_bundle(args.preset)


if __name__ == "__main__":
    main()
