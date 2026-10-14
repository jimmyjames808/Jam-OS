#!/usr/bin/env python3
"""Run a command once the machine has the CPUs and memory it needs
(tools/remote-test.sh runs this on the test PC, docs/TESTING.md, "Another
machine for the tests"). Every checkout's commands share one budget of
virtual CPUs (JAMOS_REMOTE_CPUS, default 26 of the PC's 28) and of memory
(JAMOS_REMOTE_MEM_GB, default 20), and at most JAMOS_REMOTE_SLOTS commands at
once (default 16): more load made timing tests fail, less left the PC idle
while a queue of small runs waited.

    remote-slots.py <cpus> <mem_mb> <command> [args...]
    remote-slots.py --detach <log> <cpus> <mem_mb> <command> [args...]
    remote-slots.py --status

The command's cost is <cpus> and <mem_mb>; a command that costs more than
the whole budget runs alone (when nothing else runs). Its exit status is the
command's. The state is a directory ($JAMOS_REMOTE_LOCKDIR, default
~/work, then ".cpu"):

    mutex        held for a moment at a time while the state is read and changed
    run/<id>     one file for each running command: its cost, and an flock held
                 for as long as the command (or any process it left behind)
                 lives: the kernel drops that lock when the holder dies, so a
                 crashed client's file is found unlocked and removed by the next
                 caller and never leaks its CPUs
    wait/<id>    one for each waiting command, locked the same way; <id> is the
                 time it started waiting in nanoseconds, so they sort by age

Fairness: a small command may start ahead of an older waiting one (the idle
CPUs are used), but once the older one has waited JAMOS_REMOTE_AGE (60) seconds
nothing starts ahead of it, so a big run is not starved by a stream of small
ones. The owner can change the budget while commands wait by writing
"<cpus> [<mem_gb> [<slots>]]" into <JAMOS_REMOTE_LOCKDIR>/.cpu-budget on the
machine (it wins over the variables; delete it to go back to them).

A long claim (a Chromium build, hours: docs/CR-PLAN.md section 15) is
--detach: it waits for its turn as any command does, then starts the
command in the background (its own session, stdin /dev/null, its output to
<log>), prints "started <pid>" and returns, so the ssh session that asked
can end while the claim and the command live on; the claim ends with the
command. While it holds its CPUs and memory the tests get what is left of
the budget, and they shrink rather than overcommit the machine.

Test: tools/remote-slots-test.sh (a fake lock directory, no machine)."""
import fcntl
import os
import signal
import subprocess
import sys
import time

POLL = float(os.environ.get("JAMOS_REMOTE_POLL", "3"))   # seconds between looks while waiting
SAY_EVERY = 120                                          # seconds between "waiting" lines


def limits(base):
    """(cpus, mem_mb, slots): the variables, or the budget file's numbers."""
    def num(name, default):
        try:
            return int(os.environ.get(name) or default)
        except ValueError:
            return default
    cpus, gb, slots = num("JAMOS_REMOTE_CPUS", 26), num("JAMOS_REMOTE_MEM_GB", 20), num("JAMOS_REMOTE_SLOTS", 16)
    try:
        with open(os.path.join(base, ".cpu-budget")) as f:
            words = f.read().split()
        got = [int(w) for w in words[:3]]
        if got:
            cpus = got[0]
        if len(got) > 1:
            gb = got[1]
        if len(got) > 2:
            slots = got[2]
    except (OSError, ValueError):
        pass
    return max(cpus, 1), max(gb, 1) * 1024, max(slots, 1)


def entries(path, clean):
    """[(name, "cost line")] of the holders in a directory. A file nobody
    holds a lock on belongs to a dead holder: removed (when `clean`)."""
    out = []
    try:
        names = sorted(os.listdir(path))
    except OSError:
        return out
    for name in names:
        p = os.path.join(path, name)
        try:
            fd = os.open(p, os.O_RDONLY)
        except OSError:
            continue
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                out.append((name, os.read(fd, 400).decode("utf-8", "replace").strip()))
                continue
            if clean:   # we got the lock: no one holds it
                try:
                    os.unlink(p)
                except OSError:
                    pass
        finally:
            os.close(fd)
    return out


def cost_of(line):
    """(cpus, mem_mb) from a holder's file ("cpus mem_mb pid command")."""
    w = line.split()
    try:
        return int(w[0]), int(w[1])
    except (IndexError, ValueError):
        return 1, 1024   # unreadable: counted as a small run, never as nothing


def lock_new(path, text):
    """Create path, lock it and write text; the fd (keeps the lock) or None."""
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o666)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        return None
    os.write(fd, (text + "\n").encode())
    return fd


def status(d):
    cpus, mem, slots = limits(os.path.dirname(d))
    runs = entries(os.path.join(d, "run"), False)
    used = sum(cost_of(t)[0] for _, t in runs)
    print("remote-slots: %d of %d CPUs in use by %d run(s) (at most %d); %d waiting"
          % (used, cpus, len(runs), slots, len(entries(os.path.join(d, "wait"), False))))
    for name, t in runs:
        print("  running %s" % t)
    for name, t in entries(os.path.join(d, "wait"), False):
        print("  waiting %s" % t)


def admit(d, ticket, mine, runs_dir, wait_dir, wfd):
    """Under the mutex: start now if the budget allows. Returns (fd of the
    run file or None, wait fd, a "waiting" description, used cpus)."""
    base = os.path.dirname(d)
    cpus_max, mem_max, slots = limits(base)
    cpus, mem, text = mine
    runs = entries(runs_dir, True)
    waits = entries(wait_dir, True)
    used = sum(cost_of(t)[0] for _, t in runs)
    used_mem = sum(cost_of(t)[1] for _, t in runs)
    if wfd is None:
        wfd = lock_new(os.path.join(wait_dir, ticket), text)
    aged = int(os.environ.get("JAMOS_REMOTE_AGE", "60")) * 10**9
    now = time.time_ns()
    ahead = [n for n, _ in waits if n < ticket and now - int(n.split(".")[0]) > aged]
    fits = not runs or (used + cpus <= cpus_max and used_mem + mem <= mem_max and len(runs) < slots)
    if fits and not ahead:
        rfd = lock_new(os.path.join(runs_dir, ticket), text)
        if rfd is not None:
            try:
                os.unlink(os.path.join(wait_dir, ticket))
            except OSError:
                pass
            if wfd is not None:
                os.close(wfd)
            return rfd, None, "", used + cpus, cpus_max
    why = ("%d of %d CPUs in use by %d run(s)" % (used, cpus_max, len(runs)))
    if ahead:
        why += ", an older run has waited long enough to go first"
    elif used_mem + mem > mem_max:
        why += ", %.1f of %d GB memory" % ((used_mem) / 1024, mem_max // 1024)
    elif len(runs) >= slots:
        why += ", all %d slots" % slots
    return None, wfd, why, used, cpus_max


def detach(cmd, log, rfd, run_file):
    """--detach: the command in the background, holding the claim (the run
    file's lock, rfd) for as long as it lives; this process returns at once."""
    try:
        out = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
    except OSError as e:
        print("remote-test: %s: %s" % (log, e), file=sys.stderr)
        os.unlink(run_file)
        return 2
    pid = os.fork()
    if pid:
        os.close(out)
        print("started %d" % pid, flush=True)
        return 0
    # The child: a session of its own (no hangup when the ssh session ends),
    # the claim's lock kept, then the command; the claim ends with it.
    os.setsid()
    null = os.open(os.devnull, os.O_RDONLY)
    os.dup2(null, 0)
    os.dup2(out, 1)
    os.dup2(out, 2)
    try:
        child = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, pass_fds=[rfd])
        rc = child.wait()
    except OSError as e:
        print("remote-test: %s: %s" % (cmd[0], e), file=sys.stderr, flush=True)
        rc = 127
    try:
        os.unlink(run_file)
    except OSError:
        pass
    os._exit(128 - rc if rc < 0 else rc)


def main():
    base = os.environ.get("JAMOS_REMOTE_LOCKDIR") or os.path.expanduser("~/work")
    d = os.path.join(base, ".cpu")
    runs_dir, wait_dir = os.path.join(d, "run"), os.path.join(d, "wait")
    os.makedirs(runs_dir, exist_ok=True)
    os.makedirs(wait_dir, exist_ok=True)
    if sys.argv[1:] == ["--status"]:
        status(d)
        return 0
    args, log = sys.argv[1:], None
    if args[:1] == ["--detach"] and len(args) > 1:
        log, args = args[1], args[2:]
    if len(args) < 3:
        print("usage: remote-slots.py [--detach <log>] <cpus> <mem_mb> <command> [args...]",
              file=sys.stderr)
        return 2
    cpus, mem, cmd = int(args[0]), int(args[1]), args[2:]
    ticket = "%019d.%d" % (time.time_ns(), os.getpid())
    text = "%d %d %d %s" % (cpus, mem, os.getpid(), " ".join(cmd)[:200])
    label = os.environ.get("JAMOS_REMOTE_LABEL") or "run"
    wfd, started, said, rfd = None, time.time(), 0.0, None
    while rfd is None:
        mfd = os.open(os.path.join(d, "mutex"), os.O_RDWR | os.O_CREAT, 0o666)
        fcntl.flock(mfd, fcntl.LOCK_EX)
        try:
            rfd, wfd, why, used, budget = admit(d, ticket, (cpus, mem, text), runs_dir, wait_dir, wfd)
        finally:
            os.close(mfd)
        if rfd is not None:
            break
        if time.time() - said >= SAY_EVERY:
            print("remote-test: waiting: %s (this %s needs %d CPUs, %.1f GB)"
                  % (why, label, cpus, mem / 1024), file=sys.stderr, flush=True)
            said = time.time()
        time.sleep(POLL)
    waited = time.time() - started
    if not os.environ.get("JAMOS_REMOTE_QUIET") or waited >= 1:
        print("remote-test: %s with %d of %d CPUs in use%s" % (
            "building" if label == "build" else "running", used, budget, " (after %d s)" % waited if waited >= 1 else ""), file=sys.stderr, flush=True)
    if log is not None:
        return detach(cmd, log, rfd, os.path.join(runs_dir, ticket))
    try:
        child = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, pass_fds=[rfd])
    except OSError as e:
        print("remote-test: %s: %s" % (cmd[0], e), file=sys.stderr)
        os.unlink(os.path.join(runs_dir, ticket))
        return 127
    for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda s, _f: child.send_signal(s))   # a cancelled run ends its command
    rc = child.wait()
    try:
        os.unlink(os.path.join(runs_dir, ticket))
    except OSError:
        pass
    return 128 - rc if rc < 0 else rc


if __name__ == "__main__":
    sys.exit(main())
