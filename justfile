# TAZ task runner.
#
# Bootstrap on a fresh clone:   mise install && just setup
# Everyday:                     just build | just test | just lint | just fmt
#
# mise owns tool versions (mise.toml); this file owns every task. prek hooks
# and CI call these same recipes, so a red CI job reproduces locally with the
# command shown in its log. Anything needing globbing or per-OS branching is
# in tools/dev.py so recipe bodies read the same under bash and PowerShell.

set windows-shell := ["powershell.exe", "-NoLogo", "-NoProfile", "-Command"]

preset := if os() == "windows" { "windows-debug" } else { "linux-debug" }
dev := "uv run python tools/dev.py"

# List recipes
[private]
default:
    @just --list --unsorted

# --- setup ------------------------------------------------------------------

# One-time setup after `mise install`: Python env, submodules, Conan profiles, hooks
setup:
    uv sync --group dev
    git submodule update --init --recursive
    {{ dev }} conan-profiles
    prek install --hook-type pre-push

# Report missing tools (system compiler, cppcheck, unpinned mise/uv tools)
doctor:
    {{ dev }} doctor

# --- protobuf ---------------------------------------------------------------

# Regenerate protobuf code: Python -> c3/src/taz/v1, C -> daemon/generated
proto:
    {{ dev }} proto

# Fail if `just proto` would change the checked-in generated code
proto-check:
    {{ dev }} proto --check

# --- daemon build -----------------------------------------------------------

# Install the preset's Conan dependencies (the first half of `configure`)
deps preset=preset:
    {{ dev }} deps {{ preset }}

# Re-resolve daemon/conan.lock to the newest Conan recipe revisions (a manual bump)
lock-deps preset=preset:
    {{ dev }} lock-deps {{ preset }}

# Configure the daemon: `conan install` with the preset's host profiles, then
# `cmake --preset`. On Windows the MSVC environment is loaded first if needed.
configure preset=preset:
    {{ dev }} configure {{ preset }}

# Build the daemon and its unit tests
build preset=preset: (configure preset)
    {{ dev }} cmake --build daemon/build/{{ preset }}

# Remove daemon build directories
clean:
    {{ dev }} clean

# Check that a built daemon binary has no unexpected dynamic dependencies
verify-static preset="linux-release":
    {{ dev }} verify-static daemon/build/{{ preset }}/tazd

# --- tests ------------------------------------------------------------------

# Run C unit tests (GoogleTest via ctest). Goes through dev.py so MSVC ASan
# builds find the sanitizer runtime DLL in the MSVC bin directory.
test-c preset=preset: (build preset)
    {{ dev }} ctest --test-dir daemon/build/{{ preset }} --output-on-failure

# Run Python unit + integration tests (extra args go to pytest)
test-py *args:
    uv run pytest {{ args }}

# Run all tests
test: test-c test-py

# Run the C unit tests under every sanitizer this OS supports
[unix]
test-sanitizers: (test-c "linux-asan") (test-c "linux-tsan")

# Run the C unit tests under MSVC AddressSanitizer (no TSan/UBSan on MSVC)
[windows]
test-sanitizers: (test-c "windows-asan")

# Run the C unit tests under Valgrind memcheck (plain debug build, not ASan)
test-valgrind preset=preset: (build preset)
    {{ dev }} valgrind --build-dir daemon/build/{{ preset }}

# Build with the compiler's static analyzer (GCC -fanalyzer); findings are errors
[unix]
analyze: (build "linux-analyze")

# Build with MSVC /analyze; findings are errors
[windows]
analyze: (build "windows-analyze")

# Coverage: gcov-instrumented C tests + pytest-cov, reports under out/coverage
[unix]
coverage: (test-c "linux-coverage")
    {{ dev }} coverage --build-dir daemon/build/linux-coverage
    uv run pytest --cov=taz --cov-report=term --cov-report=html:out/coverage/python --cov-report=xml:out/coverage/python.xml

# --- formatting -------------------------------------------------------------

# Format everything
fmt: fmt-c fmt-py fmt-proto fmt-cmake fmt-docs

# Verify formatting without changing files
fmt-check: fmt-check-c fmt-check-py fmt-check-proto fmt-check-cmake fmt-check-docs

fmt-c:
    {{ dev }} clang-format

fmt-check-c:
    {{ dev }} clang-format --check

fmt-py:
    uv run ruff format

fmt-check-py:
    uv run ruff format --check

fmt-proto:
    buf format -w rpc

fmt-check-proto:
    buf format -d --exit-code rpc

fmt-cmake:
    {{ dev }} cmake-format

fmt-check-cmake:
    {{ dev }} cmake-format --check

fmt-docs:
    prettier --write .

fmt-check-docs:
    prettier --check .

# --- linting ----------------------------------------------------------------

# Run every linter, formatter check and the generated-code freshness check
# (exactly what CI and the pre-push hook run)
lint: fmt-check lint-c lint-py lint-proto proto-check

# clang-tidy + cppcheck over daemon/src and daemon/include
lint-c preset=preset: (configure preset)
    {{ dev }} clang-tidy --build-dir daemon/build/{{ preset }}
    {{ dev }} cppcheck

# ruff + mypy --strict
lint-py:
    uv run ruff check
    uv run mypy

# buf lint, then buf breaking against main (skipped when main has no schemas yet)
lint-proto:
    buf lint rpc
    {{ dev }} proto-breaking
