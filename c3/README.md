# tazer (c3) — Python client for TAZER

Reference client for the TAZER remote test daemon. Speaks the TAZER binary
protocol over TCP (see `docs/protocol.md`) with payloads generated from the
schemas in `rpc/`.

```python
from tazer import TazerClient

with TazerClient("10.0.0.1", 5555) as t:
    print(t.version())
    result = t.command.exec("ls", args=["-la", "/tmp"])
```

Layout:

- `src/tazer/v1/` — generated protobuf modules (`just proto`; do not edit)
- `src/tazer/c3/` — protocol framing, connection, and namespaced client API
- `src/tazer/cli/` — command-line entry point
- `tests/` — unit tests that need no daemon (`just test-py`)

Integration tests that launch a daemon live in the repository-level `tests/`
directory.
