#!/usr/bin/env python3

import ctypes
import json
import os
import select
import socket
import subprocess
import sys
import time
import unittest


RUNNER = os.path.abspath(sys.argv[1])
del sys.argv[1]


class BridgeSession:
    def __init__(self, *runner_arguments):
        parent, child = socket.socketpair()
        self.socket = parent
        self.process = subprocess.Popen(
            [RUNNER, "--fd", str(child.fileno()), *runner_arguments],
            pass_fds=(child.fileno(),),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        child.close()
        self.reader = parent.makefile("rb", buffering=0)

    def send(self, frame):
        self.socket.sendall(
            json.dumps(frame, separators=(",", ":")).encode("utf-8") + b"\n"
        )

    def receive(self, timeout=3.0):
        ready, _, _ = select.select([self.socket], [], [], timeout)
        if not ready:
            raise AssertionError("timed out waiting for code-bridge-v1 frame")
        line = self.reader.readline()
        if not line:
            stderr = self.process.stderr.read().decode("utf-8", errors="replace")
            raise AssertionError(f"bridge closed before a frame: {stderr}")
        return json.loads(line)

    def execute(self, code, *, tools=None, limits=None, execution_id="conformance"):
        self.send({
            "version": "code-bridge-v1",
            "type": "execute",
            "executionId": execution_id,
            "code": code,
            "tools": tools or [],
            "limits": limits or {
                "maxToolCalls": 50,
                "maxResultBytes": 524288,
                "timeoutMs": 5000,
            },
        })
        return self.receive()

    def finish(self, expected=0):
        returncode = self.process.wait(timeout=5)
        stderr = self.process.stderr.read().decode("utf-8", errors="replace")
        if returncode != expected:
            raise AssertionError(
                f"runner returned {returncode}, expected {expected}; stderr={stderr!r}"
            )
        return stderr

    def terminate(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.close()
        self.socket.close()
        self.process.stderr.close()


class CodeBridgeV1Conformance(unittest.TestCase):
    def setUp(self):
        self.sessions = []

    def tearDown(self):
        for session in self.sessions:
            session.terminate()

    def session(self, *arguments):
        session = BridgeSession(*arguments)
        self.sessions.append(session)
        return session

    def test_ready_reports_clamped_effective_limits(self):
        session = self.session(
            "--max-calls", "32",
            "--max-result-bytes", "1024",
            "--timeout-ms", "200",
        )
        ready = session.execute(
            "return 1;",
            limits={
                "maxToolCalls": 50,
                "maxResultBytes": 4096,
                "timeoutMs": 1000,
            },
            execution_id="clamped",
        )
        self.assertEqual(ready["executionId"], "clamped")
        self.assertEqual(ready["runnerVersion"], "0.1.1")
        self.assertEqual(ready["effectiveLimits"], {
            "maxToolCalls": 32,
            "maxResultBytes": 1024,
            "timeoutMs": 200,
        })
        terminal = session.receive()
        self.assertTrue(terminal["ok"], terminal)
        session.finish()

    def test_execute_requires_explicit_limits(self):
        session = self.session()
        session.send({
            "version": "code-bridge-v1",
            "type": "execute",
            "executionId": "missing-limits",
            "code": "return 1;",
            "tools": [],
        })
        stderr = session.finish(expected=1)
        self.assertIn("execute.limits", stderr)

    def test_tool_limit_is_fatal_and_not_catchable(self):
        session = self.session("--max-calls", "1")
        ready = session.execute(
            "await tools['counter.next']({});"
            "try { await tools['counter.next']({}); } catch (_) {}"
            "return 'escaped';",
            tools=[{"name": "counter.next"}],
        )
        self.assertEqual(ready["effectiveLimits"]["maxToolCalls"], 1)
        call = session.receive()
        self.assertEqual(call["type"], "tool.call")
        session.send({
            "version": "code-bridge-v1",
            "type": "tool.result",
            "id": call["id"],
            "ok": True,
            "result": 1,
        })
        terminal = session.receive()
        self.assertFalse(terminal["ok"])
        self.assertEqual(terminal["error"]["kind"], "limit")
        self.assertEqual(terminal["error"]["code"], "TOOL_CALL_LIMIT_EXCEEDED")
        self.assertEqual(terminal["metrics"]["toolCalls"], 1)
        session.finish()

    def test_zero_tool_limit_emits_no_tool_call(self):
        session = self.session()
        session.execute(
            "try { await tools['safe.read']({}); } catch (_) {} return 'escaped';",
            tools=[{"name": "safe.read"}],
            limits={
                "maxToolCalls": 0,
                "maxResultBytes": 1024,
                "timeoutMs": 1000,
            },
        )
        terminal = session.receive()
        self.assertEqual(terminal["type"], "execution.result")
        self.assertEqual(terminal["error"]["code"], "TOOL_CALL_LIMIT_EXCEEDED")
        self.assertEqual(terminal["metrics"]["toolCalls"], 0)
        session.finish()

    def test_timeout_is_fatal_outside_generated_javascript(self):
        session = self.session("--timeout-ms", "40")
        session.execute(
            "try { while (true) {} } catch (_) {} return 'escaped';",
            limits={
                "maxToolCalls": 0,
                "maxResultBytes": 1024,
                "timeoutMs": 500,
            },
        )
        terminal = session.receive(timeout=2.0)
        self.assertFalse(terminal["ok"])
        self.assertEqual(terminal["error"]["kind"], "timeout")
        session.finish()

    def test_allowlist_omission_removes_callable_binding(self):
        session = self.session()
        session.execute(
            "return {kind: typeof tools['hermes.content.apply'],"
            " names: Object.keys(tools)};",
            tools=[{"name": "hermes.content.search"}],
        )
        terminal = session.receive()
        self.assertEqual(terminal["result"], {
            "kind": "undefined",
            "names": ["hermes.content.search"],
        })
        session.finish()

    def test_protocol_failure_is_fatal_and_not_catchable(self):
        session = self.session()
        session.execute(
            "try { await tools['safe.read']({}); } catch (_) {} return 'escaped';",
            tools=[{"name": "safe.read"}],
        )
        call = session.receive()
        session.send({
            "version": "code-bridge-v1",
            "type": "tool.result",
            "id": call["id"] + 1,
            "ok": True,
            "result": {},
        })
        terminal = session.receive()
        self.assertFalse(terminal["ok"])
        self.assertEqual(terminal["error"]["kind"], "internal")
        self.assertIn("protocol failure", terminal["error"]["message"])
        session.finish()

    def test_provider_tool_error_remains_catchable(self):
        session = self.session()
        session.execute(
            "try { await tools['safe.read']({}); }"
            " catch (_) { return {caught: true}; } return {caught: false};",
            tools=[{"name": "safe.read"}],
        )
        call = session.receive()
        session.send({
            "version": "code-bridge-v1",
            "type": "tool.result",
            "id": call["id"],
            "ok": False,
            "error": {"code": "DENIED", "message": "not permitted"},
        })
        terminal = session.receive()
        self.assertTrue(terminal["ok"])
        self.assertEqual(terminal["result"], {"caught": True})
        session.finish()

    def test_terminal_frame_is_drained_before_eof(self):
        session = self.session()
        session.execute("return {done: true};")
        terminal = session.receive()
        self.assertEqual(terminal["type"], "execution.result")
        self.assertTrue(terminal["ok"])
        session.finish()
        ready, _, _ = select.select([session.socket], [], [], 1.0)
        self.assertTrue(ready)
        self.assertEqual(session.reader.readline(), b"")

    def test_unsolicited_frame_flood_is_bounded_and_fatal(self):
        session = self.session("--timeout-ms", "5000")
        session.execute("while (true) {}")
        unsolicited = {
            "version": "code-bridge-v1",
            "type": "tool.result",
            "id": 1,
            "ok": True,
            "result": {},
        }
        for _ in range(9):
            session.send(unsolicited)
        terminal = session.receive(timeout=2.0)
        self.assertFalse(terminal["ok"])
        self.assertEqual(terminal["error"]["kind"], "internal")
        self.assertIn("protocol failure", terminal["error"]["message"])
        session.finish()

    def test_maximum_size_t_frame_limit_is_rejected_before_allocation(self):
        size_t_bits = 8 * ctypes.sizeof(ctypes.c_size_t)
        maximum_size_t = (1 << size_t_bits) - 1
        completed = subprocess.run(
            [RUNNER, "--max-frame-bytes", str(maximum_size_t)],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("out-of-range", completed.stderr)


if __name__ == "__main__":
    started = time.monotonic()
    unittest.main(verbosity=2)
