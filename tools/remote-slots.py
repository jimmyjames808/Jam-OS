#!/usr/bin/env python3
"""Run a command once the machine has the CPUs and memory it needs
(tools/remote-test.sh runs this on the test PC, docs/TESTING.md, "Another
machine for the tests"). Every checkout's commands share one budget of
virtual CPUs (JAMOS_REMOTE_CPUS, default 26 of the PC's 28) and of memory
(JAMOS_REMOTE_MEM_GB, default 20), and at most JAMOS_REMOTE_SLOTS commands at
once (default 16): more load made timing tests fail, less left the PC idle
while a queue of small runs waited.

    remote-slots.py [--shrink VAR:cpus:mem_mb:units] <cpus> <mem_mb> <command> [args...]
    remote-slots.py --detach <log> <cpus> <mem_mb> <command> [args...]
    remote-slots.py --status

The command's cost is <cpus> and <mem_mb>; a command that costs more than
the whole budget runs alone (when nothing else runs, and no long claim, below,
holds part of the budget). Its exit status is the command's. The state is a
directory ($JAMOS_REMOTE_LOCKDIR, default ~/work, then ".cpu"):

    mutex        held for a moment at a time while the state is read and changed
    run/<id>     one file for each running command: its cost, the pid of the
                 process that runs it (the "governor": this script), whether it
                 is a long claim, and the command; an flock held by the
                 governor, and by nothing else, for as long as it lives
    wait/<id>    one for each waiting command, locked the same way; <id> is the
                 time it started waiting in nanoseconds, so they sort by age

A claim lives and dies with its governor, not with the processes the command
left behind. The command runs in a session of its own with JAMOS_CLAIM=<id> in
its environment, which every process it starts inherits, also one that made its
own session (a QEMU, a serial feeder). When the command ends (or is cancelled:
SIGTERM, SIGINT or SIGHUP to the governor go to the command's whole group, and
after JAMOS_REMOTE_KILL_GRACE, 10 s, everything is killed) the governor kills
every process still carrying its id, and then lets go of the claim. A governor
that dies without a word (kill -9) leaves a run file nobody holds: the next
caller kills the processes carrying its id and removes the file. So a hung QEMU
left by a cancelled run cannot keep its CPUs from the others. (A process that
clears its environment escapes the sweep, and keeps only itself.)

Fairness: a small command may start ahead of an older waiting one (the idle
CPUs are used), but once the older one has waited JAMOS_REMOTE_AGE (60) seconds
nothing starts ahead of it, so a big run is not starved by a stream of small
ones. Except one that cannot fit however long the others take: a long claim
(below) holds part of the budget for hours, and a run whose cost plus the long
claims' is over the budget (or the slots) would block everything behind it for
as long, so it is told so at once ("needs 12 CPUs, but at most 10 can be free
while the long claim holds 16: ..."), does not count in the fairness rule, and
starts when the long claim ends. Runs that fit go on. The owner can change the
budget while commands wait by writing "<cpus> [<mem_gb> [<slots>]]" into
<JAMOS_REMOTE_LOCKDIR>/.cpu-budget on the machine (it wins over the variables;
delete it to go back to them).

--shrink VAR:cpus:mem_mb:units: the command is made of <units> equal parts that
may run one after the other (init-test.sh's boots, VAR=INIT_TEST_JOBS: how many
at once), each costing <cpus> and <mem_mb>. When the whole would never fit
beside the long claims, the governor claims as many parts as fit (at least one)
and starts the command with VAR set to that number, and says so. A run that
fits is not changed.

A long claim (a Chromium build, hours: docs/CR-PLAN.md section 15) is
--detach: it waits for its turn as any command does, then starts the
command in the background (its own session, stdin /dev/null, its output to
<log>) under a governor of its own, prints "started <pid>" and returns, so the
ssh session that asked can end while the claim and the command live on; the
claim ends with the command. While it holds its CPUs and memory the tests get
what is left of the budget.

--status lists the claims with their governor's pid, age and command; one whose
governor is gone says so (the next caller removes it).

Test: tools/remote-slots-test.sh (a fake lock directory, no machine)."""
import fcntl
import os
import signal
import subprocess
import sys
import time

POLL = float(os.environ.get("JAMOS_REMOTE_POLL", "3"))                  # seconds between looks while waiting
SAY_EVERY = float(os.environ.get("JAMOS_REMOTE_SAY_EVERY", "120"))      # seconds between "waiting" lines
KILL_GRACE = float(os.environ.get("JAMOS_REMOTE_KILL_GRACE", "10"))     # seconds a cancelled command has to end
TAG = "JAMOS_CLAIM"                                                     # the claim's id in its command's environment


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


class Claim:
    """What one command asks of the machine."""

    def __init__(self, cpus, mem, cmd, long=False, shrink=None):
        self.cpus, self.mem, self.cmd, self.long, self.shrink = cpus, mem, cmd, long, shrink
        self.env = {}   # added to the command's environment (--shrink's VAR)

    def text(self, pid=None):
        """The line in the run and wait files: "cpus mem_mb pid run|long command"."""
        cmd = " ".join(self.cmd).replace("\n", " ")[:200]
        return "%d %d %d %s %s" % (self.cpus, self.mem, pid or os.getpid(), "long" if self.long else "run", cmd)


def parse(line):
    """(cpus, mem_mb, pid, long, command) from a holder's file."""
    w = line.split()
    try:
        cpus, mem = int(w[0]), int(w[1])
    except (IndexError, ValueError):
        return 1, 1024, 0, False, line   # unreadable: counted as a small run, never as nothing
    try:
        pid = int(w[2])
    except (IndexError, ValueError):
        pid = 0
    rest = w[3:]
    long = rest[:1] == ["long"]
    if rest[:1] in (["run"], ["long"]):
        rest = rest[1:]
    return cpus, mem, pid, long, " ".join(rest)


def tagged(ticket):
    """The pids (not ours) whose environment carries this claim's id."""
    want = ("%s=%s" % (TAG, ticket)).encode()
    me = os.getpid()
    pids = []
    if os.path.isdir("/proc/self"):
        try:
            names = os.listdir("/proc")
        except OSError:
            return pids
        for n in names:
            if not n.isdigit() or int(n) == me:
                continue
            try:
                with open("/proc/%s/environ" % n, "rb") as f:
                    data = f.read()
            except OSError:
                continue
            if want in data.split(b"\0"):
                pids.append(int(n))
        return pids
    try:   # no /proc (the Mac, in the host test): ps -E puts the environment after the command
        out = subprocess.run(["ps", "-Eww", "-A", "-o", "pid=,command="], capture_output=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return pids
    for line in out.splitlines():
        pid, _, rest = line.strip().partition(b" ")
        if pid.isdigit() and int(pid) != me and b" " + want + b" " in b" " + rest + b" ":
            pids.append(int(pid))
    return pids


def kill_tagged(ticket):
    """SIGKILL every process carrying the claim's id; the pids killed."""
    pids = tagged(ticket)
    for p in pids:
        try:
            os.kill(p, signal.SIGKILL)
        except OSError:
            pass
    return pids


def say(text):
    """A line on stderr; an OSError (nobody reads it: the client is gone) comes through."""
    print(text, file=sys.stderr, flush=True)


def reap_dead(ticket, text):
    """The governor of this run is gone: kill what it left behind."""
    left = kill_tagged(ticket)
    if left:
        cpus, mem, pid, _long, cmd = parse(text)
        try:
            say("remote-test: the run of pid %d (%s) is gone: killed the %d process(es) it left, freeing %d CPUs, %.1f GB"
                % (pid, cmd[:50], len(left), cpus, mem / 1024))
        except OSError:
            pass


def entries(path, clean, reap=False):
    """[(name, "cost line", alive)] of the holders in a directory. A file
    nobody holds a lock on belongs to a dead holder: removed (when `clean`,
    after killing what a run left behind when `reap`), else listed as not
    alive."""
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
                out.append((name, os.read(fd, 400).decode("utf-8", "replace").strip(), True))
                continue
            # we got the lock: no one holds it
            text = os.read(fd, 400).decode("utf-8", "replace").strip()
            if not clean:
                out.append((name, text, False))
                continue
            if reap:
                reap_dead(name, text)
            try:
                os.unlink(p)
            except OSError:
                pass
        finally:
            os.close(fd)
    return out


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


def rewrite(fd, text):
    """Replace the text of a claim's file in one write, so a reader sees the
    old line or the new one, never an empty file."""
    size = os.fstat(fd).st_size
    data = (text + "\n").encode()
    if len(data) < size:
        data = text.encode().ljust(size - 1) + b"\n"
    os.pwrite(fd, data, 0)


def age(seconds):
    seconds = int(max(seconds, 0))
    if seconds >= 3600:
        return "%dh%02dm" % (seconds // 3600, seconds % 3600 // 60)
    if seconds >= 60:
        return "%dm%02ds" % (seconds // 60, seconds % 60)
    return "%ds" % seconds


def status(d):
    cpus, mem, slots = limits(os.path.dirname(d))
    now = time.time()
    runs = entries(os.path.join(d, "run"), False)
    live = [(n, t) for n, t, alive in runs if alive]
    waits = entries(os.path.join(d, "wait"), False)
    used = sum(parse(t)[0] for _, t in live)
    used_mem = sum(parse(t)[1] for _, t in live)
    print("remote-slots: %d of %d CPUs in use by %d run(s) (at most %d); %d waiting"
          % (used, cpus, len(live), slots, len([w for w in waits if w[2]])))
    print("  memory: %.1f of %d GB" % (used_mem / 1024, mem // 1024))
    for name, t, alive in runs:
        c, m, pid, long, cmd = parse(t)
        try:
            since = age(now - os.stat(os.path.join(d, "run", name)).st_mtime)
        except OSError:
            since = "?"
        print("  %s %2d CPUs %5.1f GB %7s  pid %d  %s: %s" % (
            "running" if alive else "GONE   ", c, m / 1024, since, pid, "long" if long else "run ", cmd[:90]))
        if not alive:
            print("    its governor is gone: the next caller kills what it left and removes the claim")
    for name, t, alive in waits:
        if not alive:
            continue
        c, m, pid, long, cmd = parse(t)
        print("  waiting %2d CPUs %5.1f GB %7s  pid %d  %s: %s" % (
            c, m / 1024, age(now - int(name.split(".")[0]) / 1e9), pid, "long" if long else "run ", cmd[:90]))


def admit(d, ticket, claim, runs_dir, wait_dir, wfd):
    """Under the mutex: start now if the budget allows. Returns (fd of the
    run file or None, wait fd, why it waits, used cpus, the budget's cpus,
    whether it can't fit beside the long claims however long the others take,
    lines to say)."""
    base = os.path.dirname(d)
    cpus_max, mem_max, slots = limits(base)
    runs = entries(runs_dir, True, True)
    waits = entries(wait_dir, True)
    held = [parse(t) for _, t, _ in runs]
    used = sum(h[0] for h in held)
    used_mem = sum(h[1] for h in held)
    longs = [h for h in held if h[3]]
    long_cpus, long_mem = sum(h[0] for h in longs), sum(h[1] for h in longs)
    notes = []

    def hopeless(cpus, mem):
        """Over the budget (or the slots) even with only the long claims running."""
        return bool(longs) and (long_cpus + cpus > cpus_max or long_mem + mem > mem_max or len(longs) >= slots)

    if wfd is None:
        wfd = lock_new(os.path.join(wait_dir, ticket), claim.text())
    if claim.shrink and wfd is not None and hopeless(claim.cpus, claim.mem):
        var, ucpus, umem, units = claim.shrink
        fit = min(units, (cpus_max - long_cpus) // ucpus, (mem_max - long_mem) // umem)
        if 1 <= fit < claim.cpus // ucpus:
            claim.cpus, claim.mem = fit * ucpus, fit * umem
            claim.env[var] = str(fit)
            rewrite(wfd, claim.text())
            notes.append("remote-test: %d of %d CPUs and %.1f GB are held by long claims: this run takes %s=%d part(s) at a time "
                         "(%d CPUs, %.1f GB) so that it fits"
                         % (long_cpus, cpus_max, long_mem / 1024, var, fit, claim.cpus, claim.mem / 1024))
    cant = hopeless(claim.cpus, claim.mem)
    aged = int(os.environ.get("JAMOS_REMOTE_AGE", "60")) * 10**9
    now = time.time_ns()
    # Older waiting runs that have waited long enough to go first, but not those
    # that cannot start until a long claim ends: they hold no one up.
    ahead = [n for n, t, _ in waits
             if n < ticket and now - int(n.split(".")[0]) > aged and not hopeless(parse(t)[0], parse(t)[1])]
    fits = not runs or (used + claim.cpus <= cpus_max and used_mem + claim.mem <= mem_max and len(runs) < slots)
    if fits and not ahead and not cant:
        rfd = lock_new(os.path.join(runs_dir, ticket), claim.text())
        if rfd is not None:
            try:
                os.unlink(os.path.join(wait_dir, ticket))
            except OSError:
                pass
            if wfd is not None:
                os.close(wfd)
            return rfd, None, "", used + claim.cpus, cpus_max, False, notes
    if cant:
        why = ("this run needs %d CPUs, %.1f GB, but at most %d CPUs, %.1f GB can be free while the long claim%s hold%s %d CPUs, "
               "%.1f GB (%s): it starts when that ends and the runs that fit go ahead of it; for a smaller one use fewer "
               "CPUs (QEMU_SMP, PORTS_TEST_JOBS, ...)"
               % (claim.cpus, claim.mem / 1024, max(cpus_max - long_cpus, 0), max(mem_max - long_mem, 0) / 1024,
                  "s" if len(longs) > 1 else "", "" if len(longs) > 1 else "s", long_cpus, long_mem / 1024,
                  "; ".join(h[4][:40] for h in longs)))
    else:
        why = "%d of %d CPUs in use by %d run(s)" % (used, cpus_max, len(runs))
        if ahead:
            why += ", an older run has waited long enough to go first"
        elif used_mem + claim.mem > mem_max:
            why += ", %.1f of %d GB memory" % (used_mem / 1024, mem_max // 1024)
        elif len(runs) >= slots:
            why += ", all %d slots" % slots
    return None, wfd, why, used, cpus_max, cant, notes


def stop_group(child, sig):
    """sig to the command's whole process group, while it runs."""
    if child is None or child.poll() is not None:
        return
    try:
        os.killpg(child.pid, sig)
    except OSError:
        pass


def hard_kill(child, ticket):
    stop_group(child, signal.SIGKILL)
    kill_tagged(ticket)


def run_command(cmd, extra_env, ticket):
    """Run the command in a session of its own, tagged with the claim's id;
    its exit status. A signal for this process goes to the command's group, and
    when the command is not gone JAMOS_REMOTE_KILL_GRACE seconds later (or a
    second signal comes) everything of the claim is killed. Afterwards what the
    command left behind is killed too."""
    env = dict(os.environ)
    env[TAG] = ticket
    env.update(extra_env)
    state = {"child": None, "sig": None, "at": 0.0}

    def forward(sig, _frame):
        if state["sig"] is not None:   # a second signal: no more patience
            hard_kill(state["child"], ticket)
            return
        state["sig"], state["at"] = sig, time.time()
        stop_group(state["child"], sig)
    for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, forward)
    try:
        child = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    except OSError as e:
        print("remote-test: %s: %s" % (cmd[0], e), file=sys.stderr, flush=True)
        return 127
    state["child"] = child
    if state["sig"] is not None:
        stop_group(child, state["sig"])
    rc = None
    while rc is None:
        try:
            rc = child.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            if state["sig"] is not None and time.time() - state["at"] > KILL_GRACE:
                hard_kill(child, ticket)
    left = kill_tagged(ticket)
    if left:
        try:
            say("remote-test: the command left %d process(es) running: killed them (pids %s)"
                % (len(left), " ".join(str(p) for p in left[:8])))
        except OSError:
            pass
    return 128 - rc if rc < 0 else rc


def detach(claim, cmd, log, rfd, ticket, run_file):
    """--detach: a governor of its own runs the command in the background,
    holding the claim (the run file's lock, rfd) for as long as the command
    lives; this process returns at once."""
    try:
        out = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
    except OSError as e:
        print("remote-test: %s: %s" % (log, e), file=sys.stderr)
        os.unlink(run_file)
        return 2
    # No claim id in the new governor's environment: if this call itself runs
    # inside another claim, that one's end must not kill this long one.
    env = {k: v for k, v in os.environ.items() if k != TAG}
    argv = [sys.executable, os.path.abspath(__file__), "--detached", str(rfd), ticket] + cmd
    try:
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=out, stderr=out, env=env,
                                 start_new_session=True, pass_fds=[rfd])
    except OSError as e:
        print("remote-test: %s: %s" % (cmd[0], e), file=sys.stderr)
        os.unlink(run_file)
        return 2
    os.close(out)
    rewrite(rfd, claim.text(child.pid))   # the file names the governor that holds the claim now
    print("started %d" % child.pid, flush=True)
    return 0


def detached(base, args):
    """The governor of a --detach claim: args = <fd of the held run file> <id> <command...>."""
    ticket, cmd = args[1], args[2:]
    rc = run_command(cmd, {}, ticket)
    try:
        os.unlink(os.path.join(base, ".cpu", "run", ticket))
    except OSError:
        pass
    return rc


def parse_shrink(spec):
    try:
        var, cpus, mem, units = spec.split(":")
        cpus, mem, units = int(cpus), int(mem), int(units)
    except ValueError:
        return None
    if not var.isidentifier() or min(cpus, mem, units) < 1:
        return None
    return var, cpus, mem, units


def main():
    base = os.environ.get("JAMOS_REMOTE_LOCKDIR") or os.path.expanduser("~/work")
    d = os.path.join(base, ".cpu")
    runs_dir, wait_dir = os.path.join(d, "run"), os.path.join(d, "wait")
    os.makedirs(runs_dir, exist_ok=True)
    os.makedirs(wait_dir, exist_ok=True)
    args, log, shrink = sys.argv[1:], None, None
    if args == ["--status"]:
        status(d)
        return 0
    if args[:1] == ["--detached"] and len(args) > 3:
        return detached(base, args[1:])
    while len(args) > 1 and args[0] in ("--detach", "--shrink"):
        if args[0] == "--detach":
            log = args[1]
        else:
            shrink = parse_shrink(args[1])
            if shrink is None:
                print("remote-test: --shrink wants VAR:cpus:mem_mb:units", file=sys.stderr)
                return 2
        args = args[2:]
    if len(args) < 3:
        print("usage: remote-slots.py [--detach <log>] [--shrink VAR:cpus:mem_mb:units] <cpus> <mem_mb> <command> [args...]",
              file=sys.stderr)
        return 2
    claim = Claim(int(args[0]), int(args[1]), args[2:], long=log is not None, shrink=None if log is not None else shrink)
    cmd = args[2:]
    ticket = "%019d.%d" % (time.time_ns(), os.getpid())
    label = os.environ.get("JAMOS_REMOTE_LABEL") or "run"
    wfd, started, said, rfd = None, time.time(), 0.0, None
    while rfd is None:
        mfd = os.open(os.path.join(d, "mutex"), os.O_RDWR | os.O_CREAT, 0o666)
        fcntl.flock(mfd, fcntl.LOCK_EX)
        try:
            rfd, wfd, why, used, budget, cant, notes = admit(d, ticket, claim, runs_dir, wait_dir, wfd)
        finally:
            os.close(mfd)
        try:
            for line in notes:
                say(line)
            if rfd is not None:
                break
            if time.time() - said >= SAY_EVERY:
                if cant:
                    say("remote-test: waiting: %s" % why)
                else:
                    say("remote-test: waiting: %s (this %s needs %d CPUs, %.1f GB)" % (why, label, claim.cpus, claim.mem / 1024))
                said = time.time()
        except OSError:   # nobody reads our stderr any more: the client is gone; give up the place
            try:
                os.unlink(os.path.join(wait_dir, ticket))
            except OSError:
                pass
            sys.stderr = open(os.devnull, "w")   # else Python's flush at exit fails and the status is 120
            return 141
        time.sleep(POLL)
    waited = time.time() - started
    try:
        if not os.environ.get("JAMOS_REMOTE_QUIET") or waited >= 1:
            say("remote-test: %s with %d of %d CPUs in use%s" % (
                "building" if label == "build" else "running", used, budget, " (after %d s)" % waited if waited >= 1 else ""))
    except OSError:   # the client is gone: the command would run for no one
        os.unlink(os.path.join(runs_dir, ticket))
        sys.stderr = open(os.devnull, "w")
        return 141
    run_file = os.path.join(runs_dir, ticket)
    if log is not None:
        return detach(claim, cmd, log, rfd, ticket, run_file)
    rc = run_command(cmd, claim.env, ticket)
    try:
        os.unlink(run_file)
    except OSError:
        pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
