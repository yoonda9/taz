# AGENTS.md

## Project

Test Agent Zero (TAZ) is a remote test daemon for running tests on remote
hosts. Daemon binary `tazd`, client CLI `taz`, Python import `taz`, PyPI
distribution `test-agent-zero`, identifier prefix `TAZ_`/`taz_`. Protocol-first:
the wire spec and protobuf schemas (`rpc/`) are the contract; the C daemon
(`daemon/`) and Python client (`c3/`) are reference implementations.

## Layout

| Path                  | What                                                |
| --------------------- | --------------------------------------------------- |
| `THIRD_PARTY_NOTICES` | Licence texts shipped with the release binaries     |
| `docs/`               | Requirements, wire protocol, API spec               |
| `rpc/`                | Protobuf schemas (source of truth for the protocol) |
| `daemon/`             | C daemon (libuv + nanopb), CMake + Conan build      |
| `c3/`                 | Python client package (`import taz`)                |
| `conftest.py`         | pytest options shared by every test directory       |
| `tests/`              | Integration tests (daemon + client), dev-tool tests |
| `typings/`            | Local type stubs for untyped dependencies (mypy)    |
| `tools/dev.py`        | Cross-platform helpers behind `just` recipes        |
| `tools/win.py`        | Disposable Windows VM on Proxmox (`just win*`)      |
| `tools/winbuild.py`   | Rebuilds the Windows templates (`tools/winimage/`)  |
| `justfile`            | Every developer/CI task                             |

## Build & test

Prerequisites: `mise install && just setup && just doctor`.

```sh
just build          # conan install + cmake configure + build
just test           # C unit tests (GoogleTest) + Python tests (pytest)
just lint           # all formatters, clang-tidy, cppcheck, ruff, mypy, buf
just fmt            # apply all formatters
just proto          # regenerate protobuf code after editing rpc/
just win test       # push HEAD to a Windows VM clone, run `just test` there
just verify-static  # build the release preset, prove the binary is fully static
just release-bundle # build the release preset, write out/release/<artifact>
just --list         # everything else
```

`just win*` needs the Proxmox token and node in `.env` (see `tools/win.py`). `just
win-up` clones the TAZ Windows template (a warm `C:\taz`) into VM 9200;
`just win-reset` and `just win-down` destroy only a clone tagged `win-clone`.
`just win-rebuild` reinstalls both templates from ISO, e.g. before the
Windows eval licence expires; `just win-rebuild layer` refreshes only the
TAZ layer.

## Key conventions

- **`just` is the single entry point.** Every task is a recipe. CI and the
  pre-push hook run the same recipes, so a red CI job reproduces locally.
- **Protobuf is checked in.** Python stubs go to `c3/src/taz/v1/`, C (nanopb)
  to `daemon/generated/`. Run `just proto` after editing `rpc/`; CI fails if
  the generated code is stale (`just proto-check`).
- **Daemon is pure C11.** GoogleTest (C++) is used only for unit tests.
  C++ is enabled in CMake only when `TAZ_BUILD_TESTS=ON`.
- **nanopb is a git submodule** at `daemon/third_party/nanopb`, pinned to the
  same release as the generator used by `just proto`.
- **Conan manages C dependencies** (libuv, gtest). Profiles live in
  `daemon/profiles/`; CMake presets in `daemon/CMakePresets.json`.
- **`linux-musl-release` is isolated from every other Linux preset**: a
  complete host profile (`daemon/profiles/linux-musl`, not composed after
  `default`) pointed at the Bootlin musl toolchain, and its own Conan home
  (`tools/dev.py`'s `conan_env`) so its from-source packages never collide
  with the glibc ones in `~/.conan2`. Both live under `TAZ_CACHE_DIR`
  (default `~/.cache/taz`, never the repo or `/tmp`); `just musl-toolchain`
  fetches and verifies the toolchain, and `just deps` does so on demand.
- **Python tooling:** uv for env/deps, ruff for lint+format, mypy --strict.

## Linting & formatting

| Language      | Formatter                       | Linter                                      |
| ------------- | ------------------------------- | ------------------------------------------- |
| C             | clang-format (`just fmt-c`)     | clang-tidy + cppcheck (`just lint-c`)       |
| C++ (tests)   | clang-format                    | clang-tidy                                  |
| Python        | ruff format (`just fmt-py`)     | ruff check + mypy (`just lint-py`)          |
| Protobuf      | buf format (`just fmt-proto`)   | buf lint + buf breaking (`just lint-proto`) |
| CMake         | cmake-format (`just fmt-cmake`) | —                                           |
| Markdown/YAML | prettier (`just fmt-docs`)      | —                                           |

## Testing matrix

- `just test-c` — GoogleTest via ctest
- `just test-py` — pytest (extra args forwarded); `--daemon PATH` for a binary
  inside the repo, `--daemon=PATH` or `TAZ_DAEMON=PATH` for one elsewhere
- `just test-sanitizers` — ASan+UBSan, TSan (Linux); ASan (Windows)
- `just test-valgrind` — Valgrind memcheck
- `just test-py-valgrind` — the marked Valgrind integration subset (Linux only)
- `just test-py-system` — Windows only: the RUN_AS integration rows (`-m system`)
  with pytest running as SYSTEM through a one-shot scheduled task; creates the
  local account `taz-runas` (random password, never stored) and leaves it in
  place; a privileged row that skips fails the lane under `CI=1`
- `just analyze` — compiler static analyzer (GCC -fanalyzer / MSVC /analyze)
- `just coverage` — gcov + pytest-cov, reports under `out/coverage/`
- `just test-c linux-musl-release` — the C unit tests built against musl
  (Bootlin toolchain, own Conan home); `just test-py --daemon
daemon/build/linux-musl-release/tazd` runs the Python suite against the
  static binary
- `just verify-static [preset]` — builds the preset (default: the musl
  release on Linux, `windows-release` on Windows) and proves its binary is
  fully static: no `INTERP`/`NEEDED` (readelf) or unexpected DLLs (dumpbin),
  no NSS calls in daemon sources, and on Linux a `FROM scratch` docker smoke
  run of `tazd --version`. Fails by design on a glibc preset such as
  `linux-release`.

## CI

GitHub Actions (`.github/workflows/ci.yml`). One job per toolchain: Linux GCC
(build, test, lint, ASan, TSan, Valgrind, static analysis, coverage), Linux
Clang, Linux release (musl static `tazd`), Windows MSVC (build, test, lint),
Windows ASan + static analysis, and Windows release (static `tazd.exe`).
The Windows MSVC job also runs `just test-py-system`, the RUN_AS rows as
SYSTEM (CI sets `CI=1`, so a privileged row that skips fails it).
Every check is a `just` recipe and runs even when an earlier one fails. The
Conan package cache is restored only on an exact key match
(`tools/dev.py conan-cache-key`); the musl entry caches its own Conan home
(`~/.cache/taz/conan-musl/p`) and the Bootlin toolchain
(`~/.cache/taz/toolchains`, keyed on `musl-toolchain --print-key`) separately
from the glibc presets' `~/.conan2/p` (the job sets `TAZ_CACHE_DIR` to match).
Each release entry builds its preset, runs the full test suite against the
static binary and `just verify-static`; only if all of them pass does `just
release-bundle` run and its result get uploaded as a workflow artifact
(`taz-linux-x64` / `taz-windows-x64`) on every push — no GitHub Release is
created.

## Pinned versions

Every version is exact and bumped by hand (no update bot). Where each lives:

| What                          | Where                                                         | Bump                                     |
| ----------------------------- | ------------------------------------------------------------- | ---------------------------------------- |
| CLI tools (uv, cppcheck…)     | `mise.toml`                                                   | edit the version                         |
| mise itself (CI)              | `ci.yml` `MISE_VERSION`                                       | edit the version                         |
| Python                        | `.python-version`                                             | edit the version                         |
| Python packages (incl. conan) | `uv.lock`                                                     | `uv lock --upgrade-package NAME`         |
| Build backend for `c3`        | `[tool.uv] build-constraint-dependencies` in `pyproject.toml` | edit, then `uv lock`                     |
| C dependencies                | `daemon/conanfile.py` + `daemon/conan.lock`                   | edit versions, then `just lock-deps`     |
| GitHub Actions                | `ci.yml` `uses:` commit SHA + `# vX.Y.Z`                      | replace both                             |
| cppcheck on Windows           | `ci.yml` `CPPCHECK_VERSION` + `CPPCHECK_SHA256`               | same version as `mise.toml`              |
| nanopb                        | submodule commit                                              | check out a tag in it, then `just proto` |
| musl toolchain (Bootlin)      | `tools/dev.py` `MUSL_TOOLCHAINS[<arch>]` `name` + `sha256`    | edit each arch's row (see below)         |

A musl toolchain bump also updates `compiler.version` in all three
`daemon/profiles/linux-musl{,-i686,-aarch64}` profiles and the musl section of
`THIRD_PARTY_NOTICES`.

Not pinned by the repo: runner images and their apt packages (Ubuntu 24.04,
Windows Server 2025; compilers, clang, valgrind on Linux), the dependencies
of the pre-commit hook repos, cppcheck's conda-forge dependencies (resolved
when mise installs it), and each developer's mise. Also not pinned: docker
(used only by `just verify-static`'s smoke run) and the host's
`binutils`/`readelf`; the musl toolchain's own binutils is pinned through its
tarball hash.

## Do not

- Add dependency-license CI checks (declined by project owner).
- Write "step #" or roadmap commentary in source code comments; those belong
  in `docs/` or in local, untracked planning notes.
