/* updtest: init's update check (user/services/init/update.c, <update.h>)
 * fed from files instead of the network, so it is tested before sockets
 * exist; tools/update-test.sh runs it in QEMU.
 *
 * The files: /data/update/manifest, jamos.elf and bootfs.img, put there
 * by the test script (the manifest made by tools/update-server.py and
 * signed with a throwaway test key whose public half is in the running
 * build's boot image; the boot image a copy of the build's with one more
 * file, update-marker.txt, so the boot after the update shows which build
 * runs); manifest-otherkey, the same manifest signed with a second key;
 * manifest-othernet, the same build's manifest saying the other network
 * default (untagged for a VLAN build, vlan21 for an untagged one), signed
 * with the first; nak.manifest, signed with the first, for two files that
 * are no kernel.
 * The shell gives it what its list asks for: /data (read) and init's
 * control channel, whose update_offer is the channel bin/update's offer
 * will travel on.
 *
 *   updtest good   the build as it is: accepted (init loads it as the
 *                  stored kernel; the script's `reboot` then runs it)
 *   updtest bad    every refusal, each on an offer channel of its own, and
 *                  each must be refused for its own reason: a byte of the
 *                  kernel or of the boot image changed (the SHA-256), the
 *                  kernel 4 KiB longer than the manifest says or cut to
 *                  half (the length), a VMO shorter than the length it
 *                  claims, a manifest of garbage, cut short, with a
 *                  signature too short to be one or of another format (the
 *                  format), unsigned, changed after it was signed (one
 *                  digit of a SHA-256), signed by another key, or carrying
 *                  another manifest's signature (the signature), an offer
 *                  with a bad magic, one handle or an unknown flag, the
 *                  build signed as saying the other network default (its
 *                  own refusal, unless forced), and two files that match
 *                  their signed manifest but are no kernel (the kernel's
 *                  own refusal); and the build as it is, and the
 *                  other-network one with UPDATE_OFFER_FORCE, each offered
 *                  UPDATE_OFFER_CHECK_ONLY: accepted, not loaded.
 *                  The script's `reboot` then shows the stored kernel
 *                  unchanged.
 *   updtest nokey  on a build without an update key: the build offered,
 *                  plain, check-only and to be written to the stick,
 *                  refused for that alone.
 *   updtest writefail
 *                  the build offered to be written to the stick
 *                  (UPDATE_OFFER_WRITE) with a test's failure
 *                  (UPDATE_OFFER_FAIL) at each step that has one: making
 *                  room, keeping the previous build, half way through the
 *                  new kernel, between the two renames; each answered
 *                  UPDATE_NOT_WRITTEN at that step with the stick booting
 *                  its old build (put back, after the renames); and a write
 *                  with check-only, a failure without a write and a
 *                  failure at no step refused. tools/update-write-test.sh
 *                  then boots the stick from cold.
 *   updtest writestop <n> b|c
 *                  the build (b: DIR, c: a second one in DIR "c/") offered
 *                  to be written with a test's stop (UPDATE_OFFER_STOP)
 *                  right after change n of the swap (1 to UPDATE_SWAP_OPS;
 *                  0: none), as a power cut there: answered "not written"
 *                  (n 0: accepted).
 *   updtest espcheck
 *                  which build /esp's default entry and its previous-build
 *                  entry each hold whole (A, B, C: their files in DIR
 *                  "a/", DIR and DIR "c/"); one of them must.
 *                  tools/update-write-test.sh runs a writestop and an
 *                  espcheck for each change of the swap, each its own
 *                  program, so each sees /esp as it came back.
 *   updtest menuwrite <dir> [n]
 *                  the build in DIR offered with the manifest in folder
 *                  <dir> (signed for that build and <dir>'s limine.conf,
 *                  its boot menu, which is offered too if the manifest
 *                  has a `menu` line), to be written to the stick, and
 *                  stopped after change n of the swaps (UPDATE_OFFER_STOP,
 *                  1 to UPDATE_STOP_MAX; 0 or none: no stop): its answer
 *                  in one line ("build ..., menu written" ...), which
 *                  tools/update-menu-test.sh reads.
 *   updtest menubad <dir>
 *                  the same build and menu offered as init must refuse
 *                  them: a byte of the menu changed (its SHA-256), the
 *                  menu 1 byte longer than its manifest says (its length),
 *                  the menu's VMO missing, a menu VMO with a manifest
 *                  that names none, a menu length with no menu; then the
 *                  offer check-only and plain (RAM only): accepted, the
 *                  stick's menu untouched (menucheck shows it).
 *   updtest menucheck <old> <new>
 *                  which boot menu Limine reads on /esp now
 *                  (boot/limine/limine.conf, or boot/limine.conf, the
 *                  spare, when that is missing), and that it is whole:
 *                  exactly the file <old> or <new> on /data.
 * Exit 0 when each case went as expected. */
#include <idl/initctl.h>
#include <os.h>
#include <sha256.h>
#include <update.h>
#include <wants.h>
#include "updtest.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc init\n"
          "mount /data r\n"
          "mount /esp r\n");

#define CHUNK       (64u << 10)       /* bytes copied at a time */

handle_t initctl;
unsigned failures;

/* A new VMO holding the first `keep` bytes of src (zeros after, up to
 * size bytes). */
static status_t clone(handle_t src, uint64_t keep, uint64_t size, handle_t *out)
{
    handle_t v = HANDLE_INVALID;
    status_t st = jam_vmo_create((size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), 0, HANDLE_INVALID, &v);
    uint8_t *buf = st == OK ? malloc(CHUNK) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    for (uint64_t off = 0; st == OK && off < keep; off += CHUNK) {
        uint64_t n = keep - off < CHUNK ? keep - off : CHUNK;
        st = jam_vmo_read(src, off, buf, n);
        if (st == OK)
            st = jam_vmo_write(v, off, buf, n);
    }
    free(buf);
    if (st != OK) {
        if (v)
            jam_handle_close(v);
        return st;
    }
    *out = v;
    return OK;
}

status_t load_dir(struct build *b, const char *dir)
{
    char p[80];
    handle_t m;
    uint64_t n = 0;
    snprintf(p, sizeof(p), "%smanifest", dir);
    status_t st = file_read_vmo(p, UPDATE_MANIFEST_MAX, &m, &n);
    if (st != OK)
        return st;
    st = jam_vmo_read(m, 0, b->manifest, n);
    jam_handle_close(m);
    b->manifest_len = (uint32_t)n;
    b->parts = UPDATE_FILES;
    static const char *const names[UPDATE_FILES] = { "jamos.elf", "bootfs.img" };
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        snprintf(p, sizeof(p), "%s%s", dir, names[f]);
        st = file_read_vmo(p, UPDATE_FILE_MAX, &b->vmo[f], &b->bytes[f]);
    }
    return st;
}

static status_t load(struct build *b)
{
    return load_dir(b, DIR);
}

status_t offer(const struct build *b, unsigned handles, uint32_t magic, struct update_answer *a)
{
    handle_t ch;
    status_t st = initctl_update_offer_until(initctl, now() + 5 * NS_PER_S, &ch);
    if (st != OK)
        return st;
    struct update_offer *o = calloc(1, sizeof(*o));
    handle_t hs[UPDATE_PARTS];
    unsigned nh = 0;
    st = o ? OK : ERR_NO_MEMORY;
    for (; st == OK && nh < handles; nh++)
        if ((st = jam_handle_duplicate(b->vmo[nh], RIGHT_SAME, &hs[nh])) != OK)
            break;
    if (st == OK) {
        *o = (struct update_offer){ .magic = magic, .manifest_len = b->manifest_len,
                                    .flags = b->flags };
        memcpy(o->bytes, b->bytes, sizeof(o->bytes));
        memcpy(o->manifest, b->manifest, b->manifest_len);
        st = jam_channel_write(ch, o, sizeof(*o), hs, handles);   /* moves the handles */
    }
    for (unsigned i = 0; st != OK && i < nh; i++)
        jam_handle_close(hs[i]);
    free(o);
    if (st == OK)
        st = jam_object_wait_one(ch, SIG_READABLE, now() + ANSWER_WAIT, NULL);
    uint32_t got = 0;
    struct channel_read_args r = {
        .h = ch, .bytes_cap = sizeof(*a), .bytes = (uint64_t)(uintptr_t)a,
        .actual_bytes = (uint64_t)(uintptr_t)&got,
    };
    if (st == OK)
        st = jam_channel_read(&r);
    if (st == OK && (got != sizeof(*a) || a->magic != UPDATE_ANSWER_MAGIC))
        st = ERR_INTERNAL;
    jam_handle_close(ch);
    return st;
}

void expect(const char *name, const struct build *b, unsigned handles, uint32_t magic, uint32_t why,
            uint32_t file)
{
    struct update_answer a;
    memset(&a, 0, sizeof(a));
    status_t st = offer(b, handles, magic, &a);
    bool ok = st == OK && a.why == why && (why == UPDATE_ACCEPTED) == (a.status == OK) &&
              (a.file == file || why == UPDATE_ACCEPTED);
    if (!ok)
        failures++;
    bool per_file = a.why == UPDATE_BAD_SIZE || a.why == UPDATE_SHORT_VMO ||
                    a.why == UPDATE_BAD_HASH;
    a.needs[sizeof(a.needs) - 1] = '\0';
    printf("updtest: %s: %s%s%s%s%s (%s, %s) %s\n", name,
           st == OK ? update_why_str(a.why) : "-", per_file ? ": " : "",
           per_file ? update_file_name(a.file) : "", a.needs[0] ? ": " : "", a.needs,
           status_str(st == OK ? a.status : st), a.version[0] ? a.version : "no version",
           ok ? "as expected" : "FAILED");
}

/* How a case changes file f: its VMO a clone of the first `keep` bytes,
 * `size` long, `bytes` claimed, byte `flip` (if < keep) xor-ed. */
struct change {
    unsigned f;
    uint64_t keep, size, bytes, flip;
};

/* b changed as c says, offered: the answer must be `why` about file c->f. */
static void changed(const char *name, const struct build *b, const struct change *c,
                    uint32_t why)
{
    struct build v = *b;
    status_t st = clone(b->vmo[c->f], c->keep, c->size, &v.vmo[c->f]);
    uint8_t ch = 0;
    if (st == OK && c->flip < c->keep)
        st = jam_vmo_read(v.vmo[c->f], c->flip, &ch, 1);
    ch ^= 0x20;
    if (st == OK && c->flip < c->keep)
        st = jam_vmo_write(v.vmo[c->f], c->flip, &ch, 1);
    v.bytes[c->f] = c->bytes;
    if (st == OK) {
        expect(name, &v, 2, UPDATE_OFFER_MAGIC, why, c->f);
    } else {
        failures++;
        printf("updtest: %s: can't make its file (%s): FAILED\n", name, status_str(st));
    }
    if (v.vmo[c->f] != b->vmo[c->f])
        jam_handle_close(v.vmo[c->f]);
}

/* The cases where a file's bytes differ from the manifest's. */
static void bad_files(const struct build *b)
{
    uint64_t k = b->bytes[UPDATE_KERNEL], s = b->bytes[UPDATE_BOOTFS], none = UINT64_MAX;
    const struct change kernel_byte = { UPDATE_KERNEL, k, k, k, k / 2 };
    /* the image's last byte, padding: still a boot image the kernel would take */
    const struct change bootfs_byte = { UPDATE_BOOTFS, s, s, s, s - 1 };
    const struct change longer = { UPDATE_KERNEL, k, k + 4096, k + 4096, none };
    const struct change half = { UPDATE_KERNEL, k / 2, k / 2, k / 2, none };
    const struct change short_vmo = { UPDATE_BOOTFS, s / 2, s / 2, s, none };
    changed("kernel byte changed", b, &kernel_byte, UPDATE_BAD_HASH);
    changed("boot image byte changed", b, &bootfs_byte, UPDATE_BAD_HASH);
    changed("kernel longer", b, &longer, UPDATE_BAD_SIZE);
    changed("kernel cut to half", b, &half, UPDATE_BAD_SIZE);
    changed("VMO shorter than claimed", b, &short_vmo, UPDATE_SHORT_VMO);
}

/* b with another manifest text (len bytes): refused for `why`. */
static void bad_manifest(const struct build *b, const char *name, const void *text, size_t len,
                         uint32_t why)
{
    struct build v = *b;
    memcpy(v.manifest, text, len);
    v.manifest_len = (uint32_t)len;
    expect(name, &v, 2, UPDATE_OFFER_MAGIC, why, 0);
}

/* Where the manifest's signature line starts (after the last but one '\n'). */
static size_t sig_line(const struct build *b)
{
    size_t at = b->manifest_len ? b->manifest_len - 1 : 0;
    while (at > 0 && b->manifest[at - 1] != '\n')
        at--;
    return at;
}

static void bad_manifests(const struct build *b)
{
    char text[UPDATE_MANIFEST_MAX];
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof(text); i++) {
        x = x * 1103515245u + 12345u;
        text[i] = (char)(x >> 16);
    }
    bad_manifest(b, "garbage manifest", text, sizeof(text), UPDATE_BAD_MANIFEST);
    bad_manifest(b, "manifest cut short", b->manifest, b->manifest_len / 2, UPDATE_BAD_MANIFEST);
    size_t n = b->manifest_len, at = sig_line(b);
    memcpy(text, b->manifest, n);
    memcpy(text + at, "signature 00ff\n", 15);   /* a signature too short to be one */
    bad_manifest(b, "short signature", text, at + 15, UPDATE_BAD_MANIFEST);
    memcpy(text, b->manifest, n);
    text[13] = '3';   /* "jamos-update 3" */
    bad_manifest(b, "another format", text, n, UPDATE_NEEDS_NEWER);
    memcpy(text, b->manifest, n);
    memcpy(text + at, "signature\n", 10);
    bad_manifest(b, "unsigned manifest", text, at + 10, UPDATE_UNSIGNED);
    memcpy(text, b->manifest, n);
    char *digit = &text[at - 2];   /* the boot image's SHA-256's last digit: signed */
    *digit = *digit == '0' ? '1' : '0';
    bad_manifest(b, "manifest changed after signing", text, n, UPDATE_BAD_SIGNATURE);
}

/* Signatures that are good ones, but not of this manifest by this build's
 * key: another key's (DIR "manifest-otherkey", the same build signed with
 * a second throwaway key), and another manifest's (nak's) on this one. */
static void wrong_signatures(const struct build *b, const struct build *nak)
{
    struct build other = *b;
    handle_t m;
    uint64_t n = 0;
    status_t st = file_read_vmo(DIR "manifest-otherkey", UPDATE_MANIFEST_MAX, &m, &n);
    if (st == OK) {
        st = jam_vmo_read(m, 0, other.manifest, n);
        jam_handle_close(m);
        other.manifest_len = (uint32_t)n;
    }
    if (st == OK) {
        expect("another key's signature", &other, 2, UPDATE_OFFER_MAGIC, UPDATE_BAD_SIGNATURE, 0);
    } else {
        failures++;
        printf("updtest: another key's signature: no " DIR "manifest-otherkey (%s): FAILED\n",
               status_str(st));
    }
    char text[UPDATE_MANIFEST_MAX];
    size_t at = sig_line(b), nat = sig_line(nak);
    memcpy(text, b->manifest, at);
    memcpy(text + at, nak->manifest + nat, nak->manifest_len - nat);
    bad_manifest(b, "another manifest's signature", text, at + nak->manifest_len - nat,
                 UPDATE_BAD_SIGNATURE);
}

/* The network default guard: the build, signed as saying the other kind
 * of network default (DIR "manifest-othernet"), refused; forced, taken
 * (check only). */
static void other_net(const struct build *b)
{
    struct build v = *b;
    handle_t m;
    uint64_t n = 0;
    status_t st = file_read_vmo(DIR "manifest-othernet", UPDATE_MANIFEST_MAX, &m, &n);
    if (st == OK) {
        st = jam_vmo_read(m, 0, v.manifest, n);
        jam_handle_close(m);
        v.manifest_len = (uint32_t)n;
    }
    if (st != OK) {
        failures++;
        printf("updtest: another network default: no " DIR "manifest-othernet (%s): FAILED\n",
               status_str(st));
        return;
    }
    v.flags = 0;
    expect("another network default", &v, 2, UPDATE_OFFER_MAGIC, UPDATE_NET_CHANGE, 0);
    v.flags = UPDATE_OFFER_FORCE | UPDATE_OFFER_CHECK_ONLY;
    expect("another network default, forced (check only)", &v, 2, UPDATE_OFFER_MAGIC,
           UPDATE_ACCEPTED, 0);
}

/* b with the signed manifest in DIR file instead, into *v. */
static status_t other_manifest(const struct build *b, const char *file, struct build *v)
{
    *v = *b;
    handle_t m;
    uint64_t n = 0;
    status_t st = file_read_vmo(file, UPDATE_MANIFEST_MAX, &m, &n);
    if (st == OK) {
        st = jam_vmo_read(m, 0, v->manifest, n);
        jam_handle_close(m);
        v->manifest_len = (uint32_t)n;
    }
    if (st != OK) {
        failures++;
        printf("updtest: no %s (%s): FAILED\n", file, status_str(st));
    }
    return st;
}

/* Extension lines (<update.h>): the build's manifest signed with one no
 * build knows (DIR "manifest-ext"): taken (check only); with a
 * must-understand one (DIR "manifest-must"): refused, needing a newer
 * build, and the same changed after signing: refused for the signature. */
static void extension_lines(const struct build *b)
{
    struct build v;
    if (other_manifest(b, DIR "manifest-ext", &v) == OK) {
        v.flags = UPDATE_OFFER_CHECK_ONLY;
        expect("an extension line (check only)", &v, 2, UPDATE_OFFER_MAGIC, UPDATE_ACCEPTED, 0);
    }
    if (other_manifest(b, DIR "manifest-must", &v) != OK)
        return;
    v.flags = UPDATE_OFFER_CHECK_ONLY;
    expect("a must-understand line", &v, 2, UPDATE_OFFER_MAGIC, UPDATE_NEEDS_NEWER, 0);
    char *line = strstr((char *)v.manifest, "\n!");
    if (line)
        line[2] = line[2] == 'x' ? 'y' : 'x';   /* the line's first letter: signed */
    expect("a must-understand line, changed after signing", &v, 2, UPDATE_OFFER_MAGIC,
           line ? UPDATE_BAD_SIGNATURE : UPDATE_ACCEPTED, 0);
}

/* Two files that match their signed manifest exactly but are no kernel
 * (8192 bytes of 0x55, 4096 of 0xaa; the test script signed DIR
 * "nak.manifest" for them): only kexec_load can refuse them. Into *b. */
static status_t make_nak(struct build *b)
{
    static uint8_t k[8192], s[4096];
    memset(k, 0x55, sizeof(k));
    memset(s, 0xaa, sizeof(s));
    *b = (struct build){ .bytes = { sizeof(k), sizeof(s) } };
    handle_t m;
    uint64_t n = 0;
    status_t st = file_read_vmo(DIR "nak.manifest", UPDATE_MANIFEST_MAX, &m, &n);
    if (st != OK)
        return st;
    st = jam_vmo_read(m, 0, b->manifest, n);
    jam_handle_close(m);
    b->manifest_len = (uint32_t)n;
    if (st == OK)
        st = jam_vmo_create(sizeof(k), 0, HANDLE_INVALID, &b->vmo[0]);
    if (st == OK)
        st = jam_vmo_create(sizeof(s), 0, HANDLE_INVALID, &b->vmo[1]);
    if (st == OK)
        st = jam_vmo_write(b->vmo[0], 0, k, sizeof(k));
    if (st == OK)
        st = jam_vmo_write(b->vmo[1], 0, s, sizeof(s));
    return st;
}

/* b offered with flags, as a stick write: the answer must be `why`, the
 * write must have got to `step`, and the stick must boot `stick`. */
static void expect_write(const char *name, struct build *b, uint32_t flags, uint32_t why,
                         uint32_t step, uint32_t stick)
{
    struct update_answer a;
    memset(&a, 0, sizeof(a));
    b->flags = flags;
    status_t st = offer(b, 2, UPDATE_OFFER_MAGIC, &a);
    b->flags = 0;
    bool ok = st == OK && a.why == why && (why == UPDATE_ACCEPTED) == (a.status == OK) &&
              a.write_step == step && a.stick == stick;
    failures += !ok;
    printf("updtest: %s: %s (%s), %s, %s, in %u ms: %s\n", name,
           st == OK ? update_why_str(a.why) : "-", status_str(st == OK ? a.status : st),
           update_write_step_str(a.write_step), update_stick_str(a.stick), a.write_ms,
           ok ? "as expected" : "FAILED");
}

/* Stick writes that fail (a test's failure, UPDATE_OFFER_FAIL, at each
 * step that has one: as if the ESP's service died there): the build is
 * loaded, and the stick still boots its old build; offers that can't be
 * (a write with check-only, a failure without a write) are refused. */
static void write_failures(struct build *b)
{
    static const struct { const char *name; uint32_t step; } at[] = {
        { "write fails making room", UPDATE_WRITE_ROOM },
        { "write fails keeping the previous build", UPDATE_WRITE_PREV },
        { "write fails half way through the new kernel", UPDATE_WRITE_NEW },
        { "write fails between the two renames", UPDATE_WRITE_SWITCH },
    };
    for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++)
        expect_write(at[i].name, b, UPDATE_OFFER_WRITE | UPDATE_OFFER_FAIL(at[i].step),
                     UPDATE_NOT_WRITTEN, at[i].step, UPDATE_STICK_OLD);
    b->flags = UPDATE_OFFER_WRITE | UPDATE_OFFER_CHECK_ONLY;
    expect("write and check only", b, 2, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER, 0);
    b->flags = UPDATE_OFFER_FAIL(UPDATE_WRITE_NEW);
    expect("a failure without a write", b, 2, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER, 0);
    b->flags = UPDATE_OFFER_WRITE | UPDATE_OFFER_FAIL(UPDATE_WRITE_DONE);
    expect("a failure at no step", b, 2, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER, 0);
    b->flags = 0;
}

/* ---- writestop: a stop at each change of the swap --------------------------------- */

/* A build's two files, as their SHA-256s. */
struct sums {
    uint8_t sha[UPDATE_FILES][SHA256_BYTES];
};

static status_t sum_vmo(handle_t v, uint64_t n, uint8_t out[SHA256_BYTES])
{
    static uint8_t buf[CHUNK];
    struct sha256 h;
    sha256_init(&h);
    for (uint64_t off = 0; off < n; off += CHUNK) {
        uint64_t k = n - off < CHUNK ? n - off : CHUNK;
        status_t st = jam_vmo_read(v, off, buf, k);
        if (st != OK)
            return st;
        sha256_add(&h, buf, k);
    }
    sha256_done(&h, out);
    return OK;
}

status_t esp_file(const char *path, handle_t *v, uint64_t *n)
{
    status_t st = ERR_NOT_FOUND;
    for (uint64_t end = now() + 10 * NS_PER_S; now() < end;) {   /* bounded: /esp's return */
        st = file_read_vmo(path, UPDATE_FILE_MAX, v, n);
        if (st == OK)
            return OK;
        handle_t c;
        uint64_t cn;
        if (file_read_vmo("/esp/boot/limine/limine.conf", 1u << 20, &c, &cn) == OK) {
            jam_handle_close(c);   /* /esp is there: the file isn't */
            return st;
        }
        jam_nanosleep(now() + 100 * NS_PER_MS);
    }
    return st;
}

/* The pair of /esp files kernel, bootfs as sums (false: one is missing or
 * unreadable). */
static bool esp_pair(const char *kernel, const char *bootfs, struct sums *out)
{
    const char *paths[UPDATE_FILES] = { kernel, bootfs };
    for (unsigned f = 0; f < UPDATE_FILES; f++) {
        handle_t v;
        uint64_t n = 0;
        if (esp_file(paths[f], &v, &n) != OK)
            return false;
        status_t st = sum_vmo(v, n, out->sha[f]);
        jam_handle_close(v);
        if (st != OK)
            return false;
    }
    return true;
}

/* Which of the n builds the pair is (-1: none, or no whole pair). */
static int which(bool whole, const struct sums *pair, const struct sums *builds, unsigned n)
{
    for (unsigned i = 0; whole && i < n; i++)
        if (!memcmp(pair, &builds[i], sizeof(*pair)))
            return (int)i;
    return -1;
}

/* /esp now: which build each entry boots (-1: none). */
static void entries(const struct sums *builds, unsigned n, int *def, int *prev)
{
    struct sums p;
    bool whole = esp_pair("/esp/boot/jamos.elf", "/esp/boot/bootfs.img", &p);
    *def = which(whole, &p, builds, n);
    whole = esp_pair("/esp/boot/prev-jamos.elf", "/esp/boot/prev-bootfs.img", &p);
    *prev = which(whole, &p, builds, n);
}

/* A build's sums from its files on /data (dir: DIR "a/", DIR, DIR "c/"). */
static bool sums_of(const char *dir, struct sums *out)
{
    struct build x;
    memset(&x, 0, sizeof(x));
    bool ok = load_dir(&x, dir) == OK;
    for (unsigned f = 0; ok && f < UPDATE_FILES; f++)
        ok = sum_vmo(x.vmo[f], x.bytes[f], out->sha[f]) == OK;
    for (unsigned f = 0; f < UPDATE_FILES; f++)
        if (x.vmo[f])
            jam_handle_close(x.vmo[f]);
    return ok;
}

/* writestop <n> <b|c>: that build offered to be written, stopped dead after
 * change n of its swap (0: no stop): answered "not written" (n 0:
 * accepted). */
static void write_stop(uint32_t n, const char *which)
{
    struct build x;
    struct update_answer a;
    memset(&x, 0, sizeof(x));
    memset(&a, 0, sizeof(a));
    status_t st = load_dir(&x, which[0] == 'c' ? DIR "c/" : DIR);
    x.flags = UPDATE_OFFER_WRITE | (n ? UPDATE_OFFER_STOP(n) : 0);
    if (st == OK)
        st = offer(&x, 2, UPDATE_OFFER_MAGIC, &a);
    bool good = st == OK && a.why == (n ? UPDATE_NOT_WRITTEN : UPDATE_ACCEPTED);
    failures += !good;
    printf("updtest: %s written, stopped after change %u of the swap (0: none): %s (%s), %s: %s\n",
           which, n, st == OK ? update_why_str(a.why) : "-", status_str(st == OK ? a.status : st),
           update_stick_str(a.stick), good ? "as expected" : "FAILED");
}

/* espcheck: which build (A, B, C: DIR "a/", DIR, DIR "c/") each of /esp's
 * two entries holds whole; one of them must. */
static void esp_check(void)
{
    static const char names[] = "ABC";
    static const char *const dirs[3] = { DIR "a/", DIR, DIR "c/" };
    struct sums s[3];
    int def = -1, prev = -1;
    bool ok = true;
    for (unsigned i = 0; ok && i < 3; i++)
        ok = sums_of(dirs[i], &s[i]);
    if (ok)
        entries(s, 3, &def, &prev);
    bool good = ok && (def >= 0 || prev >= 0);
    failures += !good;
    printf("updtest: the default entry boots %c, the previous build %c%s: %s\n",
           def >= 0 ? names[def] : '-', prev >= 0 ? names[prev] : '-',
           ok ? "" : " (no " DIR "a/, b or c build)", good ? "as expected" : "FAILED");
}

/* Every refusal, each on an offer channel of its own; the build offered
 * check-only (accepted, not loaded). */
static void bad(struct build *b)
{
    struct build nak;
    status_t st = make_nak(&nak);
    if (st != OK) {
        failures++;
        printf("updtest: no " DIR "nak.manifest, or its files (%s): FAILED\n", status_str(st));
    }
    bad_files(b);
    bad_manifests(b);
    if (st == OK)
        wrong_signatures(b, &nak);
    expect("bad magic", b, 2, UPDATE_OFFER_MAGIC ^ 1, UPDATE_BAD_OFFER, 0);
    expect("one handle", b, 1, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER, 0);
    b->flags = 1u << 31;   /* no such flag (the bits above CHECK_ONLY are WRITE, FORCE) */
    expect("unknown flag", b, 2, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER, 0);
    b->flags = UPDATE_OFFER_CHECK_ONLY;   /* passes, and is not loaded: */
    expect("check only", b, 2, UPDATE_OFFER_MAGIC, UPDATE_ACCEPTED, 0);
    b->flags = 0;
    other_net(b);
    extension_lines(b);
    if (st == OK)
        expect("not a kernel", &nak, 2, UPDATE_OFFER_MAGIC, UPDATE_NOT_LOADED, 0);
}

int main(int argc, char **argv)
{
    enum { GOOD, BAD, NOKEY, WRITEFAIL, WRITESTOP, ESPCHECK, MODES };
    static const char *const modes[MODES] = { "good",      "bad",       "nokey",
                                              "writefail", "writestop", "espcheck" };
    if (argc >= 2 && !strncmp(argv[1], "menu", 4))
        return menu_main(argc, argv);   /* menu.c */
    unsigned mode = 0;
    while (argc >= 2 && mode < MODES && strcmp(argv[1], modes[mode]))
        mode++;
    bool stop_args = argc == 4 && strlen(argv[2]) == 1 && argv[2][0] >= '0' &&
                     (unsigned)(argv[2][0] - '0') <= UPDATE_SWAP_OPS &&
                     (!strcmp(argv[3], "b") || !strcmp(argv[3], "c"));
    if (mode == MODES || (mode == WRITESTOP ? !stop_args : argc != 2)) {
        printf("usage: updtest good|bad|nokey|writefail|espcheck | writestop <0-%u> b|c | "
               "menuwrite <dir> [<0-%u>] | menubad <dir> | menucheck <old> <new>\n",
               UPDATE_SWAP_OPS, UPDATE_STOP_MAX);
        return 2;
    }
    initctl = svc_get(SVC_INIT);
    if (mode == ESPCHECK) {
        esp_check();
    } else if (mode == WRITESTOP) {
        write_stop(initctl ? (uint32_t)(argv[2][0] - '0') : 0, argv[3]);
    } else {
        struct build b;
        memset(&b, 0, sizeof(b));
        status_t st = initctl ? load(&b) : ERR_NOT_FOUND;
        if (st != OK) {
            printf("updtest: no init channel, or no " DIR " files (%s)\n", status_str(st));
            return 1;
        }
        if (mode == GOOD) {
            expect("the build", &b, 2, UPDATE_OFFER_MAGIC, UPDATE_ACCEPTED, 0);
        } else if (mode == NOKEY) {
            expect("no key", &b, 2, UPDATE_OFFER_MAGIC, UPDATE_NO_KEY, 0);
            b.flags = UPDATE_OFFER_CHECK_ONLY;
            expect("no key, check only", &b, 2, UPDATE_OFFER_MAGIC, UPDATE_NO_KEY, 0);
            b.flags = UPDATE_OFFER_WRITE;
            expect("no key, written to the stick", &b, 2, UPDATE_OFFER_MAGIC, UPDATE_NO_KEY, 0);
        } else if (mode == WRITEFAIL) {
            write_failures(&b);   /* each loads the build: the stored kernel is it afterwards */
        } else {
            bad(&b);
        }
    }
    printf("updtest: %s: %s\n", argv[1], failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
