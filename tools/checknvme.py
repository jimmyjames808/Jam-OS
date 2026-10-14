#!/usr/bin/env python3
"""make check: drv/nvme sends only its stage's commands, and writes only a
drive it has found to be Jam OS's (docs/M12.7-PLAN.md, "The read-only
guarantee", way 2, and "Stage 2: writes to a Jam OS disk only").
Grep-level rules over drivers/nvme/, with the C comments and strings
taken out first, so prose may name anything:

  1. The opcode constants (OPC_<NAME> = <value>) are exactly the stage's
     list, by name and value, and all in cmd.c. Admin: Delete I/O SQ 00h,
     Create I/O SQ 01h, Get Log Page 02h, Delete I/O CQ 04h, Create I/O CQ
     05h, Identify 06h, Abort 08h, Set Features 09h. I/O: Read 02h, and
     from stage 2 Write 01h and Flush 00h. No #define names an opcode.
  2. No identifier names a command off the list or a way round it: write
     zeroes, write uncorrectable, dataset management / TRIM / discard,
     format NVM, sanitize, security send, firmware download / commit,
     namespace management / attachment, directive send, copy, a subsystem
     reset (NSSR); and any name with "write" or "flush" in it is one of
     the stage's few (stage 1: none but the driver API's register and
     channel stores and the health log's counters), so a new way to write
     must be added here, where a review sees it.
  3. Submission entries are cmd.c's alone: `struct nvme_sq {` is defined
     there and nowhere else, and its memory (sq_mem) and the queues
     (sqs[...]) are named nowhere else, so no other file can reach an
     entry (the compiler sees to the rest: the struct is opaque).
  4. cmd.c stores an entry's dword 0 (the opcode) in one place, sqe_put,
     after its run-time check (`!allowed(`); allowed() lets the I/O queue
     have Read alone (stage 1), or Read, and Write and Flush only while
     the drive isn't MODE_OTHER (stage 2).
  5. (Stage 2) The write gate. A Write is built in exactly one function,
     sqe_write in cmd.c (OPC_WRITE named nowhere else but allowed(), and
     handed to sqe_put once), and only for a drive the driver has found to
     be Jam OS's: its body checks the drive's mode (exactly `bool mode_ok
     = part == PART_TABLE ? (gate.mode == MODE_BLANK || gate.mode ==
     MODE_ERASE) : gate.mode == MODE_JAMOS;`: a partition only on a Jam
     OS drive, the table's blocks only on a blank or an erased one) and
     refuses, BEFORE it checks the blocks (`in_gate(`) and refuses, before
     it builds the entry; every Write carries FUA. A Flush is built in
     sqe_flush alone, after it refuses a MODE_OTHER drive. The gate (the
     mode and the blocks) is defined in cmd.c alone and set only in
     cmd_arm, which starts from MODE_OTHER, honours `ro`, and takes the
     mode from the library's pt_kind (Jam OS's mark, or blank) and nothing
     else, but for MODE_ERASE, which needs both the caller's `erase` (the
     owner's confirmation, checked in table.c) and pt_kind's
     PT_KIND_OTHER (`erase && kind == PT_KIND_OTHER`). So a write to any
     other drive, Windows' among them, cannot even be built unless its
     owner erases it.

A later stage that adds a command must change this file's lists, which a
review sees. Then it checks itself (--selftest, also run by `make
check`): a clean driver of each stage passes and every rule catches each
of its offences in made-up files, a stage-2 write path with the range
checked before the mode, or none, or a second function building a Write,
or the gate set outside cmd_arm, among them. Exit 1 naming every
offence."""
import os
import re
import sys

ADMIN = {"DELETE_SQ": 0x00, "CREATE_SQ": 0x01, "GET_LOG_PAGE": 0x02, "DELETE_CQ": 0x04,
         "CREATE_CQ": 0x05, "IDENTIFY": 0x06, "ABORT": 0x08, "SET_FEATURES": 0x09}

ALWAYS_FORBIDDEN = [
    r"zeroes", r"uncor", r"dsm", r"dataset", r"trim", r"discard",
    r"format_?nvm", r"opc_format", r"sanitize", r"security", r"sec_?send", r"fw_",
    r"firmware_", r"ns_?mgmt", r"ns_?manage", r"ns_?attach", r"namespace_?(mgmt|manage|attach)",
    r"directive", r"copy", r"nssr", r"subsystem_reset",
]
# Names with "write" or "flush" in them that no stage forbids: the driver
# API's register stores (the doorbells; no command) and channel write (a
# reply on DR_SERVE), a VMO mapping's right, and the health log's counters
# of writes (reading them is how the drive is shown untouched).
API_IDS = {"drv_write8", "drv_write16", "drv_write32", "drv_write64", "drv_channel_write",
           "VMAR_WRITE", "host_writes", "units_written"}

STAGE1 = {
    "name": "stage 1",
    "admin": ADMIN,
    "io": {"READ": 0x02},
    "write_ids": set(),
    "io_allowed": "return opc == OPC_READ;",
    "gate": False,
}
STAGE2 = {
    "name": "stage 2",
    "admin": ADMIN,
    "io": {"FLUSH": 0x00, "WRITE": 0x01, "READ": 0x02},
    # The write path's names, every one (rule 2): the opcodes and the two
    # functions that build them (cmd.c), the request kinds (nvme.h), the
    # block protocol's write and its reply (block.c), a writable buffer's
    # right, the counters, a flush request's check and create_table's
    # flush step (serve.c).
    "write_ids": {"OPC_WRITE", "OPC_FLUSH", "sqe_write", "sqe_flush", "IO_WRITE", "IO_FLUSH",
                  "write", "b_write", "block_reply_write", "RIGHT_WRITE", "writes", "flushes",
                  "write_fail", "check_flush", "CT_FLUSH"},
    "io_allowed": "return opc == OPC_READ || (gate.mode != MODE_OTHER && "
                  "(opc == OPC_WRITE || opc == OPC_FLUSH));",
    "write_mode": "bool mode_ok = part == PART_TABLE ? (gate.mode == MODE_BLANK || "
                  "gate.mode == MODE_ERASE) : gate.mode == MODE_JAMOS;",
    "gate": True,
}
CURRENT = STAGE2


def strip(text):
    """C source with comments and string / char literals blanked (lines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] in "\"'":
            q, j = text[i], i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            out.append(q + " " * (min(j, n) - i - 1) + q)
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def functions(code):
    """{name: body} of the top-level functions (the house style: the
    closing brace at column 0), and [(name, start, end)] in order."""
    found, spans = {}, []
    for m in re.finditer(r"\n[A-Za-z_][\w \t\*]*?\b(\w+)\(([^;{}]*?)\)\s*\{(.*?)\n\}", code, re.S):
        found.setdefault(m.group(1), m.group(3))
        spans.append((m.group(1), m.start(3), m.end(3)))
    return found, spans


def check(files, stage=CURRENT):
    """files: {name: source}. Returns [(rule, "file:line: what")]."""
    found = []
    want = {}
    for kind in (stage["admin"], stage["io"]):
        for name, value in kind.items():
            want.setdefault(name, set()).add(value)
    forbidden = ALWAYS_FORBIDDEN + [r"write", r"flush"]
    seen = {}
    for name, src in sorted(files.items()):
        code = strip(src)
        for ln, line in enumerate(code.split("\n"), 1):
            where = "%s:%d" % (name, ln)
            for m in re.finditer(r"\bOPC_([A-Z0-9_]+)\s*=\s*(0[xX][0-9a-fA-F]+|\d+)", line):
                op, value = m.group(1), int(m.group(2), 0)
                seen.setdefault(op, set()).add(value)
                if name != "cmd.c":
                    found.append((1, "%s: opcode OPC_%s outside cmd.c" % (where, op)))
                elif op not in want or value not in want[op]:
                    found.append((1, "%s: opcode OPC_%s = %#x is not in the stage's list" %
                                  (where, op, value)))
            if re.search(r"#\s*define\s+\w*(OPC|OPCODE)\w*", line, re.I):
                found.append((1, "%s: an opcode #define" % where))
            for ident in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", line):
                if ident in API_IDS or ident in stage["write_ids"]:
                    continue
                for pat in forbidden:
                    if re.search(pat, ident, re.I):
                        found.append((2, "%s: %s names a command off the list" % (where, ident)))
                        break
            if name != "cmd.c":
                found += other_file_rules(where, line, stage)
        if name == "cmd.c":
            found += cmd_rules(code, stage)
    if "cmd.c" not in files:
        found.append((4, "cmd.c: missing"))
    for op in sorted(set(want) - set(seen)):
        found.append((1, "the stage's opcode OPC_%s is not defined in cmd.c" % op))
    return found


def other_file_rules(where, line, stage):
    """Rules 3, 4 and 5 for a line of a file that isn't cmd.c."""
    found = []
    if re.search(r"\bstruct\s+nvme_sq\s*\{", line):
        found.append((3, "%s: struct nvme_sq defined outside cmd.c" % where))
    if re.search(r"\bsq_mem\b|\bsqs\s*\[", line):
        found.append((3, "%s: a submission queue's memory outside cmd.c" % where))
    if re.search(r"\bdw\s*\[\s*0\s*\]\s*=[^=]", line):
        found.append((4, "%s: an entry's dword 0 stored outside cmd.c" % where))
    if stage["gate"]:
        if re.search(r"\bOPC_(WRITE|FLUSH)\b", line):
            found.append((5, "%s: a write opcode named outside cmd.c" % where))
        if re.search(r"\bgate\b|\bstruct\s+gate\b", line):
            found.append((5, "%s: the write gate named outside cmd.c" % where))
        if re.search(r"\bvoid\s+sqe_(write|flush)\s*\([^;]*$", line) and "{" in line:
            found.append((5, "%s: sqe_write or sqe_flush defined outside cmd.c" % where))
    return found


def cmd_rules(code, stage):
    """Rules 4 and 5 over cmd.c (comments and strings already blanked)."""
    found = []
    stores = [i for i, line in enumerate(code.split("\n"), 1)
              if re.search(r"\bdw\s*\[\s*0\s*\]\s*=[^=]", line)]
    if len(stores) != 1:
        found.append((4, "cmd.c: dword 0 stored %d times, not once" % len(stores)))
    fns, spans = functions(code)
    body = fns.get("sqe_put")
    if body is None:
        found.append((4, "cmd.c: no static void sqe_put(...)"))
    else:
        check_at, store_at = body.find("!allowed("), re.search(r"\bdw\s*\[\s*0\s*\]\s*=", body)
        if check_at < 0 or not store_at or check_at > store_at.start():
            found.append((4, "cmd.c: sqe_put doesn't check allowed() before it stores dword 0"))
    if " ".join(fns.get("allowed", "").split()).find(stage["io_allowed"]) < 0:
        found.append((4, "cmd.c: allowed() doesn't give the I/O queue exactly `%s`" %
                      stage["io_allowed"]))
    if stage["gate"]:
        found += gate_rules(code, fns, spans)
    return found


def in_which(spans, pos):
    for name, a, b in spans:
        if a <= pos < b:
            return name
    return None


def order(body, marks):
    """Where each of marks (regexes, in the order they must come) is first
    found in body, after the one before it; None for a mark not found
    there."""
    at, out = 0, []
    for mark in marks:
        m = re.compile(mark).search(body, at)
        out.append(m.start() if m else None)
        if m:
            at = m.end()
    return out


def gate_rules(code, fns, spans):
    """Rule 5 over cmd.c."""
    found = []
    # Only sqe_write and allowed() name OPC_WRITE; only sqe_flush and
    # allowed() name OPC_FLUSH; each built once.
    for op, fn in (("OPC_WRITE", "sqe_write"), ("OPC_FLUSH", "sqe_flush")):
        for m in re.finditer(r"\b%s\b" % op, code):
            where = in_which(spans, m.start())
            if where not in (fn, "allowed") and not re.match(r"\s*=", code[m.end():]):
                found.append((5, "cmd.c: %s named in %s, not only in %s" % (op, where, fn)))
        built = re.findall(r"\bsqe_put\s*\([^;]*\b%s\b" % op, code)
        if len(built) != 1:
            found.append((5, "cmd.c: %s handed to sqe_put %d times, not once" % (op, len(built))))
        if fn not in fns:
            found.append((5, "cmd.c: no %s" % fn))
    w = fns.get("sqe_write", "")
    # The mode, refused; then the blocks, refused; then the entry.
    marks = order(w, [r"\bgate\.mode\s*==\s*MODE_JAMOS\b", r"\brefuse\s*\(", r"\bin_gate\s*\(",
                      r"\brefuse\s*\(", r"\bsqe_put\s*\("])
    if None in marks:
        found.append((5, "cmd.c: sqe_write doesn't check the drive's mode (gate.mode == "
                         "MODE_JAMOS) and refuse, THEN check the blocks (in_gate) and refuse, "
                         "then build"))
    if " ".join(w.split()).find(STAGE2["write_mode"]) < 0:
        found.append((5, "cmd.c: sqe_write's mode isn't exactly `%s`" % STAGE2["write_mode"]))
    if not re.search(r"\bdw\s*\[\s*12\s*\]\s*=[^;]*\bRW_FUA\b", w):
        found.append((5, "cmd.c: sqe_write's Write doesn't carry FUA"))
    f = fns.get("sqe_flush", "")
    if None in order(f, [r"\bgate\.mode\s*==\s*MODE_OTHER\b", r"\brefuse\s*\(",
                         r"\bsqe_put\s*\("]):
        found.append((5, "cmd.c: sqe_flush doesn't refuse a MODE_OTHER drive before it builds"))
    # The gate: set in cmd_arm alone, from MODE_OTHER, by pt_kind.
    for m in re.finditer(r"\bgate\b(\s*\.\s*\w+(\s*\[[^]]*\])?)*\s*(=(?!=)|\+=|-=|\|=|&=|\+\+|--)",
                         code):
        if in_which(spans, m.start()) != "cmd_arm":
            ln = code.count("\n", 0, m.start()) + 1
            found.append((5, "cmd.c:%d: the gate set outside cmd_arm" % ln))
    a = fns.get("cmd_arm", "")
    if None in order(a, [r"\bgate\s*=\s*\(struct gate\)\s*\{\s*\.mode\s*=\s*MODE_OTHER\b",
                         r"\bc->ro\b", r"\breturn\b"]) or not re.search(r"\bpt_kind\s*\(", a):
        found.append((5, "cmd.c: cmd_arm doesn't start from MODE_OTHER, stop there on `ro`, and "
                         "take the kind from pt_kind"))
    for mode, kind in (("MODE_JAMOS", r"\bPT_KIND_JAMOS\b"), ("MODE_BLANK", r"\bPT_KIND_BLANK\b"),
                       ("MODE_ERASE", r"\berase\s*&&\s*kind\s*==\s*PT_KIND_OTHER\b")):
        if None in order(a, [kind, r"\bgate\.mode\s*=\s*%s\b" % mode]):
            found.append((5, "cmd.c: cmd_arm sets %s without `%s` first" % (mode, kind)))
    if len(re.findall(r"\bstruct\s+gate\s*\{", code)) != 1:
        found.append((5, "cmd.c: struct gate isn't defined here once"))
    return found


def tree(path):
    return {f: open(os.path.join(path, f)).read() for f in sorted(os.listdir(path))
            if f.endswith((".c", ".h"))}


# ---- the self-test's made-up drivers ------------------------------------------------------

CLEAN1 = """
/* Write, Flush and Format NVM have no code: prose may say so. */
enum { OPC_DELETE_SQ = 0x00, OPC_CREATE_SQ = 0x01, OPC_GET_LOG_PAGE = 0x02,
       OPC_DELETE_CQ = 0x04, OPC_CREATE_CQ = 0x05, OPC_IDENTIFY = 0x06, OPC_ABORT = 0x08,
       OPC_SET_FEATURES = 0x09 };
enum { OPC_READ = 0x02 };
struct nvme_sq { volatile uint32_t *sq_mem; };
static struct nvme_sq sqs[2];
static bool allowed(uint16_t qid, uint8_t opc)
{
    if (qid == 1)
        return opc == OPC_READ;
    return true;
}
static void sqe_put(struct nvme_queue *q, struct nvme_slot *s, uint8_t opc, uint32_t dw[16])
{
    if (!allowed(q->qid, opc))
        refuse("x", 0, 0);
    dw[0] = opc;
    drv_write32(db, 0, 1);
}
"""

CLEAN2 = """
/* Write zeroes and Format NVM have no code: prose may say so. */
enum { OPC_DELETE_SQ = 0x00, OPC_CREATE_SQ = 0x01, OPC_GET_LOG_PAGE = 0x02,
       OPC_DELETE_CQ = 0x04, OPC_CREATE_CQ = 0x05, OPC_IDENTIFY = 0x06, OPC_ABORT = 0x08,
       OPC_SET_FEATURES = 0x09 };
enum { OPC_FLUSH = 0x00, OPC_WRITE = 0x01, OPC_READ = 0x02 };
struct nvme_sq { volatile uint32_t *sq_mem; };
static struct nvme_sq sqs[2];
static struct gate { enum nvme_mode mode; uint64_t start, blocks; } gate;
void cmd_arm(struct nvme *c, const struct pt_table *t, bool erase)
{
    enum pt_kind kind = pt_kind(t);
    gate = (struct gate){ .mode = MODE_OTHER };
    if (c->ro)
        return;
    if (kind == PT_KIND_JAMOS)
        gate.mode = MODE_JAMOS;
    else if (kind == PT_KIND_BLANK)
        gate.mode = MODE_BLANK;
    else if (erase && kind == PT_KIND_OTHER)
        gate.mode = MODE_ERASE;
}
static bool allowed(uint16_t qid, uint8_t opc)
{
    if (qid == 1)
        return opc == OPC_READ || (gate.mode != MODE_OTHER && (opc == OPC_WRITE || opc == OPC_FLUSH));
    return true;
}
static void sqe_put(struct nvme_queue *q, struct nvme_slot *s, uint8_t opc, uint32_t dw[16])
{
    if (!allowed(q->qid, opc))
        refuse("x", 0, 0);
    dw[0] = opc;
    drv_write32(db, 0, 1);
}
void sqe_write(struct nvme *c, struct nvme_slot *s, uint8_t part, uint64_t lba, uint32_t n)
{
    bool mode_ok = part == PART_TABLE ? (gate.mode == MODE_BLANK || gate.mode == MODE_ERASE)
                                      : gate.mode == MODE_JAMOS;
    if (!mode_ok)
        refuse("a write", 0, 0);
    if (!in_gate(part, lba, n))
        refuse("a write outside", 0, 0);
    uint32_t dw[16] = { 0 };
    dw[12] = (n - 1) | RW_FUA;
    sqe_put(&c->io, s, OPC_WRITE, dw);
}
void sqe_flush(struct nvme *c, struct nvme_slot *s)
{
    if (gate.mode == MODE_OTHER)
        refuse("a flush", 0, 0);
    uint32_t dw[16] = { 0 };
    sqe_put(&c->io, s, OPC_FLUSH, dw);
}
"""

CLEAN2_QUEUE = """void f(struct nvme_io *io) {
    if (io->kind == IO_WRITE) sqe_write(c, s, io->part, 0, 1); else if (io->kind == IO_FLUSH) sqe_flush(c, s);
    drv_write32(db, 0, h); c->writes++; c->flushes++;
}
"""


def counts(got):
    out = {}
    for rule, _ in got:
        out[rule] = out.get(rule, 0) + 1
    return out


def expect(label, got, want):
    if os.environ.get("CHECKNVME_SHOW"):
        for rule, what in got:
            print("  %s: %d %s" % (label, rule, what))
    if counts(got) != want:
        print("checknvme: FAILED self-test (%s): offences per rule %s, want %s" %
              (label, counts(got), want))
        return False
    return True


def selftest1():
    clean = {"cmd.c": CLEAN1, "queue.c": "void f(void) { drv_write32(db, 0, h); }\n"}
    if not expect("stage 1 clean", check(clean, STAGE1), {}):
        return False
    bad = {
        # rule 1: an opcode off the list, one outside cmd.c, a wrong value, a #define
        "cmd.c": CLEAN1.replace("enum { OPC_READ = 0x02 };",
                                "enum { OPC_READ = 0x02, OPC_COMPARE = 0x05 };")
                       .replace("OPC_ABORT = 0x08", "OPC_ABORT = 0x0c"),
        "queue.c": "enum { OPC_IDENTIFY = 0x06 };\n#define NVME_OPC_X 0x81\n"
                   # rule 2: four names off the list
                   "static void sqe_write(void);\nint nvme_flush;\nint opc_format_nvm;\n"
                   "void fw_commit(void);\n"
                   # rule 3: the struct, its memory, the queues
                   "struct nvme_sq { int x; };\nvoid g(void) { q->sq->sq_mem[0] = 1; }\n"
                   "void k(void) { sqs[0].x = 1; }\n"
                   # rule 4: dword 0 outside cmd.c
                   "void h(uint32_t *dw) { dw[0] = 0x01; }\n",
    }
    if not expect("stage 1 bad", check(bad, STAGE1), {1: 4, 2: 4, 3: 3, 4: 1}):
        return False
    # rule 4 in cmd.c: the store before the check, twice, and Read not alone
    cmd = CLEAN1.replace("    if (!allowed(q->qid, opc))\n        refuse(\"x\", 0, 0);\n"
                         "    dw[0] = opc;\n",
                         "    dw[0] = opc;\n    if (!allowed(q->qid, opc))\n"
                         "        refuse(\"x\", 0, 0);\n    dw[0] = 1;\n")
    cmd = cmd.replace("return opc == OPC_READ;", "return opc <= OPC_READ;")
    if not expect("stage 1 cmd.c's rule 4", check({"cmd.c": cmd}, STAGE1), {4: 3}):
        return False
    # Stage 1 holds a stage-2 driver to stage 1: its write path is all offences.
    return expect("stage 2's driver under stage 1",
                  check({"cmd.c": CLEAN2, "queue.c": CLEAN2_QUEUE}, STAGE1),
                  {1: 2, 2: 14, 4: 1})


def selftest2():
    """Stage 2's write gate: a clean write path passes; each way of
    building a write for a drive that isn't Jam OS's is caught."""
    clean = {"cmd.c": CLEAN2, "queue.c": CLEAN2_QUEUE}
    if not expect("stage 2 clean", check(clean, STAGE2), {}):
        return False
    mode = "    bool mode_ok = part == PART_TABLE ? (gate.mode == MODE_BLANK || gate.mode == MODE_ERASE)\n" \
           "                                      : gate.mode == MODE_JAMOS;\n" \
           "    if (!mode_ok)\n        refuse(\"a write\", 0, 0);\n"
    blocks = "    if (!in_gate(part, lba, n))\n        refuse(\"a write outside\", 0, 0);\n"
    bads = [
        # the blocks checked before the mode
        ("range before mode", CLEAN2.replace(mode + blocks, blocks + mode), {5: 1}),
        # no mode check at all
        ("no mode check", CLEAN2.replace(mode, ""), {5: 2}),
        # the mode looked at but never refused
        ("mode not refused", CLEAN2.replace(mode, "    bool mode_ok = gate.mode == MODE_JAMOS;\n"),
         {5: 2}),
        # an erased drive's partitions writable (the table's blocks are all it may get)
        ("erase opens partitions", CLEAN2.replace(": gate.mode == MODE_JAMOS;",
                                                  ": (gate.mode == MODE_JAMOS || gate.mode == MODE_ERASE);"),
         {5: 1}),
        # MODE_ERASE without the owner's confirmation
        ("erase unconfirmed", CLEAN2.replace("erase && kind == PT_KIND_OTHER", "kind == PT_KIND_OTHER"),
         {5: 1}),
        # MODE_ERASE on a drive that isn't someone else's by pt_kind
        ("erase by hand", CLEAN2.replace("erase && kind == PT_KIND_OTHER", "erase"), {5: 1}),
        # a second function building a Write, past the gate
        ("a second writer", CLEAN2 + "static void sneak(struct nvme *c, struct nvme_slot *s)\n{\n"
         "    uint32_t dw[16] = { 0 };\n    sqe_put(&c->io, s, OPC_WRITE, dw);\n}\n", {5: 2}),
        # the gate set outside cmd_arm (twice: a mode, a field)
        ("gate set elsewhere", CLEAN2.replace("    uint32_t dw[16] = { 0 };\n    sqe_put(&c->io, s, "
                                              "OPC_FLUSH, dw);",
                                              "    gate.mode = MODE_JAMOS;\n    gate.blocks += 1;\n"
                                              "    uint32_t dw[16] = { 0 };\n    sqe_put(&c->io, s, "
                                              "OPC_FLUSH, dw);"), {5: 2}),
        # cmd_arm that ignores ro and doesn't start from nothing
        ("cmd_arm without ro", CLEAN2.replace("    if (c->ro)\n        return;\n", ""), {5: 1}),
        # Jam OS's mode without the library's word for it
        ("mode by hand", CLEAN2.replace("    if (kind == PT_KIND_JAMOS)\n", "    if (t->nparts == 2)\n"),
         {5: 1}),
        # allowed() giving the I/O queue Write whatever the drive
        ("allowed without mode", CLEAN2.replace("(gate.mode != MODE_OTHER && (opc == OPC_WRITE || "
                                                "opc == OPC_FLUSH))",
                                                "opc == OPC_WRITE || opc == OPC_FLUSH"), {4: 1}),
        # a Write without FUA
        ("no FUA", CLEAN2.replace("dw[12] = (n - 1) | RW_FUA;", "dw[12] = n - 1;"), {5: 1}),
        # a Flush for any drive
        ("flush ungated", CLEAN2.replace("    if (gate.mode == MODE_OTHER)\n        refuse(\"a flush\", 0, 0);\n",
                                         ""), {5: 1}),
        # Write's opcode not 01h
        ("write opcode", CLEAN2.replace("OPC_WRITE = 0x01", "OPC_WRITE = 0x11"), {1: 1}),
    ]
    for label, cmd, want in bads:
        if not expect(label, check({"cmd.c": cmd, "queue.c": CLEAN2_QUEUE}, STAGE2), want):
            return False
    # Other files: a raw write's name, the opcode, the gate, a second sqe_write.
    other = CLEAN2_QUEUE + "static void nvme_write_raw(void);\nint x = OPC_WRITE;\n" \
                           "void y(void) { gate.mode = MODE_JAMOS; }\n" \
                           "void sqe_flush(struct nvme *c, struct nvme_slot *s) {\n}\n"
    return expect("stage 2 other files", check({"cmd.c": CLEAN2, "queue.c": other}, STAGE2),
                  {2: 1, 5: 3})


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    found = check(tree(os.path.join(here, "..", "drivers", "nvme")))
    if found:
        print("checknvme: drivers/nvme breaks %s's rules (rule, where):" % CURRENT["name"])
        for rule, what in found:
            print("  %d %s" % (rule, what))
        return 1
    if not selftest1() or not selftest2():
        return 1
    print("checknvme: drivers/nvme sends only %s's commands, from cmd.c alone, and builds a "
          "Write only for a drive it found to be Jam OS's (or a table on a blank or erased one), "
          "the mode checked before the blocks (5 rules, each self-checked)" % CURRENT["name"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
