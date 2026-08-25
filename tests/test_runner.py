#!/usr/bin/env python3

import json
import os
import select
import signal
import socket
import subprocess
import sys
import time
import unittest


RUNNER = os.path.abspath(sys.argv[1])
del sys.argv[1]


class Session:
    def __init__(self, *, arguments=None):
        parent, child = socket.socketpair()
        self.socket = parent
        child_fd = child.fileno()
        command = [RUNNER, "--fd", str(child_fd)]
        if arguments:
            command.extend(arguments)
        self.process = subprocess.Popen(
            command,
            pass_fds=(child_fd,),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        child.close()
        self.reader = parent.makefile("rb", buffering=0)
        self.closed = False

    def send(self, frame):
        payload = json.dumps(frame, separators=(",", ":")).encode("utf-8") + b"\n"
        self.socket.sendall(payload)

    def send_raw(self, payload):
        self.socket.sendall(payload)

    def receive(self, timeout=3.0):
        ready, _, _ = select.select([self.socket], [], [], timeout)
        if not ready:
            raise AssertionError("timed out waiting for runner frame")
        line = self.reader.readline()
        if not line:
            stderr = self.process.stderr.read().decode("utf-8", errors="replace")
            raise AssertionError(f"runner closed the bridge early: {stderr}")
        return json.loads(line)

    def execute(self, code, tools=None, execution_id="test", limits=None):
        requested_limits = limits or {
            "maxToolCalls": 64,
            "maxResultBytes": 512 * 1024,
            "timeoutMs": 5000,
        }
        self.send({
            "version": "code-bridge-v1",
            "type": "execute",
            "executionId": execution_id,
            "code": code,
            "tools": tools or [],
            "limits": requested_limits,
        })
        ready = self.receive()
        if ready.get("type") != "ready":
            raise AssertionError(f"expected ready, received {ready!r}")
        if ready.get("executionId") != execution_id:
            raise AssertionError(f"ready has the wrong execution id: {ready!r}")
        if not isinstance(ready.get("effectiveLimits"), dict):
            raise AssertionError(f"ready has no effective limits: {ready!r}")
        return ready

    def close(self, expected=0):
        if self.closed:
            return ""
        self.closed = True
        self.reader.close()
        try:
            self.socket.close()
        except OSError:
            pass
        returncode = self.process.wait(timeout=5)
        stderr = self.process.stderr.read().decode("utf-8", errors="replace")
        self.process.stderr.close()
        if returncode != expected:
            raise AssertionError(
                f"runner returned {returncode}, expected {expected}; stderr={stderr!r}"
            )
        return stderr

    def terminate(self):
        if self.closed:
            return
        self.closed = True
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.close()
        self.socket.close()
        self.process.stderr.close()


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.sessions = []

    def tearDown(self):
        for session in self.sessions:
            session.terminate()

    def session(self, **kwargs):
        session = Session(**kwargs)
        self.sessions.append(session)
        return session

    def test_structured_result_and_empty_capability_global(self):
        session = self.session()
        session.execute(
            "return {answer: 6 * 7, globals: {"
            "std: typeof std, os: typeof os, fs: typeof fs,"
            "fetch: typeof fetch, process: typeof process, require: typeof require}};"
        )
        result = session.receive()
        self.assertTrue(result["ok"])
        self.assertEqual(result["result"]["answer"], 42)
        self.assertEqual(set(result["result"]["globals"].values()), {"undefined"})
        self.assertEqual(result["metrics"]["toolCalls"], 0)
        session.close()

    def test_declared_tool_round_trip(self):
        session = self.session()
        session.execute(
            "const value = await tools['math.double']({value: 21});"
            "return {answer: value.result};",
            tools=[{"name": "math.double", "description": "Double a number"}],
        )
        call = session.receive()
        self.assertEqual(call, {
            "arguments": {"value": 21},
            "id": 1,
            "name": "math.double",
            "type": "tool.call",
            "version": "code-bridge-v1",
        })
        session.send({
            "version": "code-bridge-v1",
            "type": "tool.result",
            "id": call["id"],
            "ok": True,
            "result": {"result": 42},
        })
        result = session.receive()
        self.assertTrue(result["ok"])
        self.assertEqual(result["result"], {"answer": 42})
        self.assertEqual(result["metrics"]["toolCalls"], 1)
        session.close()

    def test_undeclared_tool_is_rejected_without_bridge_call(self):
        session = self.session()
        session.execute("return tools['system.exec']({});")
        result = session.receive()
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["kind"], "javascript")
        self.assertIn("not a function", result["error"]["message"])
        self.assertEqual(result["metrics"]["toolCalls"], 0)
        session.close()

    def test_non_object_tool_arguments_are_rejected(self):
        session = self.session()
        session.execute(
            "return tools['safe.read'](['not', 'an', 'object']);",
            tools=[{"name": "safe.read"}],
        )
        result = session.receive()
        self.assertFalse(result["ok"])
        self.assertIn("arguments must be an object", result["error"]["message"])
        session.close()

    def test_call_quota_is_enforced(self):
        session = self.session(arguments=["--max-calls", "1"])
        ready = session.execute(
            "await tools['counter.next']({});"
            "return tools['counter.next']({});",
            tools=[{"name": "counter.next"}],
        )
        self.assertEqual(ready["effectiveLimits"]["maxToolCalls"], 1)
        call = session.receive()
        session.send({
            "version": "code-bridge-v1", "type": "tool.result",
            "id": call["id"], "ok": True, "result": 1,
        })
        result = session.receive()
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["kind"], "limit")
        self.assertEqual(result["error"]["code"], "TOOL_CALL_LIMIT_EXCEEDED")
        self.assertEqual(result["error"]["limit"], 1)
        self.assertEqual(result["error"]["attempted"], 2)
        self.assertEqual(result["metrics"]["toolCalls"], 1)
        session.close()

    def test_mismatched_tool_response_is_rejected(self):
        session = self.session()
        session.execute(
            "return tools['safe.read']({});",
            tools=[{"name": "safe.read"}],
        )
        call = session.receive()
        session.send({
            "version": "code-bridge-v1", "type": "tool.result",
            "id": call["id"] + 1, "ok": True, "result": {},
        })
        result = session.receive()
        self.assertFalse(result["ok"])
        self.assertIn("protocol failure", result["error"]["message"])
        session.close()

    def test_timeout_interrupts_busy_javascript(self):
        session = self.session(arguments=["--timeout-ms", "50"])
        started = time.monotonic()
        session.execute("while (true) {}")
        result = session.receive(timeout=2.0)
        self.assertLess(time.monotonic() - started, 1.5)
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["kind"], "timeout")
        session.close()

    def test_bridge_cancel_interrupts_busy_javascript(self):
        session = self.session(arguments=["--timeout-ms", "5000"])
        session.execute("while (true) {}", execution_id="cancel-me")
        session.send({
            "version": "code-bridge-v1",
            "type": "cancel",
            "executionId": "cancel-me",
        })
        result = session.receive(timeout=2.0)
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["kind"], "cancelled")
        session.close()

    def test_signal_cancellation_interrupts_busy_javascript(self):
        session = self.session(arguments=["--timeout-ms", "5000"])
        session.execute("while (true) {}", execution_id="signal")
        session.process.send_signal(signal.SIGTERM)
        result = session.receive(timeout=2.0)
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["kind"], "cancelled")
        session.close()

    def test_stack_limit_is_enforced(self):
        session = self.session(arguments=["--stack-bytes", "524288"])
        session.execute("function recurse() { return recurse(); } return recurse();")
        result = session.receive()
        self.assertFalse(result["ok"])
        self.assertIn("stack", result["error"]["message"].lower())
        session.close()

    def test_memory_limit_is_enforced(self):
        session = self.session(arguments=[
            "--memory-bytes", "4194304", "--timeout-ms", "1000"
        ])
        session.execute(
            "const values = [];"
            "while (true) values.push('x'.repeat(4096));"
        )
        result = session.receive(timeout=2.0)
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["kind"], "memory")
        self.assertIn("out of memory", result["error"]["message"].lower())
        session.close()

    def test_result_size_limit_is_enforced(self):
        session = self.session(arguments=["--max-result-bytes", "128"])
        session.execute("return {value: 'x'.repeat(1024)};")
        result = session.receive()
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["kind"], "result_limit")
        session.close()

    def test_duplicate_json_keys_are_rejected(self):
        session = self.session()
        session.send_raw(
            b'{"version":"code-bridge-v1","type":"execute","type":"execute",'
            b'"code":"return 1;","tools":[]}\n'
        )
        self.assertEqual(session.process.wait(timeout=3), 2)
        stderr = session.process.stderr.read().decode("utf-8", errors="replace")
        self.assertIn("duplicate", stderr.lower())


if __name__ == "__main__":
    unittest.main(verbosity=2)
