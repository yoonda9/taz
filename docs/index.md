# TAZ Design

TAZ is intended to be a well defined API such that both the daemon and client can be implemented in any language.

The TAZ reference daemon is written in C in order to be able to support _any_ test host without requiring redesigning the API or communication protocol.

For detailed requirements see [Requirements](requirements.md). For the wire protocol specification see [Protocol Design](protocol.md).

## Daemon Characteristics and Requirements

- Must be able to be ported to run on most platforms from the last decade.
- Must be stable and reliable
- Must not have runtime dependencies aside from the platform's C standard library, C runtime, and [libuv](https://libuv.org/) (MIT-licensed cross-platform async I/O).
- Handle asynchronous tasking
- All dependencies must be permissively licensed (MIT, BSD, Apache-2.0)

## Client-Daemon Communication

The client (c3) communicates with the daemon via a custom binary framing protocol over TCP.

### Communication Protocol

- Custom binary framing with a fixed 12-byte header (type, flags, opcode, length, stream_id) — see [Protocol Design](protocol.md)
- Persistent TCP connections with request-response correlation via a header-level `stream_id`
- Designed for implementation on both full-OS platforms (via libuv, in the reference daemon only — libuv is not mandated by the protocol) and constrained environments (RTOS, bare-metal) as separate implementations sharing the same wire format
- Capability handshake at connection time — the daemon advertises its supported opcodes so the client adapts dynamically

### Message Format / Serialization

Protocol Buffers are used for the RPC message format.

- Widely used with popular C library available (nanopb) for embedded/constrained targets
- Supports many languages including C and Python
- Provides consistency and single source of truth for procedure definitions by requiring schemas
  - Allows backwards/forwards compatibility and version control thereof
- `.proto` schemas define `max_size` constraints per message type, enabling static buffer allocation on embedded targets

### Tasking

#### Concurrency

There should be _no_ requirement for the client to handle concurrency. Requiring any form of concurrency may be detrimental for test writers to create simple and easily understandable tests.
This is not to say that concurrency should not be a feature on the client side, but simply that _all_ functionality — with the sole exception of the Interactive Shell — needs to be able to be exercised without concurrency enabled.

The daemon _may_ receive asynchronous tasking even though the client does _not_ handle asynchronous operations.
For example, the daemon may need to start a process and monitor its status while also being tasked to upload a file.
In terms of a synchronous test framework, the client should be able to mark each task as able to raise failures globally.
If so, a failure caught in task a, can send an error response back on the channel reserved for the response for task b.

```python
some_proc = taz.run(
    cmd="some-process", detach=True, monitor=True, error_priority="critical"
)
try:
    file = taz.get(src="some-path.txt", dest="/tmp/some-path.txt")
except Exception as err:
    ...
```

#### Task Pipelines

Task pipelines can be used to define a chain of tasks which take the output of the previous task as the input to its task such that the daemon only has to report back to the client upon error of a task or successful completion of all tasks therein.
