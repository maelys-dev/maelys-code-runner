# Integration with Maelys Warden

`maelys-code-runner` does not create its own OS sandbox. The launcher must use a
sealed Executor plan with confinement required.

## Required execution shape

```text
executable       absolute path to maelys-code-runner
environment      empty or explicit
working dir      isolated temporary directory
I/O              MAELYS_EXEC_IO_ISOLATED_DUPLEX
child protocol   fd 3
stderr           captured for diagnostics
network          MAELYS_EXECUTOR_NETWORK_NONE
filesystem       runner + runtime libraries read-only; temporary dir writable
timeouts         bounded graceful and forced stop
```

Warden's low-level Executor engine provides these pieces:

```c
maelys_execution_request_set_io_mode(
    request, MAELYS_EXEC_IO_ISOLATED_DUPLEX, 3, &error);

maelys_execution_request_set_environment_mode(
    request, MAELYS_EXEC_ENV_EMPTY, &error);

maelys_executor_plan_set_network(
    plan, MAELYS_EXECUTOR_NETWORK_NONE, &error);
```

After spawn, the trusted host takes `MAELYS_EXEC_STREAM_PROTOCOL`, sends the
`execute` frame, services `tool.call` frames through its provider dispatcher,
and consumes the terminal `execution.result`.

## Node integration

`@maelys/warden` exposes the isolated protocol and lifecycle through
`spawnIsolated()`:

```ts
const execution = spawnIsolated([runnerPath], {
  policy: profile("untrusted")
});

execution.stderr.on("data", consumeDiagnostic);
execution.protocol.write(JSON.stringify(executeFrame) + "\n");
const lifecycle = await execution.wait();
```

The SDK creates separate control, workspace and sandbox-temp directories. It
passes the sandbox temp as the launcher's `TMPDIR`, requests an empty child
environment and keeps receipts outside the writable sandbox root. The CLI
relays opaque stdin/stdout bytes to fd 3, drains the terminal frame and performs
bounded graceful/forced shutdown. Callers must continuously drain both the
protocol and diagnostic streams.

The reproducible cross-project check is:

```sh
make -C /path/to/maelys-warden code-runner-e2e \
  CODE_RUNNER=/path/to/maelys-code-runner/build/release/maelys-code-runner
```
