#!/usr/bin/env python3
"""make check: drv/nvme can't write a block (docs/M12.7-PLAN.md, "The
read-only guarantee", way 2). Grep-level rules over drivers/nvme/, with
the C comments and strings taken out first, so prose may name anything:

  1. The opcode constants (OPC_<NAME> = <value>) are exactly the stage's
     list, by name and value, and all in cmd.c: stage 1 is Delete I/O SQ
     00h, Create I/O SQ 01h, Get Log Page 02h, Delete I/O CQ 04h, Create
     I/O CQ 05h, Identify 06h, Abort 08h, Set Features 09h (admin) and
     Read 02h (I/O). No #define names an opcode.
  2. No identifier names a command off the list or a way round it: write
     (OPC_WRITE, sqe_write, ...), write zeroes, write uncorrectable,
     flush, dataset management / TRIM / discard, format NVM, sanitize,
     security send, firmware download / commit, namespace management /
     attachment, directive send, copy, a subsystem reset (NSSR).
  3. Submission entries are cmd.c's alone: `struct nvme_sq {` is defined
     there and nowhere else, and its memory (sq_mem) and the queues
     (sqs[...]) are named nowhere else, so no other file can reach an
     entry (the compiler sees to the rest: the struct is opaque).
  4. cmd.c stores an entry's dword 0 (the opcode) in one place, sqe_put,
     after its run-time check (`!allowed(`); allowed() lets the I/O queue
     have Read alone (`return opc == OPC_READ;`).

A later stage that adds a command must change this file's list, which a
review sees. Then it checks itself (--selftest, also run by `make check`):
a clean driver passes and every rule catches each of its offences in
made-up files. Exit 1 naming every offence."""
import os
import re
import sys

STAGE1 = {
    "admin": {"DELETE_SQ": 0x00, "CREATE_SQ": 0x01, "GET_LOG_PAGE": 0x02, "DELETE_CQ": 0x04,
              "CREATE_CQ": 0x05, "IDENTIFY": 0x06, "ABORT": 0x08, "SET_FEATURES": 0x09},
    "io": {"READ": 0x02},
}

FORBIDDEN = [
    r"write", r"zeroes", r"uncor", r"flush", r"dsm", r"dataset", r"trim", r"discard",
    r"format_?nvm", r"opc_format", r"sanitize", r"security", r"sec_?send", r"fw_",
    r"firmware_", r"ns_?mgmt", r"ns_?manage", r"ns_?attach", r"namespace_?(mgmt|manage|attach)",
    r"directive", r"copy", r"nssr", r"subsystem_reset",
]
# Identifiers rule 2 lets through although a pattern matches: the driver
# API's register store (the doorbells; no command) and channel write (a
# reply on DR_SERVE), and the health log's counters of writes (reading
# them is how the drive is shown untouched).
ALLOWED_IDS = {"drv_write8", "drv_write16", "drv_write32", "drv_write64", "drv_channel_write",
               "VMAR_WRITE", "host_writes", "units_written"}


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


def check(files, stage=STAGE1):
    """files: {name: source}. Returns [(rule, "file:line: what")]."""
    found = []
    want = {}
    for kind in stage.values():
        for name, value in kind.items():
            want.setdefault(name, set()).add(value)
    seen = {}
    for name, src in sorted(files.items()):
        code = strip(src)
        lines = code.split("\n")
        for ln, line in enumerate(lines, 1):
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
                if ident in ALLOWED_IDS:
                    continue
                for pat in FORBIDDEN:
                    if re.search(pat, ident, re.I):
                        found.append((2, "%s: %s names a command off the list" % (where, ident)))
                        break
            if name != "cmd.c":
                if re.search(r"\bstruct\s+nvme_sq\s*\{", line):
                    found.append((3, "%s: struct nvme_sq defined outside cmd.c" % where))
                if re.search(r"\bsq_mem\b|\bsqs\s*\[", line):
                    found.append((3, "%s: a submission queue's memory outside cmd.c" % where))
                if re.search(r"\bdw\s*\[\s*0\s*\]\s*=[^=]", line):
                    found.append((4, "%s: an entry's dword 0 stored outside cmd.c" % where))
        if name == "cmd.c":
            found += cmd_rules(code)
    if "cmd.c" not in files:
        found.append((4, "cmd.c: missing"))
    for op in sorted(set(want) - set(seen)):
        found.append((1, "the stage's opcode OPC_%s is not defined in cmd.c" % op))
    return found


def cmd_rules(code):
    """Rule 4 over cmd.c (comments and strings already blanked)."""
    found = []
    stores = [i for i, line in enumerate(code.split("\n"), 1)
              if re.search(r"\bdw\s*\[\s*0\s*\]\s*=[^=]", line)]
    if len(stores) != 1:
        found.append((4, "cmd.c: dword 0 stored %d times, not once" % len(stores)))
    m = re.search(r"\nstatic void sqe_put\([^)]*\)\s*\{(.*?)\n\}", code, re.S)
    if not m:
        found.append((4, "cmd.c: no static void sqe_put(...)"))
    else:
        body = m.group(1)
        check_at, store_at = body.find("!allowed("), re.search(r"\bdw\s*\[\s*0\s*\]\s*=", body)
        if check_at < 0 or not store_at or check_at > store_at.start():
            found.append((4, "cmd.c: sqe_put doesn't check allowed() before it stores dword 0"))
    if not re.search(r"return opc == OPC_READ;", code):
        found.append((4, "cmd.c: allowed() doesn't give the I/O queue Read alone"))
    return found


def tree(path):
    return {f: open(os.path.join(path, f)).read() for f in sorted(os.listdir(path))
            if f.endswith((".c", ".h"))}


CLEAN_CMD = """
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


def selftest():
    clean = {"cmd.c": CLEAN_CMD, "queue.c": "void f(void) { drv_write32(db, 0, h); }\n"}
    got = check(clean)
    if got:
        print("checknvme: FAILED self-test: the clean driver was refused: %s" % got)
        return False
    bad = {
        # rule 1: an opcode off the list, one outside cmd.c, a wrong value, a #define
        "cmd.c": CLEAN_CMD.replace("enum { OPC_READ = 0x02 };",
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
    got = check(bad)
    counts = {}
    for rule, _ in got:
        counts[rule] = counts.get(rule, 0) + 1
    want = {1: 4, 2: 4, 3: 3, 4: 1}
    if os.environ.get("CHECKNVME_SHOW"):
        for rule, what in got:
            print("  %d %s" % (rule, what))
    if counts != want:
        print("checknvme: FAILED self-test: offences per rule %s, want %s" % (counts, want))
        return False
    # rule 4 in cmd.c: the store before the check, twice, and Read not alone
    cmd = CLEAN_CMD.replace("    if (!allowed(q->qid, opc))\n        refuse(\"x\", 0, 0);\n"
                            "    dw[0] = opc;\n",
                            "    dw[0] = opc;\n    if (!allowed(q->qid, opc))\n"
                            "        refuse(\"x\", 0, 0);\n    dw[0] = 1;\n")
    cmd = cmd.replace("return opc == OPC_READ;", "return opc <= OPC_READ;")
    got = [r for r, _ in check({"cmd.c": cmd})]
    if got != [4, 4, 4]:
        print("checknvme: FAILED self-test: cmd.c's rule 4 offences %s, want three" % got)
        return False
    return True


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    found = check(tree(os.path.join(here, "..", "drivers", "nvme")))
    if found:
        print("checknvme: drivers/nvme could send a command off the read-only list (rule, where):")
        for rule, what in found:
            print("  %d %s" % (rule, what))
        return 1
    if not selftest():
        return 1
    print("checknvme: drivers/nvme sends only the read-only list, from cmd.c alone "
          "(4 rules, each self-checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
