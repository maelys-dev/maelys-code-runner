# Security model

## Trust boundaries

Generated JavaScript, its arguments and all tool results are untrusted. The MCP
provider, its tool catalogue, limit values and sealed Executor policy are
trusted control-plane inputs.

The runner provides a capability-empty JavaScript realm and a narrow bridge.
Maelys Executor provides the OS boundary. Both layers are required.

## Runner guarantees

- QuickJS-NG is embedded without `quickjs-libc`.
- No module loader is installed.
- `std`, `os`, `fs`, `fetch`, `require` and `process` are absent.
- Tool calls are limited to names in the execute-frame catalogue.
- Tool arguments must serialize to a JSON object.
- Bridge JSON rejects duplicate keys, NUL bytes and oversized frames.
- Tool responses must match the outstanding numeric call ID.
- Heap, stack, deadline, result size and call count are bounded.
- Effective per-execution limits are reported before generated code starts.
- Tool-call and result-size limits, timeout, cancellation and protocol failure
  are checked outside JavaScript and cannot be converted into success by
  `try/catch`. Heap and stack ceilings remain enforced even if their QuickJS
  exception is caught.
- A dedicated reader makes cancellation observable during busy JavaScript.

## Executor requirements

The runner cannot prove OS-level network or filesystem denial by itself. A
production launcher must require a Bubblewrap or Seatbelt backend, select
`NETWORK_NONE`, use an empty environment, mount only required runtime files and
apply bounded graceful/forced shutdown.

The POSIX backend is suitable for development tests only because it does not
advertise strong confinement.

## Deliberate non-capabilities

- no arbitrary process execution;
- no direct file access;
- no environment access;
- no direct or mediated network access in v1;
- no dynamic native module loading;
- no QuickJS bytecode input;
- no persistent JavaScript context across executions.

Report security issues privately to the repository maintainers. Do not include
secrets, production prompts or private tool results in a report.
