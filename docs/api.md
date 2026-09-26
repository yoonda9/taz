# TAZER API

> Living document. Last updated: 2026-09-26
>
> See also: [Requirements](requirements.md) · [Protocol Design](protocol.md) · [Design Overview](index.md)

## Overview

The TAZER API is a set of RPC operations exposed by the daemon over the [TAZER binary protocol](protocol.md).

- Every operation has a stable **opcode** (see §Opcodes below). The opcode travels in the frame header (`protocol.md` §4.4), not in the payload. Dispatch on the daemon is a single integer lookup — no envelope message to parse.
- Requests and responses are serialized as Protocol Buffers. Each operation defines its own request and response message directly; there is no wrapper.
- Correlation is by `stream_id` in the frame header (`protocol.md` §4.6). Response and error frames echo the client's `stream_id`, so response schemas do not carry a `request_id` field.
- Not all daemons implement every operation. On connection, the daemon sends a `CAPABILITY` frame advertising which opcodes it supports. The client MUST check capabilities before calling an operation, or handle a `NOT_SUPPORTED` error gracefully.
- Any response may exceed the `RESPONSE` frame limit and be delivered as a chunked response (`protocol.md` §6.1): several `RESPONSE` frames with `CONTINUATION` set on all but the last, merged by the receiver. Clients MUST implement the merge rule for every operation; it is most likely to occur on `COMMAND_EXEC`, `PROCESS_LIST`, `PROCESS_INFO`, `DIR_LIST`, and `LOG`.
- Fields written as `map<K, V>` in the tables below are modeled on the wire as `repeated` key/value pair messages — `KeyValue { string key = 1; string value = 2; }` for string maps and `UInt32Pair { uint32 key = 1; uint32 value = 2; }` for integer maps. This is byte-for-byte identical to protobuf's `map<>` encoding, so implementations may use either form; the reference schemas use the explicit pair messages for compatibility with constrained-target protobuf libraries.

## Opcodes

Opcodes are 16-bit unsigned integers, defined as an enum in the `.proto` schema and mirrored in the `CAPABILITY` frame's `operations` list. Opcode `0` is reserved. Naming convention: `SCREAMING_SNAKE_CASE`, matching the enum values.

| Opcode   | Name                   | Section                              |
| -------- | ---------------------- | ------------------------------------ |
| `0x0001` | `PING`                 | §1.1 (also a frame type — see below) |
| `0x0002` | `VERSION`              | §1.2                                 |
| `0x0003` | `CONFIGURATION_GET`    | §1.4                                 |
| `0x0004` | `CONFIGURATION_UPDATE` | §1.5                                 |
| `0x0005` | `RESTART`              | §1.6                                 |
| `0x0006` | `UPDATE`               | §1.7                                 |
| `0x0010` | `COMMAND_EXEC`         | §2.1                                 |
| `0x0011` | `SHELL_OPEN`           | §2.2                                 |
| `0x0012` | `SHELL_INPUT`          | §2.2                                 |
| `0x0013` | `SHELL_CLOSE`          | §2.2                                 |
| `0x0020` | `PROCESS_LIST`         | §3.1                                 |
| `0x0021` | `PROCESS_KILL`         | §3.2                                 |
| `0x0022` | `PROCESS_INFO`         | §3.3                                 |
| `0x0023` | `PROCESS_MONITOR`      | §3.4                                 |
| `0x0030` | `FILE_PUT`             | §4.1                                 |
| `0x0031` | `FILE_GET`             | §4.2                                 |
| `0x0032` | `FILE_CREATE`          | §4.3                                 |
| `0x0033` | `FILE_DELETE`          | §4.4                                 |
| `0x0034` | `FILE_STAT`            | §4.5                                 |
| `0x0035` | `FILE_CHMOD`           | §4.6                                 |
| `0x0036` | `DIR_MAKE`             | §4.7                                 |
| `0x0037` | `DIR_LIST`             | §4.8                                 |
| `0x0038` | `DIR_REMOVE`           | §4.9                                 |
| `0x0040` | `RUN_AS`               | §5.1                                 |
| `0x0041` | `TIMEOUT_SET`          | §5.2                                 |
| `0x0042` | `LOG`                  | §5.3                                 |
| `0x0043` | `DETACH`               | §5.4                                 |
| `0x0044` | `TASK_STATUS`          | §5.5                                 |
| `0x0045` | `TASK_CANCEL`          | §5.6                                 |
| `0x0046` | `CANCEL`               | §5.7                                 |
| `0x0050` | `PIPELINE`             | §7.3                                 |

`PING`/`PONG` and `CAPABILITY` are handled at the frame layer (`protocol.md` §4.2), not as RPCs — they do not appear in the capability `operations` list, but `PING` is assigned an opcode for symmetry in case it is ever wrapped as an RPC.

## Checksums

Where the API refers to `checksum`, the algorithm is **CRC32C** (Castagnoli polynomial, 4 bytes little-endian). Chosen because it is:

- Cheap on MCUs (small lookup-table implementation is fewer than 200 bytes).
- Hardware-accelerated on x86 (SSE4.2 `CRC32` instruction) and ARMv8 (`CRC32CX`).
- Sufficient for detecting the corruption modes that matter in a trusted transport (bit flips, truncation).

CRC32C is an integrity check, not an authentication check. If cryptographic verification is ever needed, it will be added as a separate optional field.

## 1. Daemon Control

Operations for managing the daemon itself.

### 1.1 Ping

Liveness check. Uses the protocol-level `PING`/`PONG` frames (no protobuf payload). Client-initiated only: the daemon answers `PING` with `PONG` and never sends `PING` itself (`protocol.md` §7.2).

| Field       | Value                            |
| ----------- | -------------------------------- |
| Frame type  | `PING` (0x05) / `PONG` (0x06)    |
| Payload     | None                             |
| `stream_id` | May be `0` (stateless keepalive) |

### 1.2 Version

Returns the daemon's version, build info, and platform.

**Request (`VERSION`):**

| Field    | Type | Description        |
| -------- | ---- | ------------------ |
| _(none)_ |      | Empty request body |

**Response:**

| Field      | Type   | Description                                |
| ---------- | ------ | ------------------------------------------ |
| `version`  | string | Semantic version (e.g. `"0.1.0"`)          |
| `build`    | string | Build identifier or commit hash            |
| `platform` | string | OS and architecture (e.g. `"linux/amd64"`) |

### 1.3 Capabilities

Sent by the daemon immediately on connection as a `CAPABILITY` frame (`stream_id = 0`). Not a request/response — the client receives it passively.

**Payload:**

| Field               | Type                | Description                                                                                                                     |
| ------------------- | ------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `protocol_major`    | uint32              | Major protocol version. Client MUST refuse to speak if it disagrees.                                                            |
| `protocol_minor`    | uint32              | Minor protocol version. Different minor is compatible.                                                                          |
| `operations`        | repeated uint32     | Opcodes this daemon implements                                                                                                  |
| `max_payload_sizes` | map<uint32, uint32> | Per-frame-type payload maximums, keyed by the `type` byte (see `protocol.md` §4.2). Overrides the defaults in `protocol.md` §6. |
| `compression`       | repeated string     | Supported compression algorithms in preferred order (e.g. `["NONE", "LZ4"]`)                                                    |

The client selects a compression algorithm by sending `CONFIGURATION_UPDATE` with key `compression` set to one of the advertised names. Until then, the `COMPRESSED` flag MUST NOT be set on any frame.

### 1.4 Configuration Get

Retrieve the daemon's current configuration.

**Request (`CONFIGURATION_GET`):**

| Field  | Type            | Description                                   |
| ------ | --------------- | --------------------------------------------- |
| `keys` | repeated string | Specific keys to retrieve; empty = return all |

**Response:**

| Field    | Type                | Description                   |
| -------- | ------------------- | ----------------------------- |
| `config` | map<string, string> | Key-value configuration pairs |

Values are strings; the agreed convention for structured values is dotted-path keys (e.g. `net.port = "5555"`, `log.level = "INFO"`). Boolean values use `"true"` / `"false"`; numbers are decimal strings. Daemons SHOULD reject values they cannot parse, and MUST reject unknown keys rather than silently accept them.

Keys defined by the reference daemon: `log.level` (`DEBUG`/`INFO`/`WARN`/`ERROR`), `compression` (one of the names advertised in `CAPABILITY`), `exec.max_output_bytes` (decimal; combined stdout+stderr cap for `COMMAND_EXEC`, default `1048576`). Other daemons may define additional keys.

### 1.5 Configuration Update

Modify the daemon's configuration at runtime.

**Request (`CONFIGURATION_UPDATE`):**

| Field    | Type                | Description            |
| -------- | ------------------- | ---------------------- |
| `config` | map<string, string> | Key-value pairs to set |

**Response:**

| Field      | Type                 | Description                                  |
| ---------- | -------------------- | -------------------------------------------- |
| `applied`  | repeated string      | Keys that were successfully applied          |
| `rejected` | repeated RejectedKey | Keys that could not be applied, with reasons |

**RejectedKey:**

| Field    | Type   | Description           |
| -------- | ------ | --------------------- |
| `key`    | string | Rejected key          |
| `reason` | string | Human-readable reason |

### 1.6 Restart

Restart the daemon process. The TCP connection will be dropped and the client must reconnect.

**Request (`RESTART`):**

| Field      | Type   | Description                                                                                                           |
| ---------- | ------ | --------------------------------------------------------------------------------------------------------------------- |
| `delay_ms` | uint32 | Delay before restart. Minimum enforced value: `100` (daemon clamps `0` up to `100` to ensure the response can flush). |

**Response:**

| Field                | Type   | Description                                            |
| -------------------- | ------ | ------------------------------------------------------ |
| `acknowledged`       | bool   | Daemon accepted the restart request                    |
| `effective_delay_ms` | uint32 | The actual delay the daemon will use before restarting |

The daemon MUST fully write the response (including a `shutdown(SHUT_WR)` and drain) before terminating the process. Clients should treat the TCP FIN that follows as the signal to reconnect.

Restarting terminates every detached task (§5.4) along with its process tree; task state does not survive a restart.

### 1.7 Update

Replace the daemon binary with a new version. The client uploads the new binary via `FILE_PUT` (§4.1) first — the checksum returned by `FILE_PUT` is authoritative — then issues this command to swap and restart.

**Request (`UPDATE`):**

| Field         | Type   | Description                                            |
| ------------- | ------ | ------------------------------------------------------ |
| `binary_path` | string | Path to the uploaded binary on the daemon's filesystem |
| `restart`     | bool   | Whether to restart immediately after update            |

**Response:**

| Field     | Type   | Description                    |
| --------- | ------ | ------------------------------ |
| `success` | bool   | Whether the update was applied |
| `message` | string | Status or error detail         |

When `restart` is true, the same flush-before-terminate guarantee as `RESTART` applies.

## 2. Command Execution

### 2.1 Execute Command

Run a command on the host and return its output.

**Request (`COMMAND_EXEC`):**

| Field         | Type                | Description                                                                                                    |
| ------------- | ------------------- | -------------------------------------------------------------------------------------------------------------- |
| `command`     | string              | The command to execute                                                                                         |
| `args`        | repeated string     | Arguments                                                                                                      |
| `env`         | map<string, string> | Additional environment variables                                                                               |
| `working_dir` | string              | Working directory (default: daemon's cwd)                                                                      |
| `timeout_ms`  | uint32              | Timeout in milliseconds (0 = fall back to the connection default from §5.2)                                    |
| `as_user`     | string              | Optional. Run as this user for this command only. If empty, uses the connection's current identity (see §5.1). |

**Response:**

| Field       | Type  | Description                                                                                                                        |
| ----------- | ----- | ---------------------------------------------------------------------------------------------------------------------------------- |
| `exit_code` | int32 | Process exit code                                                                                                                  |
| `stdout`    | bytes | Standard output                                                                                                                    |
| `stderr`    | bytes | Standard error                                                                                                                     |
| `timed_out` | bool  | Whether the command was killed due to timeout                                                                                      |
| `truncated` | bool  | Whether `stdout`/`stderr` were truncated because combined output exceeded the daemon's `exec.max_output_bytes` configuration value |

Output larger than a single `RESPONSE` frame is returned as a chunked response (`protocol.md` §6.1). A timeout kills the whole process tree of the command, not only the direct child.

### 2.2 Interactive Shell

Opens a bidirectional shell session. Requires a persistent connection.

> **Note:** Interactive Shell is the one operation that is inherently bidirectional-streaming. Purely-synchronous clients MAY choose not to implement it; daemons SHOULD NOT require it (see [requirements.md §4.1](requirements.md#41-reference-implementation)).

The session is opened with `SHELL_OPEN`. That request's `stream_id` (call it `S`) is reserved for the daemon's stdout/stderr stream: the daemon sends `RESPONSE` frames on `S` with `CONTINUATION` set for the life of the session. The client sends stdin and close as ordinary discrete REQUESTs, each with its own unique `stream_id` and a `session_id = S` field in the payload to route it to the correct session. This preserves the protocol rule that a `stream_id` is never reused across REQUEST frames.

**Request (`SHELL_OPEN`):**

| Field     | Type                | Description                                 |
| --------- | ------------------- | ------------------------------------------- |
| `shell`   | string              | Shell to invoke (default: platform default) |
| `env`     | map<string, string> | Additional environment variables            |
| `as_user` | string              | Optional; same semantics as §2.1            |

**Response (`SHELL_OPEN`) — first frame on `S`, `CONTINUATION` set:**

| Field        | Type   | Description                                                                                  |
| ------------ | ------ | -------------------------------------------------------------------------------------------- |
| `session_id` | uint32 | The session ID (equals `S`; returned explicitly so clients can treat it as an opaque handle) |
| `pty`        | bool   | Whether a PTY was allocated                                                                  |

**Daemon stdout/stderr — subsequent RESPONSE frames on `S`, `CONTINUATION` set:**

| Field       | Type  | Description                                   |
| ----------- | ----- | --------------------------------------------- |
| `stdout`    | bytes | Stdout bytes (may be empty)                   |
| `stderr`    | bytes | Stderr bytes (may be empty)                   |
| `exit_code` | int32 | Shell exit code (set only on the final frame) |

When the shell exits (or `SHELL_CLOSE` is honored), the daemon sends one final RESPONSE frame on `S` with `CONTINUATION` cleared and `exit_code` set.

**Request (`SHELL_INPUT`) — one REQUEST per input burst, each with a fresh unique `stream_id`:**

| Field        | Type   | Description         |
| ------------ | ------ | ------------------- |
| `session_id` | uint32 | Session to route to |
| `data`       | bytes  | Raw stdin bytes     |

**Response (`SHELL_INPUT`):**

| Field            | Type   | Description                                                      |
| ---------------- | ------ | ---------------------------------------------------------------- |
| `bytes_accepted` | uint32 | Number of stdin bytes accepted (equals `data` length on success) |

**Request (`SHELL_CLOSE`) — fresh unique `stream_id`:**

| Field        | Type   | Description      |
| ------------ | ------ | ---------------- |
| `session_id` | uint32 | Session to close |

**Response (`SHELL_CLOSE`):**

| Field      | Type | Description                                                                      |
| ---------- | ---- | -------------------------------------------------------------------------------- |
| `accepted` | bool | Whether the close was accepted (the final RESPONSE on `S` carries the exit code) |

## 3. Process Management

### 3.1 Process List

Enumerate running processes on the host.

**Request (`PROCESS_LIST`):**

| Field    | Type   | Description                              |
| -------- | ------ | ---------------------------------------- |
| `filter` | string | Optional filter (process name substring) |

**Response:**

| Field       | Type                 | Description                |
| ----------- | -------------------- | -------------------------- |
| `processes` | repeated ProcessInfo | List of matching processes |

**ProcessInfo:**

| Field          | Type   | Description                      |
| -------------- | ------ | -------------------------------- |
| `pid`          | uint32 | Process ID                       |
| `name`         | string | Process name                     |
| `user`         | string | Owning user                      |
| `cpu_percent`  | float  | CPU usage                        |
| `memory_bytes` | uint64 | Memory usage                     |
| `state`        | string | Running, sleeping, stopped, etc. |

### 3.2 Process Kill

Terminate a process.

**Request (`PROCESS_KILL`):**

| Field    | Type   | Description                                             |
| -------- | ------ | ------------------------------------------------------- |
| `pid`    | uint32 | Process ID                                              |
| `signal` | int32  | Signal to send (default: platform's SIGTERM equivalent) |

**Response:**

| Field     | Type | Description                      |
| --------- | ---- | -------------------------------- |
| `success` | bool | Whether the signal was delivered |

### 3.3 Process Info

Detailed information about a specific process.

**Request (`PROCESS_INFO`):**

| Field | Type   | Description |
| ----- | ------ | ----------- |
| `pid` | uint32 | Process ID  |

**Response:**

| Field          | Type            | Description                          |
| -------------- | --------------- | ------------------------------------ |
| `info`         | ProcessInfo     | Full process details                 |
| `command_line` | string          | Full command line                    |
| `start_time`   | uint64          | Process start time (Unix timestamp)  |
| `open_files`   | repeated string | Open file descriptors (if available) |

### 3.4 Process Monitor

Subscribe to ongoing status updates for an OS process. The daemon sends periodic `RESPONSE` frames with `CONTINUATION` set until the process exits or the client cancels via `CANCEL` (§5.7).

**Request (`PROCESS_MONITOR`):**

| Field         | Type   | Description                     |
| ------------- | ------ | ------------------------------- |
| `pid`         | uint32 | Process ID                      |
| `interval_ms` | uint32 | Update interval in milliseconds |

**Response (repeated, streamed):**

| Field             | Type        | Description                                                                                                                                                                                                                                                                            |
| ----------------- | ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `info`            | ProcessInfo | Current process state                                                                                                                                                                                                                                                                  |
| `exited`          | bool        | True if the process has exited                                                                                                                                                                                                                                                         |
| `exit_code`       | int32       | Exit code (only set when `exited` is true). **Best-effort**: on POSIX an exit status is only obtainable for processes the daemon itself spawned (detached tasks); for other PIDs it is `0` and `exit_code_known` is false. On Windows it is available for any PID the daemon can open. |
| `exit_code_known` | bool        | Whether `exit_code` is authoritative                                                                                                                                                                                                                                                   |
| `reason`          | string      | Why the stream ended: `""` (still open), `"exited"`, `"cancelled"`, `"error"`                                                                                                                                                                                                          |

The final response has `CONTINUATION` cleared. `cpu_percent` requires two samples and is `0` on the first update. To subscribe to a TAZER-managed detached task rather than an OS PID, use `TASK_STATUS` (§5.5).

## 4. File Operations

All file transfers use the chunked `FILE_CHUNK` frame mechanism described in [Protocol Design §8](protocol.md#8-file-transfers). Metadata is exchanged via `REQUEST`/`RESPONSE` frames; the file bytes themselves are raw `FILE_CHUNK` frames sharing the request's `stream_id`.

### 4.1 Put File

Transfer a file from client to daemon.

**Request (`FILE_PUT`):**

| Field         | Type   | Description                           |
| ------------- | ------ | ------------------------------------- |
| `dest`        | string | Destination path on the daemon        |
| `size`        | uint64 | Total file size in bytes              |
| `permissions` | uint32 | File permissions (POSIX mode bits)    |
| `overwrite`   | bool   | Whether to overwrite an existing file |

The initial REQUEST and both subsequent RESPONSE frames use `CONTINUATION = 0` — they are discrete, ordinary request/response frames. `CONTINUATION` is used **only** on the `FILE_CHUNK` sequence: set on every chunk except the last. What holds the transfer together as one logical operation is the shared `stream_id`, not the flag.

`FILE_PUT` returns **two** RESPONSE frames on the same `stream_id` — an initial acknowledgment before chunks flow, and a final confirmation after the last chunk is received. Both use the same protobuf schema modeled as a `oneof`:

**FilePutResponse (protobuf):**

```proto
message FilePutResponse {
  oneof phase {
    Ack        ack        = 1;   // sent first, before FILE_CHUNK frames
    Confirmation confirm = 2;   // sent after the final FILE_CHUNK
  }
  message Ack {
    bool ready = 1;             // daemon is ready to receive chunks
  }
  message Confirmation {
    uint64 bytes_written = 1;
    bytes  checksum      = 2;   // CRC32C of the written file
  }
}
```

Client receives the two RESPONSEs in order and dispatches on the `phase` discriminator.

Failure and cancellation semantics — daemon-side error mid-stream, client abort via `CANCEL`, and temp-file-then-rename atomicity — are defined in `protocol.md` §8.4. The daemon rejects the transfer with `INVALID_REQUEST` if the number of bytes received does not equal `size`.

### 4.2 Get File

Transfer a file from daemon to client.

**Request (`FILE_GET`):**

| Field | Type   | Description               |
| ----- | ------ | ------------------------- |
| `src` | string | Source path on the daemon |

**Response (metadata):**

| Field         | Type   | Description              |
| ------------- | ------ | ------------------------ |
| `size`        | uint64 | Total file size in bytes |
| `permissions` | uint32 | File permissions         |
| `checksum`    | bytes  | CRC32C of the file       |

Followed by `FILE_CHUNK` frames on the same `stream_id`. As with §4.1, the REQUEST and metadata RESPONSE frames use `CONTINUATION = 0`; only the `FILE_CHUNK` sequence uses `CONTINUATION` (set on every chunk except the last). The shared `stream_id` is what associates the metadata with its chunks.

Because `checksum` precedes the chunks, the daemon reads the file once to compute it and once to send it. If the file changes between the two passes the client's verification fails; the client SHOULD report this as a checksum mismatch rather than retry silently. The client may abort with `CANCEL` (§5.7).

### 4.3 Create File

Create an empty file or a file with inline content (for small files that don't warrant chunked transfer).

**Request (`FILE_CREATE`):**

| Field         | Type   | Description                                                                                                                            |
| ------------- | ------ | -------------------------------------------------------------------------------------------------------------------------------------- |
| `path`        | string | File path                                                                                                                              |
| `content`     | bytes  | Optional content (must fit in a single `REQUEST` frame; clients SHOULD reject larger content locally and direct callers to `FILE_PUT`) |
| `permissions` | uint32 | File permissions                                                                                                                       |

**Response:**

| Field     | Type | Description                  |
| --------- | ---- | ---------------------------- |
| `success` | bool | Whether the file was created |

### 4.4 Delete File

Remove a file from the daemon's filesystem.

**Request (`FILE_DELETE`):**

| Field  | Type   | Description |
| ------ | ------ | ----------- |
| `path` | string | File path   |

**Response:**

| Field     | Type | Description                  |
| --------- | ---- | ---------------------------- |
| `success` | bool | Whether the file was deleted |

### 4.5 Stat File

Retrieve metadata about a file or directory.

**Request (`FILE_STAT`):**

| Field  | Type   | Description |
| ------ | ------ | ----------- |
| `path` | string | File path   |

**Response:**

| Field         | Type   | Description                                            |
| ------------- | ------ | ------------------------------------------------------ |
| `size`        | uint64 | File size in bytes                                     |
| `permissions` | uint32 | File permissions                                       |
| `owner`       | string | File owner                                             |
| `modified`    | uint64 | Last modified time (Unix timestamp)                    |
| `created`     | uint64 | Creation time (Unix timestamp)                         |
| `kind`        | Kind   | One of: `FILE`, `DIR`, `SYMLINK`, `OTHER`              |
| `link_target` | string | If `kind == SYMLINK`, the target path; empty otherwise |

### 4.6 Modify File Permissions

Change permissions on an existing file.

**Request (`FILE_CHMOD`):**

| Field         | Type   | Description         |
| ------------- | ------ | ------------------- |
| `path`        | string | File path           |
| `permissions` | uint32 | New POSIX mode bits |

**Response:**

| Field     | Type | Description                      |
| --------- | ---- | -------------------------------- |
| `success` | bool | Whether permissions were changed |

### 4.7 Make Directory

Create a directory.

**Request (`DIR_MAKE`):**

| Field         | Type   | Description                                           |
| ------------- | ------ | ----------------------------------------------------- |
| `path`        | string | Directory path                                        |
| `permissions` | uint32 | POSIX mode bits (default: `0755`)                     |
| `parents`     | bool   | Create parent directories as needed (like `mkdir -p`) |

**Response:**

| Field     | Type | Description                       |
| --------- | ---- | --------------------------------- |
| `success` | bool | Whether the directory was created |

### 4.8 List Directory

Enumerate a directory's entries.

**Request (`DIR_LIST`):**

| Field            | Type   | Description                       |
| ---------------- | ------ | --------------------------------- |
| `path`           | string | Directory path                    |
| `include_hidden` | bool   | Include entries starting with `.` |

**Response:**

| Field     | Type              | Description       |
| --------- | ----------------- | ----------------- |
| `entries` | repeated DirEntry | Directory entries |

**DirEntry:**

| Field  | Type   | Description                       |
| ------ | ------ | --------------------------------- |
| `name` | string | Entry name (not full path)        |
| `kind` | Kind   | `FILE`, `DIR`, `SYMLINK`, `OTHER` |
| `size` | uint64 | Size in bytes (0 for non-files)   |

### 4.9 Remove Directory

Remove a directory.

**Request (`DIR_REMOVE`):**

| Field       | Type   | Description                 |
| ----------- | ------ | --------------------------- |
| `path`      | string | Directory path              |
| `recursive` | bool   | Remove contents recursively |

**Response:**

| Field     | Type | Description                       |
| --------- | ---- | --------------------------------- |
| `success` | bool | Whether the directory was removed |

## 5. Advanced Operations

### 5.1 Run As

Set the durable identity for subsequent **process-spawning** operations (`COMMAND_EXEC`, `SHELL_OPEN`, and detached/pipelined forms of them) on this connection.

Daemons MAY limit `RUN_AS` to process-spawning operations; file and process-management operations then continue to run under the daemon's own identity. The reference daemon does so on every platform: the identity is applied to each spawned child (POSIX `uid`/`gid` at spawn, Windows `CreateProcessAsUser`), never to the daemon process itself, so one connection's `RUN_AS` cannot leak into another connection's work. A daemon that cannot switch identity at all (e.g. not running as root / SYSTEM with the required privilege) omits `RUN_AS` from its `CAPABILITY` operations list and answers it with `NOT_SUPPORTED`.

Per-request `as_user` on `COMMAND_EXEC` and `SHELL_OPEN` overrides this for a single call. `RUN_AS` is convenience for "run a batch of things as X without repeating `as_user` every call."

**Request (`RUN_AS`):**

| Field  | Type   | Description                                                                |
| ------ | ------ | -------------------------------------------------------------------------- |
| `user` | string | Username to assume. Empty string resets to the daemon's original identity. |

**Response:**

| Field            | Type   | Description                           |
| ---------------- | ------ | ------------------------------------- |
| `success`        | bool   | Whether the identity switch succeeded |
| `effective_user` | string | The identity now in effect            |

### 5.2 Timeout

Set the default timeout for operations on this connection. Per-operation `timeout_ms` (when nonzero) overrides this default.

**Request (`TIMEOUT_SET`):**

| Field        | Type   | Description                                      |
| ------------ | ------ | ------------------------------------------------ |
| `timeout_ms` | uint32 | Default timeout in milliseconds (0 = no timeout) |

**Response:**

| Field         | Type   | Description                |
| ------------- | ------ | -------------------------- |
| `previous_ms` | uint32 | The previous timeout value |

### 5.3 Log

Retrieve daemon log entries.

**Request (`LOG`):**

| Field   | Type   | Description                                                 |
| ------- | ------ | ----------------------------------------------------------- |
| `lines` | uint32 | Number of recent log lines to retrieve                      |
| `since` | uint64 | Only return entries after this Unix timestamp               |
| `level` | string | Minimum log level filter (`DEBUG`, `INFO`, `WARN`, `ERROR`) |

**Response:**

| Field     | Type              | Description |
| --------- | ----------------- | ----------- |
| `entries` | repeated LogEntry | Log entries |

**LogEntry:**

| Field       | Type   | Description    |
| ----------- | ------ | -------------- |
| `timestamp` | uint64 | Unix timestamp |
| `level`     | string | Log level      |
| `message`   | string | Log message    |

### 5.4 Detach

Execute a task in the background. The daemon runs it asynchronously and the client can optionally monitor it (via `TASK_STATUS`) or set an error priority for cross-task escalation.

The detached task is a full operation, not just a shell command — any of `COMMAND_EXEC`, `PIPELINE`, `FILE_PUT`, or `FILE_GET` may be detached. The task's request body is passed inline.

**Request (`DETACH`):**

| Field            | Type          | Description                                                                 |
| ---------------- | ------------- | --------------------------------------------------------------------------- |
| `task`           | oneof Task    | The operation to run in the background (see below)                          |
| `monitor`        | bool          | Whether the daemon should retain status for later polling via `TASK_STATUS` |
| `error_priority` | ErrorPriority | `NONE` or `CRITICAL` (enum)                                                 |

**Task (oneof):**

| Field      | Type                                                                           |
| ---------- | ------------------------------------------------------------------------------ |
| `exec`     | `COMMAND_EXEC` request                                                         |
| `pipeline` | `PIPELINE` request                                                             |
| `file_put` | `FILE_PUT` request (followed by `FILE_CHUNK` frames on the returned `task_id`) |
| `file_get` | `FILE_GET` request                                                             |

**Response:**

| Field     | Type   | Description                                                              |
| --------- | ------ | ------------------------------------------------------------------------ |
| `task_id` | uint32 | Identifier for the detached task, valid for the connection's lifetime    |
| `pid`     | uint32 | Underlying OS process ID, if applicable (e.g. for `exec`); `0` otherwise |

`error_priority = CRITICAL` means: if the task fails, the daemon sends an `ERROR` frame with the `PRIORITY` flag set (see `protocol.md` §10.2), using this task's `stream_id` (the same value the client sent as `stream_id` on the `DETACH` request). The echoed `opcode` on that `ERROR` frame is `DETACH`.

Only two priorities exist on the wire: `NONE` and `CRITICAL`, matching the single `PRIORITY` flag bit.

**Lifetime.** Detached tasks are **connection-scoped**. When the connection that created a task closes for any reason, or the daemon restarts, the daemon terminates the task's entire process tree and discards its state. A test that needs a process to outlive the client connection must start it via a mechanism on the host (e.g. a service manager) rather than `DETACH`.

### 5.5 Task Status

Query the status of a detached task by `task_id`, or subscribe to periodic status updates.

**Request (`TASK_STATUS`):**

| Field         | Type   | Description                                                                                                      |
| ------------- | ------ | ---------------------------------------------------------------------------------------------------------------- |
| `task_id`     | uint32 | Task to query                                                                                                    |
| `subscribe`   | bool   | If true, daemon streams updates (with `CONTINUATION` set) until the task ends or the client cancels via `CANCEL` |
| `interval_ms` | uint32 | Update interval when `subscribe` is true                                                                         |

**Response (single or streamed):**

| Field            | Type      | Description                                                                      |
| ---------------- | --------- | -------------------------------------------------------------------------------- |
| `state`          | State     | `PENDING`, `RUNNING`, `COMPLETED`, `FAILED`, `CANCELLED`                         |
| `pid`            | uint32    | OS PID if applicable, else `0`                                                   |
| `progress`       | uint64    | Progress metric where meaningful (e.g. bytes transferred for `FILE_PUT`)         |
| `progress_total` | uint64    | Total for progress ratio (0 if unknown)                                          |
| `result`         | bytes     | Serialized result of the underlying operation, present when `state == COMPLETED` |
| `error`          | ErrorInfo | Error details, present when `state == FAILED`                                    |

For streamed responses, the final frame has `CONTINUATION` cleared.

### 5.6 Task Cancel

Request cancellation of a detached task.

**Request (`TASK_CANCEL`):**

| Field     | Type   | Description                                                                    |
| --------- | ------ | ------------------------------------------------------------------------------ |
| `task_id` | uint32 | Task to cancel                                                                 |
| `signal`  | int32  | Signal to send (for `exec`/`pipeline` tasks with an OS PID); ignored otherwise |

**Response:**

| Field      | Type | Description                                                                      |
| ---------- | ---- | -------------------------------------------------------------------------------- |
| `accepted` | bool | Whether the cancel request was accepted (does not mean the task has stopped yet) |

### 5.7 Cancel

Cancel an in-flight streaming operation (`PROCESS_MONITOR`, `TASK_STATUS` subscription, `SHELL_*`) or an in-progress file transfer (`FILE_PUT`, `FILE_GET` — see `protocol.md` §8.4). For detached tasks, use `TASK_CANCEL` (§5.6) instead. Discrete request/response operations that are simply slow (e.g. a long `COMMAND_EXEC`) are bounded by their timeout, not by `CANCEL`.

**Request (`CANCEL`):**

| Field              | Type   | Description                                |
| ------------------ | ------ | ------------------------------------------ |
| `target_stream_id` | uint32 | The `stream_id` of the operation to cancel |

**Response:**

| Field       | Type | Description                                |
| ----------- | ---- | ------------------------------------------ |
| `cancelled` | bool | Whether the stream was found and cancelled |

The cancelled stream sends a final frame with `CONTINUATION` cleared (and `reason = "cancelled"` where the response schema has such a field).

## 6. Error Codes

All `ERROR` frames carry a protobuf payload with a code, message, and optional detail. The originating request is identified by the `stream_id` in the frame header (see `protocol.md` §10.2), not by a payload field.

| Code | Name                | Description                              |
| ---- | ------------------- | ---------------------------------------- |
| `0`  | `UNKNOWN`           | Unspecified error                        |
| `1`  | `NOT_FOUND`         | File, process, or resource not found     |
| `2`  | `PERMISSION_DENIED` | Insufficient permissions                 |
| `3`  | `ALREADY_EXISTS`    | File or resource already exists          |
| `4`  | `TIMEOUT`           | Operation timed out                      |
| `5`  | `NOT_SUPPORTED`     | Operation not supported by this daemon   |
| `6`  | `INVALID_REQUEST`   | Malformed or invalid request             |
| `7`  | `INTERNAL`          | Daemon internal error                    |
| `8`  | `BUSY`              | Daemon is too busy to accept the request |
| `9`  | `CANCELLED`         | Operation was cancelled                  |
| `10` | `CONNECTION_LOST`   | Downstream connection was lost           |

**ErrorInfo payload:**

| Field     | Type      | Description                              |
| --------- | --------- | ---------------------------------------- |
| `code`    | ErrorCode | Enum value from the table above          |
| `message` | string    | Short human-readable summary             |
| `detail`  | string    | Optional implementation-specific context |

## 7. Tasking Model

### 7.1 Synchronous (default)

The client sends a `REQUEST`, the daemon processes it, and sends a `RESPONSE`. The client blocks until the response arrives. All operations except Interactive Shell (§2.2) are exercisable with purely synchronous code — see [requirements.md §4.1](requirements.md#41-reference-implementation).

### 7.2 Detached Tasks

A client uses `DETACH` (§5.4) to start a background task. The task runs independently of the request/response cycle. The client can:

- Poll or subscribe to status via `TASK_STATUS` (§5.5)
- Cancel via `TASK_CANCEL` (§5.6)
- Receive critical errors via the `PRIORITY` flag on `ERROR` frames (`protocol.md` §10.2)

### 7.3 Task Pipelines

A chain of operations where each takes the output of the previous as input. Executed as a single request with an ordered list of steps.

**Request (`PIPELINE`):**

| Field            | Type                  | Description                                                   |
| ---------------- | --------------------- | ------------------------------------------------------------- |
| `steps`          | repeated PipelineStep | Ordered list of operations                                    |
| `error_priority` | ErrorPriority         | Escalation level for the entire pipeline (used when detached) |

**PipelineStep:**

| Field           | Type    | Description                                                        |
| --------------- | ------- | ------------------------------------------------------------------ |
| `opcode`        | uint32  | Operation opcode (from §Opcodes)                                   |
| `params`        | bytes   | Serialized request message for that opcode                         |
| `input_binding` | Binding | How the previous step's output feeds this step's input (see below) |

**Binding** — a small, closed set. There is no expression language and there is no implicit array→scalar reduction: bindings are **strictly scalar-to-scalar** and match only when the previous step's output field is of the required scalar type. Supported bindings:

| Value   | Meaning                                                                                         | Allowed when previous → current is |
| ------- | ----------------------------------------------------------------------------------------------- | ---------------------------------- |
| `NONE`  | Step ignores previous output                                                                    | any                                |
| `STDIN` | Previous step's `stdout` (bytes, scalar) becomes this step's stdin (via a pipe on the daemon)   | `COMMAND_EXEC` → `COMMAND_EXEC`    |
| `PATH`  | Previous step's output file path (string, scalar) becomes this step's `path`/`src`/`dest` field | `FILE_*` → `FILE_*`                |
| `PID`   | Previous step's scalar `pid` (uint32) becomes this step's `pid` field                           | `COMMAND_EXEC` → `PROCESS_*`       |
| `BYTES` | Previous step's response bytes (scalar) are placed in this step's `content` field               | `FILE_GET` → `FILE_CREATE`, etc.   |

Explicitly disallowed:

- `PROCESS_LIST` (whose output is `repeated ProcessInfo`) is **not** a valid `PID` source, because array-to-scalar has no well-defined semantics. To act on a set of PIDs, either issue multiple pipelines from the client after inspecting the list, or use `COMMAND_EXEC` with `pkill`/`xargs`.
- Any binding whose types don't match the table (e.g. `PATH` from `COMMAND_EXEC`, `PID` from `FILE_GET`) is rejected.

All bindings are validated statically at pipeline submission time and rejected with `INVALID_REQUEST` before any step runs. This keeps pipelines simple to implement (no runtime type conversion, no expression evaluator, no fan-out) and simple to debug.

`FILE_PUT` and `FILE_GET` in pipelines: the file bytes are held on the daemon's side between steps as a temporary file; the pipeline does not stream `FILE_CHUNK` frames to the client. Use `DETACH` if you need a background file transfer, not a pipeline.

**Response:**

| Field         | Type   | Description                              |
| ------------- | ------ | ---------------------------------------- |
| `completed`   | uint32 | Number of steps completed                |
| `total`       | uint32 | Total number of steps                    |
| `result`      | bytes  | Serialized result of the final step      |
| `failed_step` | uint32 | Index of the failed step (only on error) |
