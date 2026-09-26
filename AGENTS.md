# AGENTS.md

## Project

Test Agent Zero (TAZ) is a remote test daemon for running tests on remote
hosts. Daemon binary `tazd`, client CLI `taz`, Python import `taz`, PyPI
distribution `test-agent-zero`, identifier prefix `TAZ_`/`taz_`. Protocol-first:
the wire spec and protobuf schemas (`rpc/`) are the contract; the C daemon
(`daemon/`) and Python client (`c3/`) are reference implementations.

## Layout

| Path           | What                                                |
| -------------- | --------------------------------------------------- |
| `docs/`        | Requirements, wire protocol, API spec               |
| `rpc/`         | Protobuf schemas (source of truth for the protocol) |
| `daemon/`      | C daemon (libuv + nanopb), CMake + Conan build      |
| `c3/`          | Python client package (`import taz`)                |
| `tests/`       | Integration tests (daemon + client)                 |
| `tools/dev.py` | Cross-platform helpers behind `just` recipes        |
| `justfile`     | Every developer/CI task                             |

## Build & test

Prerequisites: `mise install && just setup && just doctor`.

```sh
just build          # conan install + cmake configure + build
just test           # C unit tests (GoogleTest) + Python tests (pytest)
just lint           # all formatters, clang-tidy, cppcheck, ruff, mypy, buf
just fmt            # apply all formatters
just proto          # regenerate protobuf code after editing rpc/
just --list         # everything else
```

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
- `just test-py` — pytest (extra args forwarded)
- `just test-sanitizers` — ASan+UBSan, TSan (Linux); ASan (Windows)
- `just test-valgrind` — Valgrind memcheck
- `just analyze` — compiler static analyzer (GCC -fanalyzer / MSVC /analyze)
- `just coverage` — gcov + pytest-cov, reports under `out/coverage/`

## CI

GitHub Actions (`.github/workflows/ci.yml`). Matrix: Linux GCC, Linux Clang,
Windows MSVC; plus per-check jobs for ASan, TSan, Valgrind, coverage, and
static analysis. Every step is a `just` recipe.

## Do not

- Add dependency-license CI checks (declined by project owner).
- Write "step #" or roadmap commentary in source code comments; those belong
  in `docs/` or `.agents/planning/`.
