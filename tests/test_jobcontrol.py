#!/usr/bin/env python3
"""Drives mysh through a real pseudo-terminal to test Ctrl-C, Ctrl-Z, jobs, fg and bg."""
import os
import pty
import select
import sys
import time

SH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mysh")
PROMPT = b"mysh$ "


class Shell:
    def __init__(self):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(SH, [SH])
        self.buf = b""
        self.expect(PROMPT)

    def read_until(self, pattern, timeout=3.0):
        deadline = time.time() + timeout
        while pattern not in self.buf:
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError(f"timed out waiting for {pattern!r}; got {self.buf!r}")
            r, _, _ = select.select([self.fd], [], [], left)
            if r:
                try:
                    self.buf += os.read(self.fd, 4096)
                except OSError:
                    break
        idx = self.buf.index(pattern) + len(pattern)
        out, self.buf = self.buf[:idx], self.buf[idx:]
        return out.decode()

    expect = read_until

    def send(self, data):
        os.write(self.fd, data if isinstance(data, bytes) else data.encode())

    def run(self, line):
        self.send(line + "\n")
        return self.read_until(PROMPT)


failures = 0


def test(name, fn):
    global failures
    try:
        fn()
        print(f"ok    {name}")
    except AssertionError as e:
        failures += 1
        print(f"FAIL  {name}: {e}")


def ctrl_c_kills_foreground_only():
    sh = Shell()
    sh.send("sleep 10\n")
    time.sleep(0.3)
    sh.send(b"\x03")
    sh.read_until(PROMPT)
    out = sh.run("echo still-alive")
    assert "still-alive" in out, out


def ctrl_c_at_prompt():
    sh = Shell()
    sh.send(b"partial line\x03")
    sh.read_until(PROMPT)
    out = sh.run("echo ok")
    assert "ok" in out and "partial" not in out, out


def ctrl_z_then_fg():
    sh = Shell()
    sh.send("sleep 0.8\n")
    time.sleep(0.2)
    sh.send(b"\x1a")
    out = sh.read_until(PROMPT)
    assert "Stopped" in out and "sleep 0.8" in out, out
    out = sh.run("jobs")
    assert "[1]+  Stopped" in out, out
    start = time.time()
    out = sh.run("fg")
    assert "sleep 0.8" in out, out
    assert time.time() - start > 0.3, "fg returned before the job finished"
    out = sh.run("jobs")
    assert "sleep" not in out, out


def ctrl_z_then_bg():
    sh = Shell()
    sh.send("sleep 0.6\n")
    time.sleep(0.2)
    sh.send(b"\x1a")
    sh.read_until(PROMPT)
    out = sh.run("bg %1")
    assert "[1]+ sleep 0.6 &" in out, out
    out = sh.run("jobs")
    assert "Running" in out, out
    time.sleep(0.8)
    out = sh.run("echo x")
    assert "Done" in out, out


def ctrl_z_whole_pipeline():
    sh = Shell()
    sh.send("sleep 5 | cat\n")
    time.sleep(0.3)
    sh.send(b"\x1a")
    out = sh.read_until(PROMPT)
    assert "Stopped" in out and "sleep 5 | cat" in out, out
    sh.send("fg\n")
    time.sleep(0.3)
    sh.send(b"\x03")
    sh.read_until(PROMPT)
    out = sh.run("jobs")
    assert "sleep" not in out, out


def fg_then_eof():
    sh = Shell()
    sh.run("cat &")
    time.sleep(0.3)
    sh.send("fg\n")
    time.sleep(0.2)
    sh.send("hello\n")
    sh.read_until(b"hello\r\nhello")  # tty echo + cat output
    sh.send(b"\x04")
    sh.read_until(PROMPT)
    out = sh.run("jobs")
    assert "cat" not in out, out


def stopped_bg_job_reported():
    sh = Shell()
    sh.run("cat &")
    time.sleep(0.3)
    out = sh.run("echo tick")
    assert "Stopped" in out and "cat" in out, out


test("Ctrl-C kills foreground job, not the shell", ctrl_c_kills_foreground_only)
test("Ctrl-C at prompt discards the line", ctrl_c_at_prompt)
test("Ctrl-Z stops job, fg resumes it", ctrl_z_then_fg)
test("Ctrl-Z stops job, bg continues it", ctrl_z_then_bg)
test("Ctrl-Z stops a whole pipeline", ctrl_z_whole_pipeline)
test("background reader gets SIGTTIN", stopped_bg_job_reported)
test("fg hands the terminal to a job", fg_then_eof)

print("all passed" if failures == 0 else f"{failures} failed")
sys.exit(1 if failures else 0)
