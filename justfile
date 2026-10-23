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
release := if os() == "windows" { "windows-release" } else { "linux-musl-release" }
dev := "uv run python tools/dev.py"
win := "uv run python tools/win.py"

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

# Fetch (or verify the cache of) the pinned Bootlin musl toolchain for ARCH
# (default x86_64, used by linux-musl-release); a no-op once it is cached
[unix]
musl-toolchain arch="x86_64":
    {{ dev }} musl-toolchain {{ arch }}

# Develop the libuv patch series: `import` (default) rebuilds the scratch git tree in
# out/libuv-patches/ from the recipe; `export` writes its commits back to the recipe
libuv-patches mode="import" *flags:
    {{ dev }} libuv-patches {{ mode }} {{ flags }}

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

# Build preset, then prove its binary is fully static: no dynamic deps
# (readelf on Linux, dumpbin on Windows), no NSS calls in daemon sources, and
# (Linux) runs from a FROM-scratch image. `just verify-static linux-release`
# fails by design: that preset still links glibc dynamically.
verify-static preset=release: (build preset)
    {{ dev }} verify-static {{ preset }}

# Build preset, then copy its binary, a matching .sha256, LICENSE and
# THIRD_PARTY_NOTICES into out/release/<artifact> (wiping any stale copy).
# Fails on a non-release preset.
release-bundle preset=release: (build preset)
    {{ dev }} release-bundle {{ preset }}

# Check the sha256 digest already shipped in out/release/<artifact> matches
# the binary and the shipped LICENSE/THIRD_PARTY_NOTICES match the repo's —
# catches a stale or truncated copy before it's uploaded as a CI artifact.
# Does NOT depend on `release-bundle`: CI runs that as its own prior step, and
# re-bundling here would just re-verify a fresh copy instead of the one that
# step actually produced. Run `just release-bundle preset` first.
verify-release-bundle preset=release:
    {{ dev }} verify-release-bundle {{ preset }}

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

# Run the marked Valgrind integration subset (pytest)
[unix]
test-py-valgrind preset=preset: (build preset)
    {{ dev }} valgrind-py --daemon daemon/build/{{ preset }}/tazd

# Run the RUN_AS integration rows with pytest as SYSTEM (creates the taz-runas
# test account; extra args go to pytest)
[windows]
test-py-system *args: (build preset)
    {{ dev }} test-py-system {{ args }}

# Run each libFuzzer harness from its seed corpus for [seconds] seconds
[unix]
fuzz seconds="60": (build "linux-fuzz")
    {{ dev }} fuzz --build-dir daemon/build/linux-fuzz --seconds {{ seconds }}

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

# --- Windows VM -------------------------------------------------------------
# A disposable linked clone of the TAZ Windows template on Proxmox (settings
# in .env; see tools/win.py). `just win test` reproduces CI's Windows job.

# Clone (if needed) and start the Windows VM, then wait for SSH
win-up:
    {{ win }} up

# Windows VM: up|ssh|reset|down, or RECIPE [ARGS] to push HEAD and run it there
win *args:
    {{ win }} {{ args }}

# Open a shell on the Windows VM
win-ssh:
    {{ win }} ssh

# Destroy the Windows VM and clone a fresh one
win-reset:
    {{ win }} reset

# Destroy the Windows VM
win-down:
    {{ win }} down

# Rebuild the Windows templates from ISO (`all`, ~1.5 h) or only the TAZ layer (`layer`)
[unix]
win-rebuild what="all":
    uv run python tools/winbuild.py {{ what }}
