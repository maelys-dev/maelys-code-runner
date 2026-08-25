# maelys-code-runner 0.1.0

This first release provides the native execution component for provider-owned
Code Mode.

The runner creates a fresh capability-empty QuickJS-NG context for one
execution, exposes only the tool names granted in that execution and sends all
tool calls back to the trusted host over the frozen `code-bridge-v1` fd 3
channel. Native limits and terminal latches prevent generated `try/catch` from
turning quota, timeout, cancellation, protocol or oversized-result failures
into success.

Production confinement requires Maelys Executor 0.13.0 or later with a real
Seatbelt or Bubblewrap backend, `NETWORK_NONE`, an empty environment and a
private temporary workspace. The runner alone is not an operating-system
sandbox.

Release archives contain the executable, documentation, Apache-2.0 project
license, third-party notice and the exact QuickJS-NG and Jansson license texts.
The release also publishes per-archive SHA-256 files, an aggregate
`SHA256SUMS` file and GitHub artifact provenance attestations.
