#!/usr/bin/env python3
"""Cross-platform helpers behind the `just` recipes.

Anything that would otherwise need shell globbing, per-OS branching, or more
than one line lives here so the justfile stays a flat list of commands that
read identically on Linux, macOS and Windows.

Subcommands:
    conan-profiles       create the detected Conan base profiles (default, clang)
    deps PRESET          conan install (preset's profiles, daemon/conan.lock)
    lock-deps PRESET     re-resolve daemon/conan.lock to the newest Conan revisions
    configure PRESET     deps + cmake --preset
    conan-cache-key PRESET   CI cache key for the Conan packages a preset needs
    conan-cache-trim     drop what a restored CI package cache never uses
    cmake ARGS...        run cmake; on Windows, load the MSVC environment first
    ctest ARGS...        run ctest; same MSVC environment handling
    proto [--check]      regenerate Python + C protobuf code (or verify no diff)
    clang-format [--check]
    clang-tidy --build-dir DIR
    cppcheck
    cmake-format [--check]
    proto-breaking       buf breaking against main (skips if main has no schemas)
    valgrind --build-dir DIR   run the C unit tests under Valgrind memcheck
    coverage --build-dir DIR   gcovr report for a coverage-instrumented build
    doctor               report tools mise cannot provide / missing pinned tools
    clean                remove daemon build directories
    verify-static BINARY check a built daemon has no unexpected dynamic deps
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import tomllib
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
def run(cmd: Sequence[str | Path], *, cwd: Path = ROOT, check: bool = True) -> int:
    printable = " ".join(str(c) for c in cmd)
    print(f"$ {printable}", flush=True)
    result = subprocess.run([str(c) for c in cmd], cwd=cwd, check=False)
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


def host_profiles_for(preset: str) -> list[str]:
    """Host profile chain for a preset: detected base first, then our partials."""
    if preset.startswith("windows-"):
        return ["default", str(PROFILES_DIR / "windows-static")]
    if preset.startswith("linux-fuzz") or preset.startswith("linux-clang"):
        return [
            "clang",
            str(PROFILES_DIR / "linux-static"),
            str(PROFILES_DIR / "linux-clang"),
        ]
    return ["default", str(PROFILES_DIR / "linux-static")]


def conan_profile_args(preset: str) -> list[str]:
    """Conan arguments selecting a preset's settings and host profile chain."""
    build_type = "Release" if preset.endswith("-release") else "Debug"
    args = ["-s", f"build_type={build_type}"]
    for profile in host_profiles_for(preset):
        args += ["--profile:host", profile]
    return args


def cmd_deps(preset: str) -> None:
    ensure_msvc_env()
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
        ]
    )


def cmd_lock_deps(preset: str) -> None:
    """Rewrite daemon/conan.lock with the newest revisions on the remotes.

    The recipes' requirements do not depend on the platform, so any one
    preset's profiles resolve the graph for every preset.
    """
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
        ]
    )


def cmd_configure(preset: str) -> None:
    cmd_deps(preset)
    run([tool("cmake"), "-S", ROOT / "daemon", "--preset", preset])


def cmd_conan_cache_key(preset: str) -> None:
    """Print the CI cache key for the Conan packages a preset needs.

    The digest covers everything that decides those binaries: the Conan
    version, the resolved host and build profiles (so a runner image with a
    new compiler starts a fresh cache), conanfile.py and conan.lock. CI
    restores exact matches only: package ids ignore [conf] such as
    compiler_executables, so a cache filled under other profiles can satisfy
    `conan install` with binaries the current profiles would not produce.
    """
    conan = tool("conan")
    version = subprocess.run(
        [conan, "--version"], capture_output=True, text=True, check=True
    ).stdout
    profiles = subprocess.run(
        [conan, "profile", "show", *conan_profile_args(preset), "--format=json"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    digest = hashlib.sha256()
    for part in (
        version,
        profiles,
        (ROOT / "daemon" / "conanfile.py").read_text(),
        CONAN_LOCKFILE.read_text(),
    ):
        digest.update(part.encode())
    host = json.loads(profiles)["host"]["settings"]
    fields = ("os", "compiler", "compiler.version", "build_type")
    print("-".join(["conan", *(host[f] for f in fields), digest.hexdigest()[:16]]))


def cmd_conan_cache_trim() -> None:
    """Shrink the Conan package cache before CI saves it.

    Build, source and download folders are never reused, and the CMake tool
    package (about 200 MB) is only needed to build libuv from source, which a
    restored cache never does. Its recipe stays so the graph still resolves
    offline.
    """
    conan = tool("conan")
    run([conan, "cache", "clean"])
    run([conan, "remove", "cmake/*:*", "--confirm"])


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
    sources = files_under(DAEMON_C_DIRS, (".c",)) + files_under(
        DAEMON_CXX_DIRS, (".cpp",)
    )
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
            "valgrind": "valgrind (apt/dnf; for `just test-valgrind`)",
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
def cmd_fuzz(build_dir: Path, seconds: int) -> None:
    """Run each libFuzzer harness from its seed corpus."""
    fuzz_dir = ROOT / "daemon" / "fuzz"
    corpus_dir = fuzz_dir / "corpus"
    harnesses = ["fuzz_reassembly", "fuzz_decode"]
    for harness in harnesses:
        binary = build_dir / "fuzz" / harness
        if not binary.exists():
            sys.exit(f"error: fuzz binary not found: {binary}")
        harness_corpus = corpus_dir / harness
        harness_corpus.mkdir(parents=True, exist_ok=True)
        print(f"$ {binary} {harness_corpus} -max_total_time={seconds}")
        rc = subprocess.run(
            [
                str(binary),
                str(harness_corpus),
                f"-max_total_time={seconds}",
                "-error_exitcode=1",
            ],
            cwd=ROOT,
        ).returncode
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


def cmd_verify_static(binary: Path) -> None:
    if not binary.exists():
        sys.exit(f"error: {binary} not found")
    if IS_WINDOWS:
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
    else:
        ldd = subprocess.run(
            ["ldd", str(binary)], capture_output=True, text=True, check=False
        )
        text = (ldd.stdout + ldd.stderr).strip()
        print(text)
        if "not a dynamic executable" not in text and "statically linked" not in text:
            sys.exit("error: binary is dynamically linked")
    print("static link verified")


# ---------------------------------------------------------------------------
def main(argv: Sequence[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="dev.py", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("cmake", help="run cmake ARGS... (handled before argparse)")
    sub.add_parser("ctest", help="run ctest ARGS... (handled before argparse)")
    sub.add_parser("conan-profiles")
    p = sub.add_parser("deps")
    p.add_argument("preset")
    p = sub.add_parser("lock-deps")
    p.add_argument("preset")
    p = sub.add_parser("configure")
    p.add_argument("preset")
    p = sub.add_parser("conan-cache-key")
    p.add_argument("preset")
    sub.add_parser("conan-cache-trim")
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
    p = sub.add_parser("coverage")
    p.add_argument("--build-dir", type=Path, required=True)
    p = sub.add_parser("fuzz")
    p.add_argument("--build-dir", type=Path, required=True)
    p.add_argument("--seconds", type=int, default=60)
    sub.add_parser("doctor")
    sub.add_parser("clean")
    p = sub.add_parser("verify-static")
    p.add_argument("binary", type=Path)

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
        case "deps":
            cmd_deps(args.preset)
        case "lock-deps":
            cmd_lock_deps(args.preset)
        case "configure":
            cmd_configure(args.preset)
        case "conan-cache-key":
            cmd_conan_cache_key(args.preset)
        case "conan-cache-trim":
            cmd_conan_cache_trim()
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
        case "coverage":
            cmd_coverage(args.build_dir.resolve())
        case "fuzz":
            cmd_fuzz(args.build_dir.resolve(), args.seconds)
        case "doctor":
            cmd_doctor()
        case "clean":
            cmd_clean()
        case "verify-static":
            cmd_verify_static(args.binary.resolve())


if __name__ == "__main__":
    main()
