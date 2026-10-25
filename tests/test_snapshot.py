#!/usr/bin/env python3
"""Black-box acceptance: real LD_PRELOAD, processes, signals and parsed snapshots."""
import os
import re
import select
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "build/libmem_snapshot.so"


class Target:
    def __init__(self, binary="fixture", **options):
        self.temp = tempfile.TemporaryDirectory(prefix="mem-snapshot-test-")
        self.path = Path(self.temp.name) / "snapshot.log"
        env = dict(os.environ, LD_PRELOAD=str(LIB), MS_OUTPUT=str(self.path))
        env.update(options)
        self.proc = subprocess.Popen([str(ROOT / "build" / binary)], env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, bufsize=0)
        self.buffer = b""
        self.sequence = 0
        assert self.line().startswith("READY ")

    def line(self, timeout=20):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            wait = deadline - time.monotonic()
            if wait <= 0 or not select.select([self.proc.stdout], [], [], max(0, wait))[0]:
                raise AssertionError("target response timed out")
            data = os.read(self.proc.stdout.fileno(), 4096)
            if not data:
                raise AssertionError(f"target exited: {self.proc.poll()}, {self.proc.stderr.read()!r}")
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode()

    def command(self, command):
        self.proc.stdin.write((command + "\n").encode())
        lines = []
        while True:
            line = self.line()
            lines.append(line)
            if line.startswith("OK "):
                ptr = re.search(r"ptr=(\S+)", line)[1]
                return ptr, "\n".join(lines)

    def snap(self):
        os.kill(self.proc.pid, signal.SIGUSR2)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            text = self.path.read_text() if self.path.exists() else ""
            blocks = re.findall(r"BEGIN seq=(\d+) (.*?)\nEND seq=\1\n", text, re.S)
            if blocks and int(blocks[-1][0]) > self.sequence:
                self.sequence = int(blocks[-1][0])
                body = blocks[-1][1]
                header = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", body.splitlines()[0])}
                records = {}
                for ident, ptr, size, depth in re.findall(
                        r"^ALLOC id=(\d+) ptr=(\S+) size=(\d+) depth=(\d+)$", body, re.M):
                    assert ptr not in records, f"duplicate live pointer {ptr}"
                    records[ptr] = (int(size), int(ident), int(depth))
                assert len(records) == header["count"]
                assert sum(r[0] for r in records.values()) == header["bytes"]
                assert len({r[1] for r in records.values()}) == len(records)
                return header, records, body
            if self.proc.poll() is not None:
                raise AssertionError("target died during snapshot")
            time.sleep(0.02)
        raise AssertionError("snapshot timed out")

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.write(b"quit\n")
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            stream.close()
        self.temp.cleanup()


class SnapshotTests(unittest.TestCase):
    def target(self, **options):
        t = Target(**options)
        self.addCleanup(t.close)
        return t

    def test_allocation_free_cross_thread_and_symbols(self):
        t = self.target()
        p, _ = t.command("malloc 0 123")
        q, _ = t.command("calloc 1 7 19")
        header, records, text = t.snap()
        self.assertEqual(records[p][0], 123)
        self.assertEqual(records[q][0], 133)
        self.assertIn("symbol=allocation_site", text)
        source = subprocess.check_output(
            ["python3", str(ROOT / "tools/symbolize.py"), str(t.path)], text=True, timeout=30)
        self.assertRegex(source, r"SOURCE allocation_site at .*tests/fixture.c:\d+")
        self.assertEqual(header["dropped"], 0)
        self.assertEqual(t.path.stat().st_mode & 0o777, 0o600)
        t.command("free 0")
        t.command("crossfree 1")
        _, records, _ = t.snap()
        self.assertNotIn(p, records)
        self.assertNotIn(q, records)
        t.command("free 2")  # free(NULL), errno preserved in fixture

    def test_realloc_failure_success_and_zero(self):
        t = self.target()
        p, _ = t.command("malloc 0 113")
        _, before, _ = t.snap()
        unchanged, result = t.command(f"realloc 0 {(1 << 64) - 1}")
        self.assertIn("result=(nil) errno=12", result)
        self.assertEqual(unchanged, p)
        _, after, _ = t.snap()
        self.assertEqual(before[p], after[p])  # ID, stack depth and size retained
        for size in (64, 4 * 1024 * 1024, 256):
            p, _ = t.command(f"realloc 0 {size}")
            _, records, _ = t.snap()
            self.assertEqual(records[p][0], size)
        t.command("realloc 0 0")
        self.assertNotIn(p, t.snap()[1])
        p, _ = t.command("realloc 1 171")
        self.assertEqual(t.snap()[1][p][0], 171)
        t.command("free 1")

    def test_zero_overflow_alignment_and_usable(self):
        t = self.target()
        p, _ = t.command("malloc 0 0")
        if p != "(nil)": self.assertEqual(t.snap()[1][p][0], 0)
        t.command("free 0")
        p, _ = t.command(f"calloc 0 {(1 << 64) - 1} 2")
        self.assertEqual(p, "(nil)")
        p, result = t.command("aligned 0 123 3")
        self.assertIn("rc=22", result)
        self.assertEqual(p, "(nil)")
        for command in ("aligned 0 123 64", "alignalloc 0 128 64",
                        "memalign 0 321 256", "valloc 0 123", "pvalloc 0 123"):
            p, _ = t.command(command)
            self.assertIn(p, t.snap()[1])
            t.command("usable 0")
            t.command("free 0")
            self.assertNotIn(p, t.snap()[1])
        p, _ = t.command("malloc 0 15")
        t.command("sizedfree 0")
        self.assertNotIn(p, t.snap()[1])

    def test_concurrency_and_fork(self):
        t = self.target()
        t.command("stress")
        for _ in range(12):
            self.assertEqual(t.snap()[0]["dropped"], 0)
            time.sleep(0.25)
        t.command("fork")
        t.command("stop")
        _, _, text = t.snap()
        self.assertNotIn("symbol=allocation_site", text)
        p, _ = t.command("malloc 0 321")
        self.assertEqual(t.snap()[1][p][0], 321)
        t.command("free 0")

    def test_capacity_limit_and_reuse(self):
        t = self.target(MS_DEPTH="0", MS_CAPACITY_PER_SHARD="256")
        t.command("bulk 0 30000")
        header, records, _ = t.snap()
        self.assertGreater(header["dropped"], 0)
        self.assertLessEqual(len(records), 64 * 256)
        self.assertTrue(all(r[2] == 0 for r in records.values()))
        t.command("bulkfree")
        p, _ = t.command("malloc 0 999")
        self.assertEqual(t.snap()[1][p][0], 999)
        t.command("free 0")

    def test_disabled_modes(self):
        for options, expected in [({"MS_DEPTH": "33"}, "invalid configuration"),
                                  ({"MS_CAPACITY_PER_SHARD": "257"}, "invalid configuration"),
                                  ({"MS_OUTPUT": "/nonexistent-ms-dir/file"}, "cannot create"),
                                  ({"TEST_SIGNAL_CONFLICT": "1"}, "SIGUSR2 already in use")]:
            with self.subTest(options=options):
                t = self.target(**options)
                t.command("malloc 0 55")
                t.command("free 0")
                t.proc.stdin.write(b"quit\n")
                self.assertEqual(t.proc.wait(timeout=5), 0)
                self.assertIn(expected, t.proc.stderr.read().decode())

    def test_existing_output_is_not_overwritten(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"keep-me"); f.flush()
            t = self.target(MS_OUTPUT=f.name)
            t.proc.stdin.write(b"quit\n")
            self.assertEqual(t.proc.wait(timeout=5), 0)
            self.assertEqual(Path(f.name).read_bytes(), b"keep-me")
            self.assertIn("cannot create", t.proc.stderr.read().decode())

    def test_output_write_failure_has_no_end_marker(self):
        t = self.target()
        t.command("malloc 0 99")
        t.command("outputfail")
        os.kill(t.proc.pid, signal.SIGUSR2)
        self.assertTrue(select.select([t.proc.stderr], [], [], 10)[0])
        error = os.read(t.proc.stderr.fileno(), 4096).decode()
        self.assertIn("snapshot output failed", error)
        self.assertNotIn("END", t.path.read_text())
        t.command("free 0")

    def test_non_pie_symbolization(self):
        t = self.target(binary="fixture-nopie")
        t.command("malloc 0 98")
        t.snap()
        source = subprocess.check_output(
            ["python3", str(ROOT / "tools/symbolize.py"), str(t.path)], text=True, timeout=30)
        self.assertRegex(source, r"SOURCE allocation_site at .*tests/fixture.c:\d+")
        t.command("free 0")

    def test_demo(self):
        with tempfile.TemporaryDirectory(prefix="mem-snapshot-demo-") as temp:
            path = Path(temp) / "demo.log"
            env = dict(os.environ, LD_PRELOAD=str(LIB), MS_OUTPUT=str(path))
            p = subprocess.Popen([str(ROOT / "build/demo")], env=env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                self.assertTrue(select.select([p.stdout], [], [], 10)[0])
                self.assertIn(b"Demo PID=", p.stdout.readline())
                os.kill(p.pid, signal.SIGUSR2)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if path.exists() and "END seq=1" in path.read_text():
                        break
                    time.sleep(0.02)
                self.assertIn("END seq=1", path.read_text())
                source = subprocess.check_output(
                    ["python3", str(ROOT / "tools/symbolize.py"), str(path)], text=True, timeout=30)
                self.assertIn("size=1234", source)
                self.assertRegex(source, r"SOURCE intentional_leak at .*examples/demo.c:\d+")
            finally:
                p.terminate()
                p.wait(timeout=5)
                p.stdout.close()
                p.stderr.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
