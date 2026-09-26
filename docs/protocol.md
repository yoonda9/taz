# TAZER Protocol Design

> Living document. Last updated: 2026-09-25

## 1. Overview

TAZER uses a custom binary framing protocol over TCP. The protocol is designed to be:

- **Simple** — a fixed 12-byte header, no string parsing, no state machines
- **Embeddable** — implementable with static buffers and no heap allocation
- **Efficient** — minimal overhead per message, streaming support for large payloads

This document defines the wire protocol. Message payload schemas are defined separately in `.proto` files (see `rpc/`).

## 2. Architecture

```
┌──────────────────────────────────────────────────┐
│              Python Client (c3)                   │
│         speaks TAZER protocol                     │
└──────────┬───────────────────────┬───────────────┘
           │                       │
     TAZER protocol          TAZER protocol
           │                       │
┌──────────▼──────────┐  ┌────────▼────────────────┐
│   Full OS Daemon      │  │   Embedded Daemon         │
│   libuv + full API   │  │   LWIP + API subset      │
│   Linux/macOS/Win    │  │   RTOS / bare-metal      │
└─────────────────────┘  └─────────────────────────┘
```

The protocol is the portability layer. Daemons on different platforms are separate implementations that share the same wire format and message schemas.

## 3. Transport

- **TCP** over IPv4 or IPv6
- No TLS — TAZER operates in trusted test environments
- The daemon listens on a configurable port (default TBD)
- Connections are persistent — a client connects once and issues multiple requests over the same connection
- Either side may close the connection at any time; the other side must handle this gracefully

## 4. Framing

Every message on the wire is a **frame** consisting of a fixed 12-byte header followed by a variable-length payload.

### 4.1 Header Format

```
Byte:  0    1    2    3    4    5    6    7    8    9    10   11
    ┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐
    │type│flag│  opcode │        length (LE)     │   stream_id (LE)    │
    │(1B)│(1B)│  (2B)   │         (4B)           │        (4B)         │
    └────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┴────┘
```

Total header size: **12 bytes**, fixed. No variable-length header fields.

Rationale for putting `opcode` and `stream_id` in the header instead of the payload:

- Dispatch and correlation are transport concerns — the receiver decides where to route a frame before parsing the payload, and can hand `FILE_CHUNK` bytes directly to a `write()` call without wrapping them in protobuf.
- `stream_id` in the header makes concurrent file transfers, streaming responses (Process Monitor), and interactive shells trivially unambiguous. Without it, `FILE_CHUNK` frames (whose payload is raw bytes) could not be correlated to a specific in-flight transfer under any form of pipelining.
- Response and error schemas do not need to duplicate a `request_id` field.

### 4.2 Type (byte 0)

Identifies the kind of message. Dispatch is an integer switch, not string matching.

| Value | Name | Description |
|---|---|---|
| `0x01` | `REQUEST` | Client-to-daemon RPC request |
| `0x02` | `RESPONSE` | Daemon-to-client RPC response |
| `0x03` | `FILE_CHUNK` | A chunk of file data (upload or download) |
| `0x04` | `ERROR` | Error response |
| `0x05` | `PING` | Keepalive / liveness probe |
| `0x06` | `PONG` | Keepalive response |
| `0x07` | `CAPABILITY` | Capability advertisement (sent by daemon on connect) |
| `0x08`–`0xFF` | Reserved | Available for future use |

Streams (file transfers, Process Monitor, Interactive Shell) terminate by clearing the `CONTINUATION` flag on the final frame (see §4.3). There is no separate stream-end frame type.

### 4.3 Flags (byte 1)

Eight individual bits, each a boolean toggle:

```
Bit:  7  6  5  4  3  2  1  0
      │  │  │  │  │  │  │  │
      │  │  │  │  │  │  │  └─ CONTINUATION: more frames follow in this stream
      │  │  │  │  │  │  └─── COMPRESSED: payload is compressed
      │  │  │  │  │  └───── PRIORITY: error priority escalation
      │  │  │  │  └─────── (reserved)
      │  │  │  └───────── (reserved)
      │  │  └─────────── (reserved)
      │  └───────────── (reserved)
      └─────────────── (reserved)
```

- **CONTINUATION (bit 0):** When set, indicates more frames belong to the same `stream_id`. Cleared on the last frame of the stream. Applies to both:
  - **Chunked payloads** — a single logical message split across multiple frames (e.g., successive `FILE_CHUNK` frames of one file transfer).
  - **Open streams** — an ongoing exchange whose end is signalled by the sender clearing the flag on the final frame (e.g., a Process Monitor subscription, an Interactive Shell session).
  In either case, the receiver treats a cleared `CONTINUATION` as "no more frames coming on this `stream_id`."
- **COMPRESSED (bit 1):** When set, the payload is compressed using the algorithm negotiated during capability exchange (see §9). Never set on `FILE_CHUNK` frames whose stream carries an already-compressed transfer, and never set on `PING`/`PONG` (their payload is empty).
- **PRIORITY (bit 2):** When set on an `ERROR` frame, this error should interrupt any in-flight response on the connection (cross-task error escalation). See §10.2 for `stream_id` semantics.
- **Bits 3–7:** Reserved. Must be set to 0 by senders. Receivers must ignore unknown flags for forward compatibility.

### 4.4 Opcode (bytes 2–3)

Identifies the RPC operation, encoded as a **16-bit unsigned little-endian** integer.

- Only meaningful on `REQUEST` frames — routes the payload to the correct handler on the daemon.
- On `RESPONSE` and `ERROR` frames, senders MUST echo the opcode of the originating request (aids debugging and lets the client validate schema before parsing).
- On `FILE_CHUNK`, `PING`, `PONG`, and `CAPABILITY` frames, `opcode` MUST be `0`.
- The full set of assigned opcodes is defined in the `.proto` schema and mirrored in the `CAPABILITY` frame's `operations` list (see [api.md](api.md)).
- Opcode `0` is reserved and means "no operation" — never used for a real RPC.

Dispatch is a single lookup on `opcode` — no envelope message needs to be parsed first.

### 4.5 Length (bytes 4–7)

Payload size in bytes, encoded as a **32-bit unsigned little-endian** integer.

- Minimum value: `0` (e.g., `PING`, `PONG`, and some empty-body responses carry no payload)
- Maximum value: constrained per frame type (see §6 Message Size Limits)
- The receiver reads these 4 bytes to know exactly how many payload bytes follow

Little-endian is chosen because effectively all shipping x86, ARM, and RISC-V hardware runs in LE mode. Big-endian targets apply a `bswap32` — trivial and branchless on all architectures.

### 4.6 Stream ID (bytes 8–11)

Correlation identifier, encoded as a **32-bit unsigned little-endian** integer.

- Assigned by the **client** on every `REQUEST` and MUST be unique per REQUEST within the connection's lifetime. A given `stream_id` never carries more than one REQUEST frame (wrapping at 2³² is acceptable for long-lived connections — collisions with completed streams are harmless as long as no frames for the old stream are still in flight).
- One `stream_id` may carry many `RESPONSE`, `ERROR`, and `FILE_CHUNK` frames — this is how streamed responses (Process Monitor), chunked transfers (FILE_PUT / FILE_GET), and long-lived output streams (Interactive Shell stdout) work. The rule is REQUEST-side: one stream_id, one REQUEST.
- Operations that carry multi-directional client input, like Interactive Shell, use a distinct fresh `stream_id` per client REQUEST and pass a `session_id` (equal to the opening REQUEST's `stream_id`) in the payload to associate them with the session.
- Echoed by the daemon on the matching `RESPONSE`, `ERROR`, and (for chunked or streaming operations) `FILE_CHUNK` frames.
- `PING`/`PONG` MAY use `stream_id = 0` (they are stateless keepalives).
- `CAPABILITY` uses `stream_id = 0` (there is no originating request).
- `ERROR` frames with the `PRIORITY` flag set use the `stream_id` of the originating background task if known; `stream_id = 0` denotes a connection-level error not tied to any specific request (see §10.2).

Correlation being in the header lets the receiver route a frame — including `FILE_CHUNK` with a raw-bytes payload — with no payload parsing at all.

### 4.7 Payload

The bytes immediately following the header, exactly `length` bytes long.

For everything except `FILE_CHUNK`, the payload is a serialized Protocol Buffer message whose schema is determined by the frame `type` and, for `REQUEST`/`RESPONSE`/`ERROR`, the `opcode`.

For `FILE_CHUNK`, the payload is raw file bytes with no protobuf wrapping, keeping overhead minimal on large transfers.

## 5. Reading a Frame (pseudocode)

```c
uint8_t header[12];
read_exact(sock, header, 12);       // block until all 12 bytes arrive

uint8_t  type      = header[0];
uint8_t  flags     = header[1];
uint16_t opcode    = (uint16_t)header[2]
                   | ((uint16_t)header[3] << 8);
uint32_t len       = (uint32_t)header[4]
                   | ((uint32_t)header[5] << 8)
                   | ((uint32_t)header[6] << 16)
                   | ((uint32_t)header[7] << 24);
uint32_t stream_id = (uint32_t)header[8]
                   | ((uint32_t)header[9] << 8)
                   | ((uint32_t)header[10] << 16)
                   | ((uint32_t)header[11] << 24);

if (len > max_payload_for(type)) {
    // reject: frame too large for this implementation / type
}

uint8_t payload[len];               // or a static buffer on embedded
read_exact(sock, payload, len);

dispatch(type, flags, opcode, stream_id, payload, len);
```

No parser. No state machine. No string scanning. Array indexing and bit shifts — works identically on a Linux server and a Cortex-M0.

## 6. Message Size Limits

Every message type defines a **maximum payload size** in the `.proto` schema options. This serves two purposes:

1. **Static allocation** — embedded daemons allocate fixed buffers at compile time
2. **Safety** — receivers reject oversized frames before reading the payload

Recommended defaults:

| Message type | Max payload |
|---|---|
| `PING` / `PONG` | 0 bytes |
| `CAPABILITY` | 1 KiB |
| `REQUEST` | 64 KiB |
| `RESPONSE` | 64 KiB |
| `ERROR` | 4 KiB |
| `FILE_CHUNK` | 64 KiB |

The frame `length` field can encode payloads up to 4 GiB, but every receiver enforces its per-type maximum before allocating buffers or reading the payload. Implementations may negotiate different limits during capability exchange (see §9).

## 7. Connection Lifecycle

### 7.1 Handshake

```
Client                          Daemon
  │                               │
  │──── TCP connect ─────────────▶│
  │                               │
  │◀─── CAPABILITY frame ────────│  (daemon advertises supported features)
  │                               │
  │──── REQUEST / FILE_CHUNK ───▶│  (normal operation begins)
  │◀─── RESPONSE ────────────────│
  │         ...                   │
```

1. Client opens a TCP connection to the daemon
2. Daemon immediately sends a `CAPABILITY` frame listing its supported API operations, protocol version, and any negotiable parameters (compression, chunk sizes)
3. Client may now send requests

There is no client-side handshake message. The connection is ready for requests as soon as the client has received the capability frame.

### 7.2 Keepalive

Either side may send a `PING` frame at any time. The other side must respond with a `PONG` frame. A missing pong within a configurable timeout indicates a dead connection.

### 7.3 Shutdown

Either side may close the TCP connection. The other side detects this as a read returning 0 bytes (EOF) and cleans up. No explicit shutdown frame is required.

## 8. File Transfers

Large files are transferred as a sequence of `FILE_CHUNK` frames. All frames in a single transfer share the `stream_id` of the originating `FILE_PUT` or `FILE_GET` request, so multiple transfers may be in flight concurrently on one connection without ambiguity.

### 8.1 Upload (client → daemon)

```
Client                          Daemon
  │                               │
  │──── REQUEST FILE_PUT ───────▶│   stream_id=S; metadata: path, size, perms
  │◀─── RESPONSE ───────────────│   stream_id=S; daemon ready
  │                               │
  │──── FILE_CHUNK [CONT] ─────▶│   stream_id=S; first chunk
  │──── FILE_CHUNK [CONT] ─────▶│   stream_id=S; ...
  │──── FILE_CHUNK ─────────────▶│   stream_id=S; final chunk (CONT cleared)
  │                               │
  │◀─── RESPONSE ───────────────│   stream_id=S; bytes_written, checksum
```

### 8.2 Download (daemon → client)

```
Client                          Daemon
  │                               │
  │──── REQUEST FILE_GET ───────▶│   stream_id=S; request: path
  │                               │
  │◀─── RESPONSE ───────────────│   stream_id=S; size, perms, checksum
  │◀─── FILE_CHUNK [CONT] ──────│   stream_id=S; first chunk
  │◀─── FILE_CHUNK [CONT] ──────│   stream_id=S; ...
  │◀─── FILE_CHUNK ─────────────│   stream_id=S; final chunk (CONT cleared)
```

### 8.3 Properties

- Neither side needs to hold the entire file in memory — chunks are written/read incrementally
- Chunk size is bounded by `FILE_CHUNK` max payload (default 64 KiB)
- The `CONTINUATION` flag distinguishes intermediate chunks from the final one; the `stream_id` distinguishes concurrent transfers
- Total file size is communicated in the initial request/response metadata so the receiver can pre-allocate or validate disk space
- File integrity is verified with a checksum in the metadata exchange (see [api.md §Checksums](api.md#checksums))

## 9. Capability Handshake

The `CAPABILITY` frame payload is a protobuf message containing:

- **Protocol version** — `major`/`minor`, for compatibility gating. Clients MUST refuse to speak to a daemon with a different `major`; different `minor` is compatible.
- **Supported opcodes** — list of RPC opcodes this daemon implements (e.g., `[PING, VERSION, FILE_PUT, FILE_GET, COMMAND_EXEC, …]`). Values match the `opcode` field in the frame header (§4.4).
- **Max payload sizes** — `map<uint32, uint32>` keyed by frame `type` byte (§4.2), giving per-type overrides for the defaults in §6. Keyed by integer, not string, so `FILE_CHUNK` (which has no protobuf schema name) can appear.
- **Compression algorithms** — ordered list of algorithms this daemon supports (e.g., `[NONE, LZ4]`). The client picks one from the list (or `NONE`) and MUST send its selection in a `CONFIGURATION_UPDATE` request under key `compression` before setting the `COMPRESSED` flag on any frame. Until a selection is made, `COMPRESSED` MUST NOT be set. Daemons that advertise only `NONE` need not implement compression at all.

Future extension: if per-frame algorithm selection is ever needed, it will be carried in currently-reserved flag bits or in an extended header — not by overloading `COMPRESSED`.

This lets the client adapt to daemons with different capability levels — a full Linux daemon exposes the complete API, while an RTOS daemon advertises only the subset it supports. The client can query capabilities programmatically and fail fast (with a clear `NOT_SUPPORTED` error) if it tries an unadvertised opcode, rather than sending a request and getting an opaque error.

There is no client-side capability message. The connection is ready for requests as soon as the client has received and parsed the `CAPABILITY` frame.

## 10. Error Handling

### 10.1 Frame-Level Errors

- **Oversized frame:** receiver reads the 12-byte header, sees `length` exceeds its maximum for that `type`, and (a) on a full-OS daemon SHOULD send an `ERROR` frame with `INVALID_REQUEST` and then close the connection; (b) on a constrained daemon MAY simply close the connection with no ERROR frame.
- **Unknown type:** receiver sends an `ERROR` frame with `NOT_SUPPORTED` and continues (forward compatibility).
- **Unknown opcode:** receiver sends an `ERROR` frame with `NOT_SUPPORTED` echoing the request's `stream_id` and continues.
- **Unknown flags:** receiver ignores unknown flag bits (forward compatibility).

### 10.2 Application-Level Errors

Application errors are sent as `ERROR` frames with a protobuf payload containing:

- **Error code** (enumerated, see [api.md §Error Codes](api.md#error-codes))
- **Human-readable message**
- **Detail** (optional string with implementation-specific context)

The originating request is identified by the `stream_id` in the frame header (§4.6), not by a payload field. `ERROR` frames echo the request's `stream_id`.

When the `PRIORITY` flag is set on an `ERROR` frame:

- `stream_id` is the `stream_id` of the background task that raised the error (as originally assigned by the client when it issued the `DETACH` request).
- If the error is not attributable to any specific task (a connection-level condition like out-of-memory), `stream_id` is `0`.
- The client MAY choose to fail any in-flight synchronous request when a `PRIORITY` error arrives; the exact policy (abort current call, mark next call as failed, log-and-continue) is a client-side concern and is not dictated by the protocol.
- If the client fails a request due to a `PRIORITY` error, and a `RESPONSE` for that request later arrives, the client MUST discard it (matched by `stream_id`).

## 11. Request-Response Correlation

Correlation is done via the `stream_id` field in the frame header (§4.6), not by any payload field. `RESPONSE`, `ERROR`, and streamed `FILE_CHUNK` frames echo the originating REQUEST's `stream_id`.

This supports:

- Pipelined requests (multiple in-flight requests on one connection)
- Concurrent file transfers (each transfer's `FILE_CHUNK` frames carry the originating request's `stream_id`)
- Detached-task errors arriving while a different request is in flight (§10.2)
- Long-lived daemon-to-client streams like Process Monitor and Interactive Shell stdout, where many RESPONSE frames flow on one `stream_id` originated by a single REQUEST

For operations where the client also sends multiple messages over the life of a "session" (Interactive Shell stdin), each such REQUEST uses its **own fresh `stream_id`** and carries a `session_id` field in its payload — because the "one REQUEST per stream_id" rule is what keeps the receiver's routing table unambiguous. See [api.md §2.2](api.md#22-interactive-shell) for the shell's concrete mapping.

Because `stream_id` is in the header, response schemas and error schemas do not carry a redundant `request_id` field.

Client responsibilities:

- Assign `stream_id`s that are unique per REQUEST within the connection's lifetime.
- Wrap-around is acceptable for long-lived connections — collisions with completed streams are harmless as long as the client doesn't reuse a `stream_id` that still has frames in flight.
- Reserved values: `stream_id = 0` is used for `PING`/`PONG`, `CAPABILITY`, and connection-level `ERROR` frames — clients MUST NOT assign `0` to a real request.

## 12. Future Considerations

These are not part of the current protocol but the design explicitly does not preclude them:

- **Compression algorithms:** the `COMPRESSED` flag and capability negotiation are defined; specific algorithms beyond `NONE` (e.g., LZ4) may be added by advertising them in the `CAPABILITY` frame's `compression` list. Per-frame algorithm selection can be introduced later via currently-reserved flag bits.
- **Multiplexing:** `stream_id` in the header already supports interleaved streams natively. No further additions needed unless per-stream flow control is introduced.
- **Protocol version negotiation:** the current design has the client accept-or-reject the daemon's advertised version. A future minor-version bump could add a client-side counter-offer (a `CLIENT_HELLO` frame after `CAPABILITY`), if downgrading proves useful. Not needed for v1.
- **UDP transport:** the framing format is transport-agnostic; a UDP variant could use the same frames with an added sequence number for reordering.
- **Encryption:** a TLS wrapper around the TCP connection would require no protocol changes.
