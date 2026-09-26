# rpc — TAZER protocol schemas

Protocol Buffers schemas for every TAZER message. These files are the
contract: any daemon or client implementation, in any language, is built from
them. The wire framing around these payloads is defined in
[`docs/protocol.md`](../docs/protocol.md); field semantics are in
[`docs/api.md`](../docs/api.md).

```
rpc/
├── buf.yaml                  # buf lint / breaking-change config (module root)
└── tazer/v1/
    ├── common.proto          # Opcode, FrameType, FrameFlag, ErrorCode, ErrorInfo, KeyValue, ...
    ├── daemon_control.proto  # Version, Capability, Configuration, Restart
    ├── command.proto         # CommandExec
    ├── process.proto         # ProcessList / Kill / Info / Monitor
    ├── file.proto            # FilePut / FileGet / Create / Delete / Stat / Chmod / Dir*
    ├── advanced.proto        # RunAs, TimeoutSet, Log, Detach, TaskStatus, TaskCancel, Cancel
    └── *.options             # nanopb size limits (reference C daemon only)
```

## Conventions

- Package `tazer.v1`; one file per API group.
- Maps are modeled as `repeated KeyValue` / `repeated UInt32Pair`, which is
  wire-identical to protobuf `map<>` and keeps the schemas usable from nanopb.
- The `.proto` files carry no nanopb annotations. Size limits for the C
  daemon's static allocation live in sidecar `.options` files with the same
  base name, so other implementers never need `nanopb.proto`.

## Generated code

Both generated outputs are checked in and regenerated with `just proto`:

| Target        | Output                                       | Generator                                                       |
| ------------- | -------------------------------------------- | --------------------------------------------------------------- |
| Python client | `c3/src/tazer/v1/*_pb2.py`, `*_pb2.pyi`      | `protoc` from `grpcio-tools`                                    |
| C daemon      | `daemon/generated/tazer/v1/*.pb.c`, `*.pb.h` | nanopb generator from the `daemon/third_party/nanopb` submodule |

CI runs `just proto-check` and fails if regeneration produces a diff.

Lint and format with `just lint-proto` / `just fmt-proto` (both wrap `buf`).
