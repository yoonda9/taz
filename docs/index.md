# TAZER Design

TAZER is intended to be a well defined API such that both the agent and client can be implemented in any language.

The TAZER reference agent is written in C in order to be able to support *any* test host without requiring redesigning the API or communication protocol.

## Agent Characteristics and Requirements

* Must be able to be ported to run on most platforms from the last decade.
* Must be stable and reliable
* Must not have runtime dependencies aside from the platform's C standard library and C runtime.
* Handle asyncronous tasking

## Client-Agent Communication

The command and control client (c3) communicates with the agent via RPC over HTTP.

### Communication Protocol

* HTTP >= 1.1 provides request/response semantics, status codes, chunking, paths, etc
* HTTP does not require persistent connections *but can* be upgraded to provide full duplex bidirectional persistent connections via Websockets.
* HTTP v1.x C libraries are widely available and HTTP/2 reliable implementations do exist
* HTTP connections can be upgraded to Websockets where available for full duplex when required.

### Message Format / Serialization

Protobufs will be used for the RPC message format.

* Widely used with popular C library available (nanopb)
* Supports many languages including C and Python
* Provides consistency and single source of truth for procedure definitions by requiring schemas
    * Allows backwards/forwards compatibility and version control thereof

### Tasking

#### Concurrency

There should be *no* requirement for the client to handle concurrency. Requiring any form of concurrency may be detrimental for test writers to create simple and easily understandable tests.
This is not to say that concurrency should not be a feature on the client side, but simply that *all* functionality needs to be able to be exercised without concurrency enabled.

The agent *may* recieve asyncronous tasking even though the client does *not* handle asynchronous operations.
For example, the agent may need to start a process and monitor its status while also being tasked to upload a file.
In terms of a synchronous test framework, the client should be able to mark each task as able to raise failures globally.
If so, a failure caught in task a, can send an error response back on the channel reserved for the response for task b.

```python
some_proc = tazer.run(cmd="some-process", detach=True, monitor=True, error_priority="critical")
try:
    file = tazer.get(src="some-path.txt", dest="/tmp/some-path.txt")
except Exception as err:
    ...
```

#### Task Pipelines

Task pipelines can be used to define a chain of tasks which take the output of the previous task as the input to its task such that the agent only has to report back to the client upon error of a task or successful completion of all tasks therein.
