# TAZER API

> Living document. Last updated: 2026-09-25
>
> See also: [Requirements](requirements.md) · [Protocol Design](protocol.md) · [Design Overview](index.md)

## Overview

The TAZER API is a set of operations exposed by the agent over the [TAZER binary protocol](protocol.md). Requests and responses are serialized as Protocol Buffers. Each request carries a `request_id` (uint32) that the response echoes for correlation.

Not all agents implement every operation. On connection, the agent sends a `CAPABILITY` frame advertising which operations it supports. The client must check capabilities before calling an operation, or handle an "unsupported" error gracefully.

## 1. Agent Control

Operations for managing the agent itself.

### 1.1 Ping

Liveness check. Uses the protocol-level `PING`/`PONG` frames (no protobuf payload).

| Field | Value |
|---|---|
| Frame type | `PING` (0x06) / `PONG` (0x07) |
| Payload | None |

### 1.2 Version

Returns the agent's version, build info, and platform.

**Request:**

| Field | Type | Description |
|---|---|---|
| *(none)* | | Empty request body |

**Response:**

| Field | Type | Description |
|---|---|---|
| `version` | string | Semantic version (e.g. `"0.1.0"`) |
| `build` | string | Build identifier or commit hash |
| `platform` | string | OS and architecture (e.g. `"linux/amd64"`) |

### 1.3 Capabilities

Sent by the agent immediately on connection as a `CAPABILITY` frame. Not a request/response — the client receives it passively.

**Payload:**

| Field | Type | Description |
|---|---|---|
| `protocol_version` | uint32 | Protocol major.minor encoded as `(major << 16) \| minor` |
| `operations` | repeated string | List of supported operation names |
| `max_payload_sizes` | map<string, uint32> | Per-type payload size overrides |
| `compression` | repeated string | Supported compression algorithms (e.g. `["NONE", "LZ4"]`) |

### 1.4 Configuration Get

Retrieve the agent's current configuration.

**Request:**

| Field | Type | Description |
|---|---|---|
| `keys` | repeated string | Specific keys to retrieve; empty = return all |

**Response:**

| Field | Type | Description |
|---|---|---|
| `config` | map<string, string> | Key-value configuration pairs |

### 1.5 Configuration Update

Modify the agent's configuration at runtime.

**Request:**

| Field | Type | Description |
|---|---|---|
| `config` | map<string, string> | Key-value pairs to set |

**Response:**

| Field | Type | Description |
|---|---|---|
| `applied` | repeated string | Keys that were successfully applied |
| `rejected` | repeated RejectedKey | Keys that could not be applied, with reasons |

### 1.6 Restart

Restart the agent process. The TCP connection will be dropped and the client must reconnect.

**Request:**

| Field | Type | Description |
|---|---|---|
| `delay_ms` | uint32 | Delay before restart (0 = immediate) |

**Response:**

| Field | Type | Description |
|---|---|---|
| `acknowledged` | bool | Agent accepted the restart request |

### 1.7 Update

Replace the agent binary with a new version. The client uploads the new binary via file transfer, then issues this command to swap and restart.

**Request:**

| Field | Type | Description |
|---|---|---|
| `binary_path` | string | Path to the uploaded binary on the agent's filesystem |
| `checksum` | bytes | Expected checksum of the binary |
| `restart` | bool | Whether to restart immediately after update |

**Response:**

| Field | Type | Description |
|---|---|---|
| `success` | bool | Whether the update was applied |
| `message` | string | Status or error detail |

## 2. Command Execution

### 2.1 Execute Command

Run a command on the host and return its output.

**Request:**

| Field | Type | Description |
|---|---|---|
| `command` | string | The command to execute |
| `args` | repeated string | Arguments |
| `env` | map<string, string> | Additional environment variables |
| `working_dir` | string | Working directory (default: agent's cwd) |
| `timeout_ms` | uint32 | Timeout in milliseconds (0 = no timeout) |
| `as_user` | string | Run as this user (requires prior `RunAs`) |

**Response:**

| Field | Type | Description |
|---|---|---|
| `exit_code` | int32 | Process exit code |
| `stdout` | bytes | Standard output |
| `stderr` | bytes | Standard error |
| `timed_out` | bool | Whether the command was killed due to timeout |

### 2.2 Interactive Shell

Opens a bidirectional shell session. Requires a persistent connection. Input and output are streamed as sequential frames with the `CONTINUATION` flag.

**Request (open):**

| Field | Type | Description |
|---|---|---|
| `shell` | string | Shell to invoke (default: platform default) |
| `env` | map<string, string> | Additional environment variables |
| `as_user` | string | Run as this user |

**Response (open):**

| Field | Type | Description |
|---|---|---|
| `session_id` | uint32 | Identifier for this shell session |

After the session is opened, the client sends `REQUEST` frames with `session_id` containing stdin data, and the agent sends `RESPONSE` frames containing stdout/stderr data. Both sides use the `CONTINUATION` flag to indicate the session is still active.

**Request (close):**

| Field | Type | Description |
|---|---|---|
| `session_id` | uint32 | Session to close |

## 3. Process Management

### 3.1 Process List

Enumerate running processes on the host.

**Request:**

| Field | Type | Description |
|---|---|---|
| `filter` | string | Optional filter (e.g. process name substring) |

**Response:**

| Field | Type | Description |
|---|---|---|
| `processes` | repeated ProcessInfo | List of matching processes |

**ProcessInfo:**

| Field | Type | Description |
|---|---|---|
| `pid` | uint32 | Process ID |
| `name` | string | Process name |
| `user` | string | Owning user |
| `cpu_percent` | float | CPU usage |
| `memory_bytes` | uint64 | Memory usage |
| `state` | string | Running, sleeping, stopped, etc. |

### 3.2 Process Kill

Terminate a process.

**Request:**

| Field | Type | Description |
|---|---|---|
| `pid` | uint32 | Process ID |
| `signal` | int32 | Signal to send (default: platform's SIGTERM equivalent) |

**Response:**

| Field | Type | Description |
|---|---|---|
| `success` | bool | Whether the signal was delivered |

### 3.3 Process Info

Detailed information about a specific process.

**Request:**

| Field | Type | Description |
|---|---|---|
| `pid` | uint32 | Process ID |

**Response:**

| Field | Type | Description |
|---|---|---|
| `info` | ProcessInfo | Full process details |
| `command_line` | string | Full command line |
| `start_time` | uint64 | Process start time (Unix timestamp) |
| `open_files` | repeated string | Open file descriptors (if available) |

### 3.4 Process Monitor

Subscribe to ongoing status updates for a process. The agent sends periodic `RESPONSE` frames with the `CONTINUATION` flag until the process exits or the client cancels.

**Request:**

| Field | Type | Description |
|---|---|---|
| `pid` | uint32 | Process ID |
| `interval_ms` | uint32 | Update interval in milliseconds |

**Response (repeated, streamed):**

| Field | Type | Description |
|---|---|---|
| `info` | ProcessInfo | Current process state |
| `exited` | bool | True if the process has exited |
| `exit_code` | int32 | Exit code (only set when `exited` is true) |

The final response has the `CONTINUATION` flag cleared.

## 4. File Operations

All file transfers use the chunked `FILE_CHUNK` frame mechanism described in [Protocol Design §8](protocol.md#8-file-transfers). Metadata is exchanged via `REQUEST`/`RESPONSE` frames; the file bytes themselves are raw `FILE_CHUNK` frames.

### 4.1 Put File

Transfer a file from client to agent.

**Request:**

| Field | Type | Description |
|---|---|---|
| `dest` | string | Destination path on the agent |
| `size` | uint64 | Total file size in bytes |
| `permissions` | uint32 | File permissions (POSIX mode bits) |
| `overwrite` | bool | Whether to overwrite an existing file |

**Response (acknowledge):**

| Field | Type | Description |
|---|---|---|
| `ready` | bool | Agent is ready to receive chunks |

After acknowledgement, the client sends `FILE_CHUNK` frames. After the final chunk (no `CONTINUATION` flag), the agent sends a confirmation response:

**Response (confirmation):**

| Field | Type | Description |
|---|---|---|
| `bytes_written` | uint64 | Total bytes written |
| `checksum` | bytes | Checksum of the written file |

### 4.2 Get File

Transfer a file from agent to client.

**Request:**

| Field | Type | Description |
|---|---|---|
| `src` | string | Source path on the agent |

**Response (metadata):**

| Field | Type | Description |
|---|---|---|
| `size` | uint64 | Total file size in bytes |
| `permissions` | uint32 | File permissions |
| `checksum` | bytes | Checksum of the file |

Followed by `FILE_CHUNK` frames containing the file data.

### 4.3 Create File

Create an empty file or a file with inline content (for small files that don't warrant chunked transfer).

**Request:**

| Field | Type | Description |
|---|---|---|
| `path` | string | File path |
| `content` | bytes | Optional content (must fit in a single `REQUEST` frame) |
| `permissions` | uint32 | File permissions |

**Response:**

| Field | Type | Description |
|---|---|---|
| `success` | bool | Whether the file was created |

### 4.4 Delete File

Remove a file from the agent's filesystem.

**Request:**

| Field | Type | Description |
|---|---|---|
| `path` | string | File path |

**Response:**

| Field | Type | Description |
|---|---|---|
| `success` | bool | Whether the file was deleted |

### 4.5 Stat File

Retrieve metadata about a file.

**Request:**

| Field | Type | Description |
|---|---|---|
| `path` | string | File path |

**Response:**

| Field | Type | Description |
|---|---|---|
| `size` | uint64 | File size in bytes |
| `permissions` | uint32 | File permissions |
| `owner` | string | File owner |
| `modified` | uint64 | Last modified time (Unix timestamp) |
| `created` | uint64 | Creation time (Unix timestamp) |
| `is_dir` | bool | Whether the path is a directory |

### 4.6 Modify File Permissions

Change permissions on an existing file.

**Request:**

| Field | Type | Description |
|---|---|---|
| `path` | string | File path |
| `permissions` | uint32 | New POSIX mode bits |

**Response:**

| Field | Type | Description |
|---|---|---|
| `success` | bool | Whether permissions were changed |

## 5. Advanced Operations

### 5.1 Run As

Set the identity for subsequent operations. Platform-dependent (POSIX `setuid`/`seteuid`, Windows impersonation).

**Request:**

| Field | Type | Description |
|---|---|---|
| `user` | string | Username to assume |

**Response:**

| Field | Type | Description |
|---|---|---|
| `success` | bool | Whether the identity switch succeeded |
| `effective_user` | string | The identity now in effect |

### 5.2 Timeout

Set the default timeout for operations on this connection.

**Request:**

| Field | Type | Description |
|---|---|---|
| `timeout_ms` | uint32 | Default timeout in milliseconds (0 = no timeout) |

**Response:**

| Field | Type | Description |
|---|---|---|
| `previous_ms` | uint32 | The previous timeout value |

### 5.3 Log

Retrieve agent log entries.

**Request:**

| Field | Type | Description |
|---|---|---|
| `lines` | uint32 | Number of recent log lines to retrieve |
| `since` | uint64 | Only return entries after this Unix timestamp |
| `level` | string | Minimum log level filter (`DEBUG`, `INFO`, `WARN`, `ERROR`) |

**Response:**

| Field | Type | Description |
|---|---|---|
| `entries` | repeated LogEntry | Log entries |

**LogEntry:**

| Field | Type | Description |
|---|---|---|
| `timestamp` | uint64 | Unix timestamp |
| `level` | string | Log level |
| `message` | string | Log message |

### 5.4 Detach

Execute a task in the background. The agent runs it asynchronously and the client can optionally monitor it or set an error priority for cross-task escalation.

**Request:**

| Field | Type | Description |
|---|---|---|
| `command` | string | Command to execute |
| `args` | repeated string | Arguments |
| `monitor` | bool | Whether to enable status monitoring |
| `error_priority` | string | Error escalation level: `NONE`, `NORMAL`, `CRITICAL` |

**Response:**

| Field | Type | Description |
|---|---|---|
| `task_id` | uint32 | Identifier for the detached task |

A `CRITICAL` error priority means the agent will send an `ERROR` frame with the `PRIORITY` flag set if this task fails, interrupting whatever response the client is currently waiting on. See [Protocol Design §4.3](protocol.md#43-flags-byte-1).

## 6. Error Codes

All `ERROR` frames carry a protobuf payload with a code and message.

| Code | Name | Description |
|---|---|---|
| `0` | `UNKNOWN` | Unspecified error |
| `1` | `NOT_FOUND` | File, process, or resource not found |
| `2` | `PERMISSION_DENIED` | Insufficient permissions |
| `3` | `ALREADY_EXISTS` | File or resource already exists |
| `4` | `TIMEOUT` | Operation timed out |
| `5` | `NOT_SUPPORTED` | Operation not supported by this agent |
| `6` | `INVALID_REQUEST` | Malformed or invalid request |
| `7` | `INTERNAL` | Agent internal error |
| `8` | `BUSY` | Agent is too busy to accept the request |
| `9` | `CANCELLED` | Operation was cancelled |
| `10` | `CONNECTION_LOST` | Downstream connection was lost |

## 7. Tasking Model

### 7.1 Synchronous (default)

The client sends a `REQUEST`, the agent processes it, and sends a `RESPONSE`. The client blocks until the response arrives.

### 7.2 Detached Tasks

A client uses the Detach operation (§5.4) to start a background task. The task runs independently of the request/response cycle. The client can:

- Check status via Process Monitor (§3.4) if `monitor` was set
- Receive critical errors via the `PRIORITY` flag on `ERROR` frames

### 7.3 Task Pipelines

A chain of tasks where each takes the output of the previous as input. Defined as a single request containing an ordered list of operations. The agent executes them sequentially, reporting back only on error or successful completion of all tasks.

**Request:**

| Field | Type | Description |
|---|---|---|
| `steps` | repeated PipelineStep | Ordered list of operations |
| `error_priority` | string | Escalation level for the entire pipeline |

**PipelineStep:**

| Field | Type | Description |
|---|---|---|
| `operation` | string | Operation name |
| `params` | bytes | Serialized operation request (protobuf) |

**Response:**

| Field | Type | Description |
|---|---|---|
| `completed` | uint32 | Number of steps completed |
| `total` | uint32 | Total number of steps |
| `result` | bytes | Serialized result of the final step |
| `failed_step` | uint32 | Index of the failed step (only on error) |
