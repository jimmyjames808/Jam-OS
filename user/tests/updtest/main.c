/* updtest: init's update check (user/services/init/update.c, <update.h>)
 * fed from files instead of the network, so it is tested before sockets
 * exist; tools/update-test.sh runs it in QEMU.
 *
 * The files: /data/update/manifest, jamos.elf and bootfs.img, put there
 * by the test script (the manifest made by tools/update-server.py, the
 * boot image a copy of the build's with one more file, update-marker.txt,
 * so the boot after the update shows which build runs). The shell gives
 * it what its list asks for: /data (read) and init's control channel,
 * whose update_offer is the channel bin/update's offer will travel on.
 *
 *   updtest good   the build as it is: accepted (init loads it as the
 *                  stored kernel; the script's `reboot` then runs it)
 *   updtest bad    every refusal, each on an offer channel of its own, and
 *                  each must be refused for its own reason: a byte of the
 *                  kernel or of the boot image changed (the SHA-256), the
 *                  kernel 4 KiB longer than the manifest says or cut to
 *                  half (the length), a VMO shorter than the length it
 *                  claims, a manifest of garbage, cut short, signed (this
 *                  build can't check a signature) or of another format,
 *                  an offer with a bad magic, one handle or an unknown
 *                  flag, and two files that match their manifest but are
 *                  no kernel (the kernel's own refusal); and the build
 *                  as it is, offered UPDATE_OFFER_CHECK_ONLY: accepted,
 *                  not loaded. The script's `reboot` then shows the
 *                  stored kernel unchanged.
 * Exit 0 when each case went as expected. */
#include <idl/initctl.h>
#include <os.h>
#include <update.h>
#include <wants.h>

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc init\n"
          "mount /data r\n");

#define DIR         "/data/update/"
#define ANSWER_WAIT (60 * NS_PER_S)   /* init copies and hashes ~9 MB */
#define CHUNK       (64u << 10)       /* bytes copied at a time */

/* One offer: the manifest's text and the two files, each a VMO and the
 * length claimed for it. */
struct build {
    char     manifest[UPDATE_MANIFEST_MAX];
    uint32_t manifest_len;
    handle_t vmo[UPDATE_FILES];
    uint64_t bytes[UPDATE_FILES];
    uint32_t flags;   /* the offer's (UPDATE_OFFER_CHECK_ONLY) */
};

static handle_t initctl;
static unsigned failures;

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

static status_t load(struct build *b)
{
    handle_t m;
    uint64_t n = 0;
    status_t st = file_read_vmo(DIR "manifest", UPDATE_MANIFEST_MAX, &m, &n);
    if (st != OK)
        return st;
    st = jam_vmo_read(m, 0, b->manifest, n);
    jam_handle_close(m);
    b->manifest_len = (uint32_t)n;
    if (st == OK)
        st = file_read_vmo(DIR "jamos.elf", UPDATE_FILE_MAX, &b->vmo[UPDATE_KERNEL],
                           &b->bytes[UPDATE_KERNEL]);
    if (st == OK)
        st = file_read_vmo(DIR "bootfs.img", UPDATE_FILE_MAX, &b->vmo[UPDATE_BOOTFS],
                           &b->bytes[UPDATE_BOOTFS]);
    return st;
}

/* Offer b (its VMOs duplicated: b keeps its own) with `handles` of them
 * and this magic; init's answer into *a. */
static status_t offer(const struct build *b, unsigned handles, uint32_t magic,
                      struct update_answer *a)
{
    handle_t ch;
    status_t st = initctl_update_offer_until(initctl, now() + 5 * NS_PER_S, &ch);
    if (st != OK)
        return st;
    struct update_offer *o = calloc(1, sizeof(*o));
    handle_t hs[UPDATE_FILES];
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

/* One case: b offered, the answer must be `why` (about file `file`). */
static void expect(const char *name, const struct build *b, unsigned handles, uint32_t magic,
                   uint32_t why, uint32_t file)
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
    printf("updtest: %s: %s%s%s (%s, %s) %s\n", name, st == OK ? update_why_str(a.why) : "-",
           per_file ? ": " : "", per_file ? update_file_name(a.file) : "",
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

/* b with another manifest text (len bytes). */
static void bad_manifest(const struct build *b, const char *name, const void *text, size_t len)
{
    struct build v = *b;
    memcpy(v.manifest, text, len);
    v.manifest_len = (uint32_t)len;
    expect(name, &v, 2, UPDATE_OFFER_MAGIC, UPDATE_BAD_MANIFEST, 0);
}

static void bad_manifests(const struct build *b)
{
    char text[UPDATE_MANIFEST_MAX];
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof(text); i++) {
        x = x * 1103515245u + 12345u;
        text[i] = (char)(x >> 16);
    }
    bad_manifest(b, "garbage manifest", text, sizeof(text));
    bad_manifest(b, "manifest cut short", b->manifest, b->manifest_len / 2);
    size_t n = b->manifest_len;
    memcpy(text, b->manifest, n);
    if (n > 1 && n + 6 < sizeof(text)) {
        memcpy(text + n - 1, " 00ff\n", 6);   /* "signature" with a value */
        bad_manifest(b, "signed manifest", text, n + 5);
    }
    memcpy(text, b->manifest, n);
    text[13] = '2';   /* "jamos-update 2" */
    bad_manifest(b, "another format", text, n);
}

/* Two files that match their manifest exactly but are no kernel: only
 * kexec_load can refuse them. */
static void not_a_kernel(void)
{
    static uint8_t k[8192], s[4096];
    memset(k, 0x55, sizeof(k));
    memset(s, 0xaa, sizeof(s));
    uint8_t dk[SHA256_BYTES], ds[SHA256_BYTES];
    char hk[2 * SHA256_BYTES + 1], hs[2 * SHA256_BYTES + 1];
    sha256(k, sizeof(k), dk);
    sha256(s, sizeof(s), ds);
    sha256_hex(dk, hk);
    sha256_hex(ds, hs);
    struct build b = { .bytes = { sizeof(k), sizeof(s) } };
    int n = snprintf(b.manifest, sizeof(b.manifest), "jamos-update 1\nversion not-a-kernel\n"
                     "git 0000000\nkernel %u %s\nbootfs %u %s\nsignature\n",
                     (unsigned)sizeof(k), hk, (unsigned)sizeof(s), hs);
    b.manifest_len = (uint32_t)n;
    if (jam_vmo_create(sizeof(k), 0, HANDLE_INVALID, &b.vmo[0]) != OK ||
        jam_vmo_create(sizeof(s), 0, HANDLE_INVALID, &b.vmo[1]) != OK ||
        jam_vmo_write(b.vmo[0], 0, k, sizeof(k)) != OK ||
        jam_vmo_write(b.vmo[1], 0, s, sizeof(s)) != OK) {
        failures++;
        printf("updtest: not a kernel: can't make its files: FAILED\n");
        return;
    }
    expect("not a kernel", &b, 2, UPDATE_OFFER_MAGIC, UPDATE_NOT_LOADED, 0);
    jam_handle_close(b.vmo[0]);
    jam_handle_close(b.vmo[1]);
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "good") && strcmp(argv[1], "bad"))) {
        printf("usage: updtest good|bad\n");
        return 2;
    }
    initctl = svc_get(SVC_INIT);
    struct build b;
    memset(&b, 0, sizeof(b));
    status_t st = initctl ? load(&b) : ERR_NOT_FOUND;
    if (st != OK) {
        printf("updtest: no init channel, or no " DIR " files (%s)\n", status_str(st));
        return 1;
    }
    if (!strcmp(argv[1], "good")) {
        expect("the build", &b, 2, UPDATE_OFFER_MAGIC, UPDATE_ACCEPTED, 0);
    } else {
        bad_files(&b);
        bad_manifests(&b);
        expect("bad magic", &b, 2, UPDATE_OFFER_MAGIC ^ 1, UPDATE_BAD_OFFER, 0);
        expect("one handle", &b, 1, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER, 0);
        b.flags = UPDATE_OFFER_CHECK_ONLY << 1;
        expect("unknown flag", &b, 2, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER, 0);
        b.flags = UPDATE_OFFER_CHECK_ONLY;   /* passes, and is not loaded: */
        expect("check only", &b, 2, UPDATE_OFFER_MAGIC, UPDATE_ACCEPTED, 0);
        not_a_kernel();
    }
    printf("updtest: %s: %s\n", argv[1], failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
