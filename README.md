# TAZER — Test Daemon for Remote Hosts

TAZER is a remote test daemon whose primary purpose is to facilitate tests running on remote hosts.

It can be used for all three stages of a test run on a host:

- Before the test : Prepare the host for the test
- During the test : Monitor the host and processes on the host
- After the test : Validate the state of the host

## Hooks

This repo uses [`prek`](https://github.com/j178/prek) (a pre-commit compatible runner) to lint the source before it is pushed to a remote. `prek` is pinned in [`mise.toml`](mise.toml) and the hook configuration lives in [`.pre-commit-config.yaml`](.pre-commit-config.yaml).

On a fresh clone, run once:

```sh
mise install && prek install --hook-type pre-push
```

`mise install` provisions the pinned `prek` binary. `prek install --hook-type pre-push` writes `.git/hooks/pre-push`, which invokes `prek run --hook-stage pre-push` against `.pre-commit-config.yaml` (ruff lint + format check on Python, plus core file-hygiene hooks) on every `git push`.

If any hook fails, `git push` is aborted with a non-zero exit. Fix the reported issues locally, re-stage, and push again — there is no auto-fix in the gate.
