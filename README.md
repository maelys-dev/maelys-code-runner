# Maelys Code Runner

`maelys-code-runner` is a small native JavaScript runner for provider-owned
Code Mode. It embeds QuickJS-NG without `quickjs-libc` and exposes no filesystem,
process, environment or network API to generated code.

It is deliberately not an MCP server and does not know Hermes. A trusted host
sends one execution over `code-bridge-v1`; generated JavaScript may call only
the tool names declared in that execution. Every call returns to the host over
an isolated full-duplex descriptor, normally child `fd 3`.

```text
MCP provider
    │
    │ code-bridge-v1
    ▼
maelys-warden
    │ NETWORK_NONE + filesystem-minimal plan
    ▼
maelys-code-runner
    └── QuickJS context: tools[declaredName](arguments)
```

## Security boundary

The QuickJS context starts without `std`, `os`, `fs`, `fetch`, `require`,
`process.env` or a module loader. The runner enforces memory, stack, time,
frame, result and tool-call limits. It validates duplicate-free JSON and tool
response correlation.

This capability-empty context is one layer, not the complete OS boundary.
Strong `NETWORK_NONE`, filesystem denial and forced process-tree termination
must be supplied by Maelys Warden. See [security model](SECURITY.md).

## Build and test

QuickJS-NG `v0.15.1` and Jansson `v2.15.1` are fetched by immutable commit.
Neither dependency must be installed globally.

```sh
make check
make asan-ubsan
make tsan
make fuzz-smoke
make install-check
```

The binary defaults to:

| Limit | Default |
|---|---:|
| QuickJS heap | 64 MiB |
| QuickJS stack | 2 MiB |
| wall-clock execution | 5 s |
| tool calls | 64 |
| incoming/outgoing frame | 1 MiB |
| final JavaScript result | 512 KiB |

Run `build/release/maelys-code-runner --help` for absolute hard-limit
overrides. The host declares lower per-execution limits in the `execute` frame;
the runner reports the clamped values in `ready`. Generated JavaScript cannot
alter either set of limits.

## JavaScript API

The runner installs a frozen, null-prototype `tools` registry. Tool names are
kept verbatim, so names containing dots use bracket notation:

```js
const page = await tools["hermes.content.search"]({
  query: "Pour prolonger",
  collection: "parcours"
});

return {
  count: page.items.length,
  paths: page.items.map(item => item.path)
};
```

`await` is accepted even though bridge calls are synchronous in v1. A script is
evaluated as the body of an async function and must `return` its final JSON
value.

## Documentation

- [QuickJS runner and capability boundary](docs/quickjs-runner.md)
- [code-bridge-v1](docs/code-bridge-v1.md)
- [Warden integration](docs/warden-integration.md)
- [Security policy](SECURITY.md)
- [Release process](docs/releasing.md)

## License

Mozilla Public License 2.0 ([LICENSE](LICENSE)), like every Maelys repository.
Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
