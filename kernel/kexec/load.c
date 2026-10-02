/* kexec's two loads: the stored kernel at boot, from the boot modules,
 * and its replacement from two VMOs (the kexec_load system call, which
 * init makes when the files on /esp changed).
 *
 * The stored kernel is this boot's own kernel and bootfs: jamos.elf loaded
 * a second time by Limine as a module, or handed on as one by the kernel
 * that kexec'd this one (the running image's .data and .bss have changed
 * since it started, so it is no source), and the bootfs module, which
 * nothing ever writes.
 *
 * Its command line (kexec_next_cmdline) is this boot's, keeping only the
 * words that describe the machine and how the boot looks: whichever way it
 * is started, after a reboot or after a panic in a test entry, it is a
 * plain boot to the shell. It also says which disk the machine booted
 * from (bootdisk=N, the MBR disk id: <jam/boot.h>), which only the
 * loader knew. */
#include <jam/boot.h>
#include <jam/bootfs.h>
#include <jam/cmdline.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vmo.h>
#include "kexec_internal.h"

#define CRASHTEST_MAX 16   /* a crash test's name */
#define KERNEL_FILE_MAX (64ull << 20)
#define BOOTFS_FILE_MAX (256ull << 20)

/* Boot words the next kernel keeps: the hardware switches (TESTING.md
 * lists them), which it needs as much as this one did, the ones that
 * choose how a plain boot looks, and `net` (the PC's network card bound:
 * a reboot, a panic and `update` on the network boot come back with the
 * network, so netlog can send the panicked boot's log and the fetched
 * build can be reached; `netprobe` and `netsend` are one-shot tests,
 * not kept). */
static const char *const kept_words[] = {
    "shell", "verbose", "nosplash", "nousb", "smp=loader", "nopcid", "forcepcid",
    "nodeadline", "noserialirq", "nooneshot", "nofpuopt", "nokmcache", "nospinidle",
    "noplaceorder", "noaffinepair", "hidboot", "net",
};
/* ... and key=value words. */
static const char *const kept_keys[] = { "crashkernel=", "idlespin=", "bootdisk=" };

/* A `vlan` word, well-formed or not ("vlan", "vlan=", "vlan=off",
 * "vlan=21"): all are kept, and first, so a reboot, a panic and `update`
 * come back on the same VLAN, and a boot with no VLAN never comes back
 * on the default one because its word was dropped or didn't fit.
 * (`netprobe` and `netsend` are not kept: the listen-only probe and the
 * send test run only when their boot entry is picked.) */
static bool vlan_word(const char *w, size_t n)
{
    return n >= 4 && !memcmp(w, "vlan", 4) && (n == 4 || w[4] == '=');
}

static bool kept(const char *w, size_t n)
{
    for (size_t i = 0; i < sizeof(kept_words) / sizeof(kept_words[0]); i++)
        if (strlen(kept_words[i]) == n && !memcmp(w, kept_words[i], n))
            return true;
    for (size_t i = 0; i < sizeof(kept_keys) / sizeof(kept_keys[0]); i++) {
        size_t kl = strlen(kept_keys[i]);
        if (n > kl && !memcmp(w, kept_keys[i], kl))
            return true;
    }
    return false;
}

/* s begins with prefix (s is read no further than its first mismatch). */
static bool starts_with(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (*s != *prefix)
            return false;
    return true;
}

/* n bytes of w onto buf's string at `at`, as far as they fit (buf stays
 * terminated); the new end. */
static size_t append_bytes(char *buf, size_t size, size_t at, const char *w, size_t n)
{
    for (size_t i = 0; i < n && at + 1 < size; i++)
        buf[at++] = w[i];
    buf[at] = '\0';
    return at;
}

/* ... after a space if buf has something already. A word that doesn't
 * fit whole is left out. */
static size_t append_word(char *buf, size_t size, size_t at, const char *w, size_t n)
{
    size_t need = (at ? 1 : 0) + n;
    if (at + need + 1 > size)
        return at;
    if (at)
        at = append_bytes(buf, size, at, " ", 1);
    return append_bytes(buf, size, at, w, n);
}

void kexec_next_cmdline(const char *from, char *buf, size_t size)
{
    if (!size)
        return;
    buf[0] = '\0';
    size_t at = 0;
    for (int pass = 0; pass < 2; pass++) {   /* the vlan words, then the rest */
        for (const char *p = from; *p;) {
            while (*p == ' ')
                p++;
            size_t n = 0;
            while (p[n] && p[n] != ' ')
                n++;
            if (n && (pass == 0 ? vlan_word(p, n) : !vlan_word(p, n) && kept(p, n)))
                at = append_word(buf, size, at, p, n);
            p += n;
        }
    }
    for (const char *p = from; *p; p++) {
        if ((p != from && p[-1] != ' ') || !starts_with(p, "crashtest="))
            continue;
        size_t n = 0;
        while (p[10 + n] && p[10 + n] != ' ' && n < CRASHTEST_MAX)
            n++;
        char word[4 + CRASHTEST_MAX] = "test";
        memcpy(word + 4, p + 10, n);
        if (n)
            at = append_word(buf, size, at, word, 4 + n);
        break;
    }
}

/* The stored kernel's command line: kexec_next_cmdline's, and the boot
 * disk if the loader named it (after a kexec the word is kept already). */
static void stored_cmdline(char *buf, size_t size)
{
    kexec_next_cmdline(cmdline_get(), buf, size);
    if (!boot_disk_id() || cmdline_get_u64("bootdisk", 0, 0))
        return;
    char w[24];
    ksnprintf(w, sizeof(w), "bootdisk=%u", boot_disk_id());
    (void)append_word(buf, size, strlen(buf), w, strlen(w));   /* no room: left out */
}

/* The boot module whose path ends in `suffix`, or NULL. */
static const struct boot_module *module(const char *suffix)
{
    size_t sl = strlen(suffix);
    for (size_t i = 0; i < kx_boot->module_count; i++) {
        const struct boot_module *m = &kx_boot->modules[i];
        size_t pl = strlen(m->path);
        if (pl >= sl && !strcmp(m->path + pl - sl, suffix))
            return m;
    }
    return NULL;
}

/* Load under the lock. A refused image leaves the state as it was
 * (kx_build writes nothing then): what was stored stays armed. */
static status_t load(const struct kx_image *im)
{
    mutex_lock(&kx_lock);
    int s = __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE);
    if (s == KX_OFF || s == KX_JUMPING) {
        mutex_unlock(&kx_lock);
        return s == KX_OFF ? ERR_NOT_SUPPORTED : ERR_BAD_STATE;
    }
    status_t st = kx_build(im);
    /* Release: the panic path reads kx only once it sees ARMED. */
    if (st == OK)
        __atomic_store_n(&kx_state, KX_ARMED, __ATOMIC_RELEASE);
    mutex_unlock(&kx_lock);
    return st;
}

void kexec_load_stored(void)
{
    if (__atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) != KX_EMPTY)
        return;
    kx_window_init();
    const struct boot_module *k = module(KEXEC_KERNEL_MODULE), *b = module(BOOTFS_MODULE);
    if (!k || !b) {
        kprintf("kexec: no %s module (boot/limine.conf): no stored kernel\n",
                k ? BOOTFS_MODULE : KEXEC_KERNEL_MODULE);
        return;
    }
    static char cmd[KEXEC_CMDLINE];
    stored_cmdline(cmd, sizeof(cmd));
    struct kx_image im = {
        .kernel = phys_to_virt(k->phys), .kernel_size = k->size,
        .bootfs = phys_to_virt(b->phys), .bootfs_size = b->size, .cmdline = cmd,
    };
    if (load(&im) == OK)
        kprintf("kexec: stored kernel armed, command line \"%s\"\n", cmd);
    else
        kprintf("kexec: no stored kernel: a panic halts on its screen\n");
}

bool kexec_armed(void)
{
    return __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) == KX_ARMED;
}

status_t kexec_load_image(struct vmo *kernel, struct vmo *bootfs, const char *cmdline)
{
    if (__atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) == KX_OFF)
        return ERR_NOT_SUPPORTED;
    char next[KEXEC_CMDLINE];   /* the default: kexec_next_cmdline's */
    uint64_t ks = vmo_size(kernel), bs = vmo_size(bootfs);
    if (!ks || !bs || ks > KERNEL_FILE_MAX || bs > BOOTFS_FILE_MAX)
        return ERR_NO_RESOURCES;
    if (cmdline && strlen(cmdline) >= KEXEC_CMDLINE)
        return ERR_INVALID_ARGS;
    void *kp = NULL, *bp = NULL;
    status_t st = vmo_map_kernel(kernel, 0, ks, 0, &kp);
    if (st == OK)
        st = vmo_map_kernel(bootfs, 0, bs, 0, &bp);
    if (st == OK) {
        if (!cmdline || !cmdline[0]) {
            stored_cmdline(next, sizeof(next));
            cmdline = next;
        }
        struct kx_image im = {
            .kernel = kp, .kernel_size = ks, .bootfs = bp, .bootfs_size = bs, .cmdline = cmdline,
        };
        st = load(&im);
    }
    if (bp)
        (void)vmo_unmap_kernel(bootfs, bp);   /* our own mapping: nothing to do if it failed */
    if (kp)
        (void)vmo_unmap_kernel(kernel, kp);
    return st;
}
