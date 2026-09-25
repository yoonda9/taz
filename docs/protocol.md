# TAZER Protocol Design

> Living document. Last updated: 2026-09-25

## 1. Overview

TAZER uses a custom binary framing protocol over TCP. The protocol is designed to be:

- **Simple** — a fixed 6-byte header, no string parsing, no state machines
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
│   Full OS Agent      │  │   Embedded Agent         │
│   libuv + full API   │  │   LWIP + API subset      │
│   Linux/macOS/Win    │  │   RTOS / bare-metal      │
└─────────────────────┘  └─────────────────────────┘
```

The protocol is the portability layer. Agents on different platforms are separate implementations that share the same wire format and message schemas.

## 3. Transport

- **TCP** over IPv4 or IPv6
- No TLS — TAZER operates in trusted test environments
- The agent listens on a configurable port (default TBD)
- Connections are persistent — a client connects once and issues multiple requests over the same connection
- Either side may close the connection at any time; the other side must handle this gracefully

## 4. Framing

Every message on the wire is a **frame** consisting of a fixed 6-byte header followed by a variable-length payload.

### 4.1 Header Format

```
Byte:  0         1         2    3    4    5
    ┌─────────┬─────────┬────┬────┬────┬────┐
    │  type   │  flags  │     length (LE)    │
    │  (1B)   │  (1B)   │       (4B)         │
    └─────────┴─────────┴────┴────┴────┴────┘
```

Total header size: **6 bytes**, fixed. No variable-length header fields.

### 4.2 Type (byte 0)

Identifies the kind of message. Dispatch is an integer switch, not string matching.

| Value | Name | Description |
|---|---|---|
| `0x01` | `REQUEST` | Client-to-agent RPC request |
| `0x02` | `RESPONSE` | Agent-to-client RPC response |
| `0x03` | `FILE_CHUNK` | A chunk of file data (upload or download) |
| `0x04` | `ERROR` | Error response |
| `0x05` | `STREAM_END` | Marks the end of a streamed sequence |
| `0x06` | `PING` | Keepalive / liveness probe |
| `0x07` | `PONG` | Keepalive response |
| `0x08` | `CAPABILITY` | Capability advertisement (sent by agent on connect) |
| `0x09`–`0xFF` | Reserved | Available for future use |

### 4.3 Flags (byte 1)

Eight individual bits, each a boolean toggle:

```
Bit:  7  6  5  4  3  2  1  0
      │  │  │  │  │  │  │  │
      │  │  │  │  │  │  │  └─ CONTINUATION: more frames follow in this sequence
      │  │  │  │  │  │  └─── COMPRESSED: payload is compressed
      │  │  │  │  │  └───── PRIORITY: error priority escalation
      │  │  │  │  └─────── (reserved)
      │  │  │  └───────── (reserved)
      │  │  └─────────── (reserved)
      │  └───────────── (reserved)
      └─────────────── (reserved)
```

- **CONTINUATION (bit 0):** When set, indicates more frames follow as part of the same logical message (e.g., successive file chunks). The final frame in a sequence has this bit cleared.
- **COMPRESSED (bit 1):** When set, the payload is compressed. Compression algorithm is negotiated during capability exchange.
- **PRIORITY (bit 2):** When set on an `ERROR` frame, this error should interrupt any in-flight response on the connection (cross-task error escalation).
- **Bits 3–7:** Reserved. Must be set to 0. Receivers must ignore unknown flags for forward compatibility.

### 4.4 Length (bytes 2–5)

Payload size in bytes, encoded as a **32-bit unsigned little-endian** integer.

- Minimum value: `0` (e.g., `PING`, `PONG`, `STREAM_END` may carry no payload)
- Maximum value: constrained per message type (see §6 Message Size Limits)
- The receiver reads these 4 bytes to know exactly how many payload bytes follow

Little-endian is chosen because ARM, x86, and RISC-V are natively LE. Big-endian targets apply a `bswap32` — trivial and branchless on all architectures.

### 4.5 Payload

The bytes immediately following the header, exactly `length` bytes long. Contents are a serialized Protocol Buffer message whose schema is determined by the `type` field.

For `FILE_CHUNK` frames, the payload is raw file bytes (not protobuf-wrapped), keeping overhead minimal on large transfers.

## 5. Reading a Frame (pseudocode)

```c
uint8_t header[6];
read_exact(sock, header, 6);        // block until all 6 bytes arrive

uint8_t  type  = header[0];
uint8_t  flags = header[1];
uint32_t len   = (uint32_t)header[2]
               | ((uint32_t)header[3] << 8)
               | ((uint32_t)header[4] << 16)
               | ((uint32_t)header[5] << 24);

if (len > MAX_PAYLOAD_SIZE) {
    // reject: frame too large for this implementation
}

uint8_t payload[len];               // or a static buffer on embedded
read_exact(sock, payload, len);

dispatch(type, flags, payload, len);
```

No parser. No state machine. No string scanning. Array indexing and bit shifts — works identically on a Linux server and a Cortex-M0.

## 6. Message Size Limits

Every message type defines a **maximum payload size** in the `.proto` schema options. This serves two purposes:

1. **Static allocation** — embedded agents allocate fixed buffers at compile time
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
| `STREAM_END` | 256 bytes |

These are protocol defaults. Implementations may negotiate different limits during capability exchange.

## 7. Connection Lifecycle

### 7.1 Handshake

```
Client                          Agent
  │                               │
  │──── TCP connect ─────────────▶│
  │                               │
  │◀─── CAPABILITY frame ────────│  (agent advertises supported features)
  │                               │
  │──── REQUEST / FILE_CHUNK ───▶│  (normal operation begins)
  │◀─── RESPONSE ────────────────│
  │         ...                   │
```

1. Client opens a TCP connection to the agent
2. Agent immediately sends a `CAPABILITY` frame listing its supported API operations, protocol version, and any negotiable parameters (compression, chunk sizes)
3. Client may now send requests

There is no client-side handshake message. The connection is ready for requests as soon as the client has received the capability frame.

### 7.2 Keepalive

Either side may send a `PING` frame at any time. The other side must respond with a `PONG` frame. A missing pong within a configurable timeout indicates a dead connection.

### 7.3 Shutdown

Either side may close the TCP connection. The other side detects this as a read returning 0 bytes (EOF) and cleans up. No explicit shutdown frame is required.

## 8. File Transfers

Large files are transferred as a sequence of `FILE_CHUNK` frames.

### 8.1 Upload (client → agent)

```
Client                          Agent
  │                               │
  │──── REQUEST (PutFile) ──────▶│   metadata: path, permissions, total size
  │◀─── RESPONSE (OK) ──────────│   agent ready to receive
  │                               │
  │──── FILE_CHUNK [CONT] ─────▶│   first chunk (continuation flag set)
  │──── FILE_CHUNK [CONT] ─────▶│   ...
  │──── FILE_CHUNK ─────────────▶│   final chunk (continuation flag cleared)
  │                               │
  │◀─── RESPONSE (PutFileAck) ──│   confirmation with checksum
```

### 8.2 Download (agent → client)

```
Client                          Agent
  │                               │
  │──── REQUEST (GetFile) ──────▶│   request: path
  │                               │
  │◀─── RESPONSE (FileInfo) ────│   metadata: size, permissions, checksum
  │◀─── FILE_CHUNK [CONT] ──────│   first chunk
  │◀─── FILE_CHUNK [CONT] ──────│   ...
  │◀─── FILE_CHUNK ─────────────│   final chunk (continuation flag cleared)
```

### 8.3 Properties

- Neither side needs to hold the entire file in memory — chunks are written/read incrementally
- Chunk size is bounded by `FILE_CHUNK` max payload (default 64 KiB)
- The `CONTINUATION` flag distinguishes intermediate chunks from the final one
- Total file size is communicated in the initial request/response metadata so the receiver can pre-allocate or validate disk space

## 9. Capability Handshake

The `CAPABILITY` frame payload is a protobuf message containing:

- **Protocol version** — major.minor, for compatibility gating
- **Supported operations** — list of API operations this agent implements (e.g., `[PING, VERSION, FILE_PUT, FILE_GET, COMMAND_EXEC]`)
- **Max payload sizes** — per-type overrides if different from protocol defaults
- **Compression algorithms** — supported compression (e.g., `[NONE, LZ4]`)

This lets the client adapt to agents with different capability levels — a full Linux agent exposes the complete API, while an RTOS agent advertises only the subset it supports. The client can query capabilities programmatically and fail fast (with a clear error) if it tries an unsupported operation, rather than sending a request and getting an opaque error.

## 10. Error Handling

### 10.1 Frame-Level Errors

- **Oversized frame:** receiver reads the 6-byte header, sees `length` exceeds its maximum, and closes the connection (or sends an `ERROR` frame and then closes)
- **Unknown type:** receiver sends an `ERROR` frame with a "not supported" code and continues (forward compatibility)
- **Unknown flags:** receiver ignores unknown flag bits (forward compatibility)

### 10.2 Application-Level Errors

Application errors are sent as `ERROR` frames with a protobuf payload containing:

- Error code (enumerated)
- Human-readable message
- Originating request ID (to correlate errors with requests)

When the `PRIORITY` flag is set on an `ERROR` frame, it indicates a critical failure from a detached task that should interrupt the client's current operation.

## 11. Request-Response Correlation

Each `REQUEST` frame carries a **request ID** (a uint32 field in the protobuf payload). The corresponding `RESPONSE` or `ERROR` frame echoes the same ID. This allows the client to match responses to requests, which is required for:

- Pipelined requests (multiple in-flight requests on one connection)
- Detached task errors arriving while a different request is in-flight

Request IDs are assigned by the client and must be unique within a connection's lifetime (wrapping at 2³² is acceptable for long-lived connections — collisions with completed requests are harmless).

## 12. Future Considerations

These are not part of the current protocol but the design explicitly does not preclude them:

- **Compression:** the `COMPRESSED` flag and capability negotiation are defined but no compression algorithm is mandated yet
- **Multiplexing:** request IDs already support pipelining; full multiplexing (interleaved responses) could be added with a stream ID field if needed
- **UDP transport:** the framing format is transport-agnostic; a UDP variant could use the same frames with an added sequence number for ordering
- **Encryption:** a TLS wrapper around the TCP connection would require no protocol changes
