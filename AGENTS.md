# Repository instructions

`maelys-code-runner` executes untrusted generated JavaScript. Preserve the
capability boundary on every change.

- Never link `quickjs-libc` or expose `std`, `os`, filesystem, process,
  environment, network or native module loading APIs.
- Keep QuickJS-NG and Jansson revisions immutable and update
  `THIRD_PARTY_NOTICES.md` with dependency changes.
- Treat the AST or source inspection as diagnostics only; QuickJS isolation,
  runtime limits and the Executor sandbox are the enforcement layers.
- Keep `code-bridge-v1` duplicate-key rejecting, bounded and single-writer.
- Any bridge change requires matching protocol documentation and integration
  tests.
- Run `make check`, `make asan-ubsan`, `make tsan` and `make fuzz-smoke` in separate build
  directories before release.
- Do not add an MCP implementation or provider-specific Hermes behavior here.
- Do not claim OS-level filesystem or network isolation without a real
  Maelys Executor integration test using a confining backend.
