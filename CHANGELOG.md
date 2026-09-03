# Changelog

## 0.1.1 — 2026-09-03

- Relicense from Apache-2.0 to the Mozilla Public License 2.0, the license
  of every Maelys repository. No code change.

## 0.1.0 — 2026-08-25

- Introduce a one-execution-per-process QuickJS-NG runner with no libc helpers,
  module loader, filesystem, process, environment or network bindings.
- Freeze `code-bridge-v1`, a bounded duplicate-key-rejecting JSON Lines
  protocol over Executor's isolated fd 3 channel.
- Add per-execution tool capability allowlists, effective heap/stack/time/frame,
  result and tool-call limits, cancellation and fatal native containment latches.
- Preserve ordinary provider errors as catchable JavaScript exceptions while
  keeping quota, timeout, cancellation, protocol and result-size failures fatal.
- Add runner, bridge conformance, sanitizer, ThreadSanitizer, fuzz and real
  Seatbelt/Bubblewrap Executor integration coverage.
- Ship deterministic macOS ARM64 and Linux x86-64/ARM64 archives with pinned
  QuickJS-NG and Jansson license texts, checksums and build provenance.
