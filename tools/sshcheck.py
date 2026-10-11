#!/usr/bin/env python3
"""The Mac's side of tools/ssh-test.sh: once the guest's log says bin/sshd
listens, OpenSSH's client logs in to Jam OS through the network peer's
--tcp-forward port, as `ssh jam@<pc>` on the Mac would, and checks what it
gets.

    sshcheck.py --log SERIAL_LOG --port P (the guest's 22) --key KEY
                --wrong WRONG_KEY --known KNOWN_HOSTS --out DIR [--again FIRST_LOG]

The first boot: a command (its output and its exit status, `exit 7` from
the shell's own command too); an interactive session on a pty (the prompt,
TERM as the client sent it, the window's size and a window change as a
POSIX program on the terminal sees them, Ctrl+C on `sleep 100`, `exit 7`
ending it with 7); the host key the client saw against the fingerprint in
the guest's log; a key not in authorized_keys, a password attempt (never
asked: the askpass helper must never run) and another user name, each
refused; 8 sessions at once and a 9th connection refused; 4 sessions'
output at once, each whole; a session killed mid-output; then, in a
session of its own, the guest's processes: nothing left of the others
(one session's shell and console, the listener and one tinysshd). The
last login, as user sshcheck-done, is refused on purpose: the guest's
script waits for that line.

With --again (a second boot of the same stick): the host key must be the
one the first boot made (StrictHostKeyChecking=yes with the first boot's
known_hosts, and the same fingerprint in both logs, and no "made" line),
then the same refused last login.

One line per check, "sshcheck: PASS" or "sshcheck: FAIL" last."""
import argparse
import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import threading
import time

problems = []
ANSI = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)|"
                  rb"\x1b[()][0-9A-Za-z]|\x1b[=>78]")


def say(msg):
    print("sshcheck: " + msg, flush=True)


def check(cond, what):
    say(("ok: " if cond else "FAILED: ") + what)
    if not cond:
        problems.append(what)
    return cond


def log_text(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("latin-1")
    except OSError:
        return ""


def wait_log(path, text, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if text in log_text(path):
            return True
        time.sleep(0.2)
    check(False, "'%s' in the guest's log within %d s" % (text, timeout))
    return False


def plain(b):
    """Bytes the terminal showed, as text: escapes and CRs out."""
    return ANSI.sub(b"", b).replace(b"\r", b"").decode("utf-8", "replace")


class Ssh:
    """OpenSSH's client against the forward, with the test's options."""

    def __init__(self, a):
        self.a = a

    def argv(self, user="jam", key=None, strict="accept-new", extra=(), quiet=True):
        return ["ssh", "-F", "/dev/null", "-p", str(self.a.port),
                "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-o", "IdentityAgent=none",
                "-o", "IdentityFile=" + (key or self.a.key),
                "-o", "UserKnownHostsFile=" + self.a.known, "-o", "HostKeyAlias=jamos-test",
                "-o", "StrictHostKeyChecking=" + strict, "-o", "ConnectTimeout=60"] + \
            (["-o", "LogLevel=ERROR"] if quiet else []) + list(extra) + [user + "@127.0.0.1"]

    def run(self, command, timeout=120, **kw):
        """ssh ... 'command' without a pty: (exit code, output, errors)."""
        p = subprocess.run(self.argv(**kw) + [command], stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        return p.returncode, plain(p.stdout), p.stderr.decode("utf-8", "replace")


def commands(s):
    rc, out, err = s.run("echo hello-jamos")
    check(rc == 0 and "hello-jamos\n" in out, "a command: echo (exit %d, %r %r)" % (rc, out, err))
    rc, out, _ = s.run("pxio seq 3")
    check(rc == 0 and "line 1\nline 2\nline 3\n" in out,
          "a POSIX program's output (pxio seq 3: %r)" % out)
    rc, _, _ = s.run("pxio exit 7")
    check(rc == 7, "a command's exit status: pxio exit 7 gives %d" % rc)
    rc, _, _ = s.run("false")
    check(rc == 1, "a builtin's status: false gives %d" % rc)
    rc, _, _ = s.run("exit 5")
    check(rc == 5, "exit 5 gives %d" % rc)


def fingerprint(a):
    """The host key the client saw (its known_hosts) against the guest's log line."""
    p = subprocess.run(["ssh-keygen", "-l", "-f", a.known], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    seen = re.search(r"SHA256:[A-Za-z0-9+/]{43}", p.stdout.decode())
    said = re.search(r"init: SSH host key (SHA256:[A-Za-z0-9+/]{43})", log_text(a.log))
    check(seen and said and seen.group(0) == said.group(1),
          "the client's host key %s is the one the guest's log says (%s)" %
          (seen.group(0) if seen else None, said.group(1) if said else None))
    return said.group(1) if said else None


class Term:
    """An interactive ssh -tt on a pty of ours: what it shows, typed keys."""

    def __init__(self, s, cols, rows):
        self.master, slave = pty.openpty()
        self.size(cols, rows, slave)
        env = dict(os.environ, TERM="xterm-256color")
        self.p = subprocess.Popen(s.argv(extra=("-tt",)), stdin=slave, stdout=slave,
                                  stderr=slave, env=env, start_new_session=True)
        os.close(slave)
        self.buf = b""

    def size(self, cols, rows, fd=None):
        fcntl.ioctl(self.master if fd is None else fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))

    def resize(self, cols, rows):
        self.size(cols, rows)
        self.p.send_signal(signal.SIGWINCH)   # ssh sends window-change

    def send(self, text):
        os.write(self.master, text.encode())

    def expect(self, pattern, timeout):
        """Wait until pattern (a regex) is in what came since the last match."""
        end = time.monotonic() + timeout
        while True:
            m = re.search(pattern, plain(self.buf))
            if m:
                self.buf = self.buf[len(self.buf):]   # the next expect sees what comes after
                return m
            left = end - time.monotonic()
            if left <= 0:
                return None
            r, _, _ = select.select([self.master], [], [], min(left, 0.5))
            if r:
                try:
                    self.buf += os.read(self.master, 65536)
                except OSError:
                    return None

    def done(self, timeout):
        try:
            return self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            return None


PROMPT = r"jam:[^\n]*> ?$"


def interactive(s):
    t = Term(s, 80, 24)
    if not check(t.expect(PROMPT, 60), "an interactive session: the shell's prompt"):
        t.p.kill()
        return
    t.send("pxio env\r")
    check(t.expect(r"pxio: TERM=xterm-256color", 20), "TERM as the client sent it")
    t.expect(PROMPT, 10)
    t.send("pxio size\r")
    check(t.expect(r"pxio: size 80x24", 20), "the window's size at the start: 80x24")
    t.expect(PROMPT, 10)
    t.resize(100, 30)
    time.sleep(1)
    t.send("pxio size\r")
    check(t.expect(r"pxio: size 100x30", 20), "a window change: 100x30")
    t.expect(PROMPT, 10)
    t.send("sleep 100\r")
    time.sleep(1.5)
    t0 = time.monotonic()
    t.send("\x03")
    check(t.expect(PROMPT, 15) and time.monotonic() - t0 < 15, "Ctrl+C ends sleep 100")
    t.send("exit 7\r")
    rc = t.done(30)
    check(rc == 7, "exit 7 ends the session with 7 (got %s)" % rc)
    os.close(t.master)


def refusals(s, a):
    rc, _, err = s.run("echo no", key=a.wrong)
    check(rc == 255 and "Permission denied (publickey)" in err,
          "a key not in authorized_keys: refused (exit %d, %r)" % (rc, err.strip()))
    askpass = os.path.join(a.out, "askpass.sh")
    asked = os.path.join(a.out, "askpass.asked")
    with open(askpass, "w") as f:
        f.write("#!/bin/sh\ntouch '%s'\necho not-a-password\n" % asked)
    os.chmod(askpass, 0o755)
    if os.path.exists(asked):
        os.remove(asked)
    env = dict(os.environ, SSH_ASKPASS=askpass, SSH_ASKPASS_REQUIRE="force", DISPLAY=":0")
    argv = s.argv(extra=("-o", "BatchMode=no", "-o", "PubkeyAuthentication=no",
                         "-o", "PreferredAuthentications=password,keyboard-interactive",
                         "-o", "NumberOfPasswordPrompts=1", "-v"), quiet=False)
    p = subprocess.run(argv + ["echo no"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, env=env, timeout=120)
    err = p.stderr.decode("utf-8", "replace")
    check(p.returncode == 255 and "Permission denied (publickey)" in err and
          "Authentications that can continue: publickey\n" in err.replace("\r", ""),
          "a password attempt: refused, only publickey offered (exit %d)" % p.returncode)
    check(not os.path.exists(asked), "a password was never asked for")
    rc, _, err = s.run("echo no", user="other")
    check(rc == 255 and "Permission denied" in err,
          "another user name (other@): refused (exit %d)" % rc)


def at_once(s, a):
    """8 sessions at once, a 9th connection refused; then 4 with output."""
    results = [None] * 8

    def one(i):
        results[i] = s.run("sleep 8", timeout=180)

    ts = [threading.Thread(target=one, args=(i,)) for i in range(8)]
    for t in ts:
        t.start()
    full = wait_log(a.log, "(8 open)", 60)
    if full:
        t0 = time.monotonic()
        rc, _, err = s.run("echo ninth", timeout=60)
        check(rc == 255 and time.monotonic() - t0 < 30,
              "a 9th connection while 8 run: refused (exit %d, %r)" % (rc, err.strip()))
    for t in ts:
        t.join()
    check(all(r and r[0] == 0 for r in results),
          "8 sessions at once, each ended 0 (%s)" % [r[0] if r else None for r in results])
    outs = [None] * 4

    def two(i):
        outs[i] = s.run("pxio seq 3000", timeout=180)

    ts = [threading.Thread(target=two, args=(i,)) for i in range(4)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    want = ["line %d" % n for n in range(1, 3001)]

    def lines(o):   # pxio's lines (the shell's own `run:` lines and the log's left out)
        return [x for x in o[1].splitlines() if x.startswith("line ")] if o else []

    for i, o in enumerate(outs):
        others = [x for x in o[1].splitlines() if x and not x.startswith("line ")] if o else []
        if others:
            say("session %d also showed: %r" % (i, others[:6]))
    check(all(o and o[0] == 0 and lines(o) == want for o in outs),
          "4 sessions' output at once, each 3000 lines whole and in order (%s)" %
          [(o[0] if o else None, len(lines(o)), lines(o)[-1:]) for o in outs])


def killed_mid_output(s, a):
    p = subprocess.Popen(s.argv() + ["cat /data/big.txt"], stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    got = 0
    t0 = time.monotonic()
    while got < 256 * 1024 and time.monotonic() - t0 < 120:
        b = p.stdout.read1(65536)
        if not b:
            break
        got += len(b)
    p.kill()
    p.wait()
    check(got >= 256 * 1024, "a session killed mid-output: %d bytes came before the kill" % got)


def leftovers(s):
    """What runs now, from a session of its own: none of the earlier ones' left."""
    time.sleep(6)   # init kills an ended session's job 2 s after its shell ends
    rc, out, _ = s.run("ps")
    names = [line.split()[-1] for line in out.splitlines() if line[:5].strip().isdigit()]
    counts = {n: names.count(n) for n in ("session-shell", "session-console", "sshd")}
    check(rc == 0 and counts == {"session-shell": 1, "session-console": 1, "sshd": 2},
          "nothing left of the other sessions: %s (want this one's shell and console, "
          "the listener and this tinysshd)" % counts)


def last(s, a, log):
    rc, out, _ = s.run("echo still-here")
    check(rc == 0 and "still-here" in out, "the server still serves (exit %d)" % rc)
    rc, _, _ = s.run("true", user="sshcheck-done")
    check(rc == 255, "the last login, as sshcheck-done: refused (exit %d)" % rc)
    wait_log(log, "account sshcheck-done: not exist", 30)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    for name in ("--log", "--key", "--wrong", "--known", "--out"):
        ap.add_argument(name, required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--again", help="the first boot's serial log")
    a = ap.parse_args()
    s = Ssh(a)
    if not wait_log(a.log, "sshd: listening on port 22", 300):
        say("FAIL")
        return 1
    if a.again:
        rc, out, err = s.run("echo again", strict="yes")
        check(rc == 0 and "again" in out,
              "the second boot: the same host key (known_hosts, strict): exit %d %r" %
              (rc, err.strip()))
        first = re.search(r"init: SSH host key (SHA256:\S+)", log_text(a.again))
        now = re.search(r"init: SSH host key (SHA256:\S+)", log_text(a.log))
        check(first and now and first.group(1) == now.group(1),
              "the same fingerprint on both boots (%s)" % (now.group(1) if now else None))
        check("SSH host key made" not in log_text(a.log), "no key made on the second boot")
        last(s, a, a.log)
    else:
        commands(s)
        fingerprint(a)
        interactive(s)
        refusals(s, a)
        at_once(s, a)
        killed_mid_output(s, a)
        leftovers(s)
        last(s, a, a.log)
    say("PASS" if not problems else "FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
