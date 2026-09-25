# TAZER Requirements

> Living document. Last updated: 2026-09-25

## 1. Purpose

TAZER is a test orchestration tool whose primary purpose is to facilitate tests running on remote hosts. It covers all three stages of a test run:

- **Before:** Prepare the host for the test
- **During:** Monitor the host and processes
- **After:** Validate the state of the host

TAZER is defined as a **protocol and API specification** such that both the agent and client can be implemented in any language.

## 2. Guiding Principles

1. **Simplicity** — Fewer moving parts over maximum flexibility. The protocol and implementations should be straightforward to understand, implement, and debug.
2. **Portability** — The protocol must be implementable on full-OS platforms (Linux, macOS, Windows) and have a clear path to constrained environments (RTOS, bare-metal).
3. **No security mechanisms** — TAZER operates in trusted test environments. Authentication, encryption, and authorization are out of scope.
4. **Schema-first** — The protocol is the contract. Implementations are interchangeable as long as they speak the protocol correctly.

## 3. Agent Requirements

### 3.1 Reference Implementation

- Written in **C**
- Targets **Linux, macOS, and Windows** natively (POSIX + Win32)
- No runtime dependencies beyond the platform's C standard library, C runtime, and libuv
- Must be deployable as a single statically-linked binary with no shared library dependencies on the test host
- Must be stable and reliable under sustained operation
- Must handle asynchronous tasking (multiple concurrent tasks)

### 3.2 Portability

- The protocol must be feasible to implement on RTOS and bare-metal targets (FreeRTOS, Zephyr, bare ARM Cortex-M, etc.) as a **separate implementation** sharing the same protocol
- RTOS/embedded agents implement a **subset** of the API surface appropriate to the target's capabilities
- A **capability handshake** at connection time advertises what the agent supports, so the client adapts dynamically

### 3.3 Licensing

All dependencies must use **permissive licenses** (MIT, BSD, Apache-2.0, or equivalent). Copyleft licenses (GPL, LGPL) are not acceptable.

## 4. Client Requirements

### 4.1 Reference Implementation

- Written in **Python** (≥ 3.12)
- No concurrency requirement — all functionality except Interactive Shell must be exercisable with purely synchronous code
- Concurrency is an optional enhancement, never a prerequisite
- **Exception:** Interactive Shell (`SHELL_OPEN`/`SHELL_INPUT`/`SHELL_CLOSE`) is inherently bidirectional-streaming and cannot be exercised synchronously. It is an optional capability — the reference client MAY implement it under a concurrent submodule, and agents MAY omit it. All other operations exposed by an agent MUST be usable from a purely synchronous client.

### 4.2 Error Handling

- A failure in a detached/background task on the agent can propagate to the client's current synchronous call
- Tasks with elevated error priority can interrupt unrelated in-flight responses (cross-task error escalation)
- On the wire, error priority is a single bit (`CRITICAL` vs `NONE`); intermediate priorities are not representable

## 5. API Surface

The agent exposes the following functionality. RTOS agents may implement a subset, declared via capability handshake.

### 5.1 Agent Control

| Operation | Description |
|---|---|
| Ping | Liveness check |
| Version | Agent version and build info |
| Capabilities | What this agent supports (returned at connection time) |
| Configuration Get | Read agent configuration |
| Configuration Update | Modify agent configuration |
| Restart | Restart the agent process |
| Update | Update the agent binary |

### 5.2 Command Execution

| Operation | Description |
|---|---|
| Execute Command | Run a command and return output |
| Interactive Shell | Bidirectional shell session (requires persistent connection) |

### 5.3 Process Management

| Operation | Description |
|---|---|
| Process List | Enumerate running processes |
| Process Kill | Terminate a process |
| Process Info | Detailed info on a specific process |
| Process Monitor | Ongoing status reporting for a process |

### 5.4 File Operations

| Operation | Description |
|---|---|
| Put File | Transfer file from client to agent (upload) |
| Get File | Transfer file from agent to client (download) |
| Create File | Create an empty file or file with content |
| Delete File | Remove a file |
| Stat File | File or directory metadata (size, permissions, timestamps, kind) |
| Modify Permissions | Change file permissions |
| Make Directory | Create a directory (with `-p` semantics available) |
| List Directory | Enumerate directory entries |
| Remove Directory | Remove a directory (optionally recursive) |

Must support **large file transfers** — files that exceed available memory must stream in chunks without requiring the full file in memory on either side.

File integrity is verified with CRC32C checksums returned in the transfer metadata.

### 5.5 Advanced

| Operation | Description |
|---|---|
| Run As | Execute subsequent operations as a different user |
| Timeout | Set/get operation timeout |
| Log | Retrieve agent logs |
| Detach | Fire-and-forget task execution with optional monitoring |

## 6. Tasking Model

### 6.1 Synchronous (default)

Client sends a request, blocks until the response arrives.

### 6.2 Detached Tasks

Client marks a task as detached. The agent runs it in the background. The client receives a `task_id` and can poll for status, cancel it, or register for error escalation. Any operation (command, file transfer, or pipeline) may be detached — not just command execution.

### 6.3 Error Priority

Detached tasks can be assigned an error priority. A `CRITICAL` priority failure can interrupt any in-flight response on the same connection.

### 6.4 Cancellation

Streaming operations (Process Monitor, subscribed Task Status, Interactive Shell) can be cancelled by the client. Detached tasks can be cancelled by `task_id`.

### 6.5 Task Pipelines

A chain of tasks where each takes the output of the previous as input, subject to a closed set of typed bindings (stdin, path, pid, bytes). No runtime expression language. The agent executes the full pipeline, reporting back only on error or successful completion of all tasks. Reduces client-agent round trips.

## 7. Communication

### 7.1 Transport

- **TCP** over IPv4/IPv6
- No TLS (trusted environment)
- Persistent connections with explicit lifecycle management

### 7.2 Protocol

- Custom binary framing protocol (see [Protocol Design](protocol.md))
- Fixed 12-byte header per message: type (1B) + flags (1B) + opcode (2B LE) + length (4B LE) + stream_id (4B LE)
- Opcode routes dispatch on REQUEST frames; `stream_id` correlates responses, errors, and streamed data (including raw `FILE_CHUNK` frames) to the originating request
- Designed for implementation on constrained devices with static memory allocation

### 7.3 Serialization

- **Protocol Buffers** for structured messages
- **nanopb** on constrained C targets; standard protobuf libraries elsewhere
- `.proto` schemas are the single source of truth for all message definitions
- All message types define `max_size` constraints for static buffer allocation

### 7.4 File Transfers

- Streamed as sequential fixed-size chunks over the framing protocol
- Chunk size negotiable but bounded by protocol-defined maximum
- No requirement to hold an entire file in memory

## 8. Non-Requirements

The following are explicitly **out of scope**:

- Authentication and authorization
- Encryption (TLS/DTLS)
- Agent discovery or registration protocols
- Multi-agent orchestration (one client talks to one agent)
- Web browser interface
- Backwards compatibility with any prior TAZER version (pre-1.0)
