<p align="center">
  <img src="docs/assets/taz-logo.png" alt="TAZ logo" width="200">
</p>

# Test Agent Zero, the Tasmanian daemonfish

TAZ is a remote test daemon whose primary purpose is to facilitate tests running on remote hosts.

It can be used for all three stages of a test run on a host:

- Before the test : Prepare the host for the test
- During the test : Monitor the host and processes on the host
- After the test : Validate the state of the host

TAZ is defined as a protocol and API first; this repository holds the
specification and the reference implementations.

| Directory | Contents                                                              |
| --------- | --------------------------------------------------------------------- |
| `docs/`   | Requirements, wire protocol, and API specification                    |
| `rpc/`    | Protocol Buffers schemas (the contract) and nanopb size limits        |
| `daemon/` | Reference daemon in C (libuv + nanopb), CMake + Conan build           |
| `c3/`     | Reference client in Python (`import taz`, PyPI `test-agent-zero`)     |
| `tests/`  | Integration tests that launch the daemon and drive it with the client |
| `tools/`  | Cross-platform helpers behind the `just` recipes                      |

## Getting started

Tool versions are pinned with [mise](https://mise.jdx.dev) and every task is a
[`just`](https://just.systems) recipe. On a fresh clone:

```sh
mise install     # pinned tools: uv, just, cmake, ninja, buf, prettier, prek, cppcheck
just setup       # Python env (incl. conan), git submodules, Conan profiles, pre-push hook
just doctor      # reports anything mise cannot install (C compiler; cppcheck on Windows)
```

Then:

```sh
just build       # conan install + cmake --preset, then build the daemon
just test        # C unit tests (GoogleTest) + Python tests (pytest)
just lint        # every formatter check, clang-tidy, cppcheck, ruff, mypy, buf
just fmt         # apply all formatters
just proto       # regenerate protobuf code after editing rpc/
just --list      # everything else
```

`just lint` is exactly what CI and the pre-push hook run, so a red CI job
reproduces locally with the command shown in its log.

The build produces the daemon binary `tazd`; the Python package installs the
`taz` client CLI.

### Requirements not managed by mise

- A C compiler: GCC or Clang on Linux; on Windows, Visual Studio Build Tools
  with the C++ workload and the "C++ Clang tools for Windows" component
  (provides `clang-format`/`clang-tidy` alongside the PyPI wheels used on
  other platforms).
- On Windows, `cppcheck` at the version pinned in `mise.toml`, from the
  official installer. mise provides it on Linux and macOS, but conda-forge's
  Windows build cannot find its configuration files. `just doctor` checks
  the version.

## Hooks

This repo uses [`prek`](https://github.com/j178/prek) (a pre-commit compatible
runner) as a pre-push gate. `just setup` installs it; the hook runs
`just proto-check` and `just lint`, plus basic file-hygiene checks, on every
`git push`. If any check fails the push is aborted; fix the reported issues
and push again.

## Layout of the generated code

Both protobuf outputs are checked in and regenerated with `just proto`:
Python to `c3/src/taz/v1/`, C (nanopb) to `daemon/generated/`. CI fails if
regeneration would produce a diff. See [`rpc/README.md`](rpc/README.md).

## Planning artifacts

The implementation roadmap lives under `.agents/planning/`: requirements
clarification, research notes, the detailed design, and the step-by-step
implementation plan.
