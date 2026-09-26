# taz (c3) — Python client for Test Agent Zero

Reference client for the TAZ remote test daemon (`pip install test-agent-zero`,
`import taz`; installs the `taz` CLI). Speaks the TAZ binary
protocol over TCP (see `docs/protocol.md`) with payloads generated from the
schemas in `rpc/`.

```python
from taz import TazClient

with TazClient("10.0.0.1", 5555) as t:
    print(t.version())
    result = t.command.exec("ls", args=["-la", "/tmp"])
```

Layout:

- `src/taz/v1/` — generated protobuf modules (`just proto`; do not edit)
- `src/taz/c3/` — protocol framing, connection, and namespaced client API
- `src/taz/cli/` — command-line entry point
- `tests/` — unit tests that need no daemon (`just test-py`)

Integration tests that launch a daemon live in the repository-level `tests/`
directory.
