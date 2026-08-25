# `code-bridge-v1`

`code-bridge-v1` is the frozen transport contract between a Code Mode host and
one `maelys-code-runner` process. The key words **MUST**, **MUST NOT**,
**SHOULD** and **SHOULD NOT** are normative.

## Transport and lifecycle

The transport is a connected `SOCK_STREAM` full-duplex byte stream installed
at child descriptor 3. Every message is one UTF-8 JSON object followed by
`\n`.

- Embedded NUL bytes and duplicate JSON object keys MUST be rejected.
- Frames larger than the runner's `max_frame_bytes` MUST be rejected.
- The runner MUST keep a bounded receive queue. Queue exhaustion is a fatal
  protocol failure and MUST interrupt generated code.
- Exactly one writer exists on each side of the stream.
- One runner process executes exactly one `execute` request.
- Every frame contains `"version":"code-bridge-v1"` and a string `type`.
- The runner emits at most one `execution.result` terminal frame.
- The host MUST drain the runner-to-host direction through EOF after the
  process exits so that the terminal frame cannot be lost.

The channel uses Executor's first-class `MAELYS_EXEC_IO_ISOLATED_DUPLEX`
contract at fd 3. `MAELYS_PRESERVE_FDS` is never used by Code Mode v1; it is
reserved for explicit additional channels at fd 4 and above.

## Execute and capability distribution

The host sends the first frame:

```json
{
  "version": "code-bridge-v1",
  "type": "execute",
  "executionId": "01J...",
  "code": "return await tools['math.double']({\"value\":21});",
  "tools": [
    {"name": "math.double", "description": "Double a number"}
  ],
  "limits": {
    "maxToolCalls": 50,
    "maxResultBytes": 524288,
    "timeoutMs": 5000
  }
}
```

`limits` and all three fields are required. `maxToolCalls` is a non-negative
integer; the other fields are positive integers. The host MUST resolve client,
provider, administrator and effect-mode policy before sending this frame.

`tools` is a capability allowlist for this execution, not a copy of the full
provider catalog. In `inspect` mode, for example, the host MUST omit every tool
whose effect is not `read`. Only `name` is consumed by the runner. Schemas,
effects and generated TypeScript declarations remain host concerns.

An omitted tool has no callable binding in the frozen, null-prototype `tools`
registry. The provider dispatcher MUST independently revalidate the tool name,
arguments, effect and current mode. Passing the complete catalog and relying
only on the dispatcher is non-conforming.

## Effective limits and readiness

The runner clamps every requested limit to its absolute command-line limit and
MUST report the values it retained before executing generated code:

```json
{
  "version": "code-bridge-v1",
  "type": "ready",
  "executionId": "01J...",
  "runnerVersion": "0.1.0",
  "effectiveLimits": {
    "maxToolCalls": 32,
    "maxResultBytes": 524288,
    "timeoutMs": 5000
  }
}
```

The effective limits are immutable for the execution and MUST NOT exceed the
requested values. A host MUST reject a `ready` frame that increases a requested
limit.

## Tool calls

```json
{
  "version": "code-bridge-v1",
  "type": "tool.call",
  "id": 1,
  "name": "math.double",
  "arguments": {"value": 21}
}
```

The host answers with the same numeric `id`:

```json
{
  "version": "code-bridge-v1",
  "type": "tool.result",
  "id": 1,
  "ok": true,
  "result": {"value": 42}
}
```

Provider errors are ordinary, catchable tool failures:

```json
{
  "version": "code-bridge-v1",
  "type": "tool.result",
  "id": 1,
  "ok": false,
  "error": {"code": "INVALID_ARGUMENT", "message": "value is required"}
}
```

V1 permits one outstanding tool call because the native bridge is synchronous.
IDs still make mismatches and future concurrency detectable.

## Completion and fatal conditions

Successful completion:

```json
{
  "version": "code-bridge-v1",
  "type": "execution.result",
  "executionId": "01J...",
  "ok": true,
  "result": {"value": 42},
  "metrics": {"elapsedMs": 3, "toolCalls": 1}
}
```

Tool-call exhaustion is fatal:

```json
{
  "version": "code-bridge-v1",
  "type": "execution.result",
  "executionId": "01J...",
  "ok": false,
  "error": {
    "kind": "limit",
    "code": "TOOL_CALL_LIMIT_EXCEEDED",
    "message": "tool call limit exceeded",
    "limit": 32,
    "attempted": 33
  },
  "metrics": {"elapsedMs": 3, "toolCalls": 32}
}
```

Tool-call exhaustion, timeout, cancellation, result-size exhaustion and
protocol violations are fatal execution conditions. The runner MUST record
them outside generated JavaScript and MUST NOT allow a `try/catch` to turn them
into successful completion. It MUST NOT emit the tool call that exceeds
`maxToolCalls`.

Heap and stack limits are hard QuickJS resource ceilings. QuickJS may expose
their exception to generated JavaScript, but catching it cannot increase or
disable the configured ceiling; the independent wall-clock timeout still
bounds subsequent execution.

Known error kinds are `cancelled`, `timeout`, `javascript`, `memory`, `result`,
`result_limit`, `limit` and `internal`.

## Cancellation and timeout ordering

The host may send:

```json
{
  "version": "code-bridge-v1",
  "type": "cancel",
  "executionId": "01J..."
}
```

A dedicated bridge reader records cancellation while QuickJS is busy. SIGINT
and SIGTERM use the same cooperative path.

The launcher MUST configure an outer deadline that leaves the runner enough
time to serialize and flush its terminal frame:

```text
runnerTimeoutMs + terminalDrainBudgetMs < providerDeadlineMs

providerDeadlineMs                    -> Executor sends SIGTERM
providerDeadlineMs + graceTimeoutMs   -> Executor sends SIGKILL
then forceTimeoutMs                   -> bounded containment confirmation
```

`graceTimeoutMs` and `forceTimeoutMs` are escalation durations, not absolute
deadlines. If the runner cannot stop cooperatively, Executor performs the
forced termination.
