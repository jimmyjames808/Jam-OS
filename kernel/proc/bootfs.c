/* bootfs: the read-only file image Limine loads as a module (init, the
 * test programs, init.cfg), so userspace can start before USB and FAT32.
 *
 * The image comes from the boot medium, which anyone with the stick can
 * rewrite, so it is validated like any other untrusted input before a
 * single entry is used (bootfs_validate). After that the entry table is
 * copied into kernel memory and every lookup works from the copy.
 *
 * The module's bytes stay where Limine put them. Its pages are
 * "kernel+modules" memory, which the PMM never hands out, and the HHDM maps
 * them read-only, so bootfs_data can give out plain pointers and
 * bootfs_find can make physical VMOs over them without copying. */
#include <stdbool.h>
#include <jam/boot.h>
#include <jam/bootfs.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vmo.h>

struct bootfs_file {
    char     name[BOOTFS_NAME_MAX];
    uint64_t offset;
    uint64_t size;
};

static struct bootfs_file *files;
static unsigned            nfiles;
static uint64_t            image_phys;
static uint64_t            image_size;   /* header's size: whole pages */

/* A bootfs path: 1..55 printable non-space ASCII characters, relative,
 * '/'-separated, no empty, "." or ".." components (mkbootfs.py's rules). */
static bool name_ok(const char *n)
{
    size_t len = 0;
    while (len < BOOTFS_NAME_MAX && n[len])
        len++;
    if (len == 0 || len == BOOTFS_NAME_MAX)
        return false;
    size_t comp = 0;   /* start of the current component */
    for (size_t i = 0; i <= len; i++) {
        char c = n[i];
        if (c == '/' || c == '\0') {
            size_t clen = i - comp;
            if (clen == 0 || (clen == 1 && n[comp] == '.') ||
                (clen == 2 && n[comp] == '.' && n[comp + 1] == '.'))
                return false;
            comp = i + 1;
        } else if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

static uint64_t page_end(uint64_t offset, uint64_t size)
{
    return offset + ALIGN_UP(size, PAGE_SIZE);
}

status_t bootfs_validate(const void *img, uint64_t len, const char **why)
{
#define REJECT(msg)                  \
    do {                             \
        *why = (msg);                \
        return ERR_INVALID_ARGS;     \
    } while (0)

    const uint8_t *p = img;
    struct bootfs_header h;
    if (len < sizeof(h))
        REJECT("shorter than the header");
    memcpy(&h, p, sizeof(h));
    if (memcmp(h.magic, BOOTFS_MAGIC, sizeof(h.magic)))
        REJECT("bad magic");
    if (h.version != BOOTFS_VERSION)
        REJECT("unknown version");
    if (h.size > len)
        REJECT("image size is bigger than the data");
    if (h.size % PAGE_SIZE)
        REJECT("image size is not a whole number of pages");
    if (h.count > BOOTFS_MAX_FILES)
        REJECT("too many files");
    /* No overflow: count <= 256 entries of 72 bytes. */
    uint64_t table_end = sizeof(h) + (uint64_t)h.count * sizeof(struct bootfs_entry);
    if (table_end > h.size)
        REJECT("entry table runs past the end");
    uint64_t data_start = ALIGN_UP(table_end, PAGE_SIZE);

    for (uint32_t i = 0; i < h.count; i++) {
        struct bootfs_entry e;
        memcpy(&e, p + sizeof(h) + (uint64_t)i * sizeof(e), sizeof(e));
        if (!name_ok(e.name))
            REJECT("bad file name");
        if (e.offset % PAGE_SIZE)
            REJECT("file data not page-aligned");
        if (e.offset < data_start)
            REJECT("file data overlaps the header");
        if (e.size == 0)
            REJECT("empty file");
        /* h.size is whole pages, so the page-rounded end fits too. */
        if (e.offset > h.size || e.size > h.size - e.offset)
            REJECT("file runs past the end");
        for (uint32_t j = 0; j < i; j++) {
            struct bootfs_entry o;
            memcpy(&o, p + sizeof(h) + (uint64_t)j * sizeof(o), sizeof(o));
            if (!strcmp(e.name, o.name))
                REJECT("duplicate file name");
            if (e.offset < page_end(o.offset, o.size) && o.offset < page_end(e.offset, e.size))
                REJECT("two files share a page");
        }
    }
    *why = NULL;
    return OK;
#undef REJECT
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && !memcmp(s + ls - lx, suffix, lx);
}

void bootfs_init(const struct boot_info *bi)
{
    const struct boot_module *m = NULL;
    for (size_t i = 0; i < bi->module_count; i++) {
        if (ends_with(bi->modules[i].path, BOOTFS_MODULE)) {
            m = &bi->modules[i];
            break;
        }
    }
    if (!m) {
        kprintf("bootfs: no %s module, no user programs\n", BOOTFS_MODULE);
        return;
    }
    if (m->phys % PAGE_SIZE) {
        kprintf("bootfs: %s at %lx is not page-aligned, ignored\n", m->path, m->phys);
        return;
    }
    const uint8_t *img = phys_to_virt(m->phys);
    const char *why;
    if (bootfs_validate(img, m->size, &why) != OK) {
        kprintf("bootfs: %s rejected: %s\n", m->path, why);
        return;
    }

    struct bootfs_header h;
    memcpy(&h, img, sizeof(h));
    struct bootfs_file *f = h.count ? kmalloc(h.count * sizeof(*f)) : NULL;
    if (h.count && !f) {
        kprintf("bootfs: no memory for the file table\n");
        return;
    }
    for (uint32_t i = 0; i < h.count; i++) {
        struct bootfs_entry e;
        memcpy(&e, img + sizeof(h) + (uint64_t)i * sizeof(e), sizeof(e));
        memcpy(f[i].name, e.name, sizeof(f[i].name));
        f[i].offset = e.offset;
        f[i].size = e.size;
    }
    files = f;
    nfiles = h.count;
    image_phys = m->phys;
    image_size = h.size;
    kprintf("bootfs: %u files, %lu KiB\n", nfiles, image_size / 1024);
}

static const struct bootfs_file *lookup(const char *name)
{
    for (unsigned i = 0; i < nfiles; i++)
        if (!strcmp(files[i].name, name))
            return &files[i];
    return NULL;
}

status_t bootfs_find(const char *name, struct vmo **out, uint64_t *size)
{
    const struct bootfs_file *f = lookup(name);
    if (!f)
        return ERR_NOT_FOUND;
    status_t st = vmo_create_physical(image_phys + f->offset, ALIGN_UP(f->size, PAGE_SIZE), 0,
                                      out);
    if (st == OK)
        *size = f->size;
    return st;
}

status_t bootfs_data(const char *name, const void **data, uint64_t *size)
{
    const struct bootfs_file *f = lookup(name);
    if (!f)
        return ERR_NOT_FOUND;
    *data = (const uint8_t *)phys_to_virt(image_phys) + f->offset;
    *size = f->size;
    return OK;
}

status_t bootfs_image(struct vmo **out, uint64_t *size)
{
    if (!image_size)
        return ERR_NOT_FOUND;
    status_t st = vmo_create_physical(image_phys, image_size, 0, out);
    if (st == OK)
        *size = image_size;
    return st;
}

unsigned bootfs_count(void)
{
    return nfiles;
}

const char *bootfs_name(unsigned i)
{
    return i < nfiles ? files[i].name : NULL;
}
