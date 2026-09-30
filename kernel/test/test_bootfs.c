/* bootfs: the boot image's files are there and served correctly, and the
 * validator refuses corrupted images without ever reading past them. */
#include <jam/bootfs.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vmo.h>

static const char *const expected[] = { "bin/init", "bin/utest", "init.cfg" };

static bool listed(const char *name)
{
    for (unsigned i = 0; i < bootfs_count(); i++)
        if (!strcmp(bootfs_name(i), name))
            return true;
    return false;
}

/* bootfs_find's VMO holds exactly the bytes bootfs_data points at, then
 * zeros to the end of the page. */
static void check_file(const char *name)
{
    const void *data;
    uint64_t size, vsize;
    struct vmo *v;
    KT_EQ(bootfs_data(name, &data, &size), OK);
    KT_ASSERT(size > 0);
    KT_EQ(bootfs_find(name, &v, &vsize), OK);
    KT_EQ(vsize, size);
    KT_EQ(vmo_size(v), ALIGN_UP(size, PAGE_SIZE));
    void *va;
    KT_EQ(vmo_map_kernel(v, 0, vmo_size(v), 0, &va), OK);
    KT_EQ(memcmp(va, data, size), 0);
    const uint8_t *tail = (const uint8_t *)va + size;
    for (uint64_t i = 0; i < vmo_size(v) - size; i++)
        KT_EQ(tail[i], 0);
    KT_EQ(vmo_unmap_kernel(v, va), OK);
    kobject_unref(vmo_kobject(v));
}

KTEST(bootfs_files)
{
    KT_ASSERT(bootfs_count() >= 3);
    KT_ASSERT(bootfs_name(bootfs_count()) == NULL);
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        KT_ASSERT(listed(expected[i]));
        check_file(expected[i]);
    }

    struct vmo *v;
    const void *data;
    uint64_t size;
    KT_EQ(bootfs_find("bin/nope", &v, &size), ERR_NOT_FOUND);
    KT_EQ(bootfs_data("", &data, &size), ERR_NOT_FOUND);
    KT_EQ(bootfs_data("bin", &data, &size), ERR_NOT_FOUND);

    /* init.cfg names utest. */
    KT_EQ(bootfs_data("init.cfg", &data, &size), OK);
    const char *cfg = data;
    bool found = false;
    for (uint64_t i = 0; i + 9 <= size && !found; i++)
        found = !memcmp(cfg + i, "bin/utest", 9);
    KT_ASSERT(found);

    /* The whole-image VMO starts with a header that validates. */
    KT_EQ(bootfs_image(&v, &size), OK);
    KT_EQ(vmo_size(v), size);
    void *va;
    const char *why;
    KT_EQ(vmo_map_kernel(v, 0, size, 0, &va), OK);
    KT_EQ(bootfs_validate(va, size, &why), OK);
    KT_EQ(vmo_unmap_kernel(v, va), OK);
    kobject_unref(vmo_kobject(v));
}

/* ---- corrupted images -------------------------------------------------- */

/* A small valid image: header, two entries, "a" (100 bytes) on page 1 and
 * "dir/b" (5000 bytes) on pages 2-3. */
#define IMG_SIZE (4 * PAGE_SIZE)

static struct bootfs_header *hdr(uint8_t *img)
{
    return (struct bootfs_header *)img;
}

static struct bootfs_entry *ent(uint8_t *img, unsigned i)
{
    return (struct bootfs_entry *)(img + sizeof(struct bootfs_header)) + i;
}

static void make_image(uint8_t *img)
{
    memset(img, 0, IMG_SIZE);
    memcpy(hdr(img)->magic, BOOTFS_MAGIC, 8);
    hdr(img)->version = BOOTFS_VERSION;
    hdr(img)->count = 2;
    hdr(img)->size = IMG_SIZE;
    memcpy(ent(img, 0)->name, "a", 2);
    ent(img, 0)->offset = PAGE_SIZE;
    ent(img, 0)->size = 100;
    memcpy(ent(img, 1)->name, "dir/b", 6);
    ent(img, 1)->offset = 2 * PAGE_SIZE;
    ent(img, 1)->size = 5000;
    memset(img + PAGE_SIZE, 'a', 100);
    memset(img + 2 * PAGE_SIZE, 'b', 5000);
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t rng(void) { return kt_rng(&rng_state); }

/* Everything bootfs_init relies on after a successful validation. */
static void check_accepted(uint8_t *img, uint64_t len)
{
    struct bootfs_header *h = hdr(img);
    KT_ASSERT(h->size <= len && h->size % PAGE_SIZE == 0 && h->count <= BOOTFS_MAX_FILES);
    for (unsigned i = 0; i < h->count; i++) {
        struct bootfs_entry *e = ent(img, i);
        KT_ASSERT(strlen(e->name) < BOOTFS_NAME_MAX);
        KT_ASSERT(e->offset % PAGE_SIZE == 0 && e->size > 0);
        KT_ASSERT(e->offset <= h->size && e->size <= h->size - e->offset);
    }
}

KTEST(bootfs_corrupt)
{
    /* The image sits in a VMO mapped flush against the guard gap vmm_reserve
     * leaves after every vmap mapping: a read past its end faults. */
    struct vmo *v;
    uint8_t *img;
    const char *why;
    KT_EQ(vmo_create(IMG_SIZE, 0, &v), OK);
    KT_EQ(vmo_map_kernel(v, 0, IMG_SIZE, VM_WRITE, (void **)&img), OK);

    make_image(img);
    KT_EQ(bootfs_validate(img, IMG_SIZE, &why), OK);
    KT_EQ(bootfs_validate(img, IMG_SIZE + PAGE_SIZE - 1, &why), OK);   /* len may exceed size */

    enum { N_CASES = 24 };
    for (int c = 0; c < N_CASES; c++) {
        make_image(img);
        uint64_t len = IMG_SIZE;
        struct bootfs_header *h = hdr(img);
        struct bootfs_entry *e0 = ent(img, 0), *e1 = ent(img, 1);
        switch (c) {
        case 0:  h->magic[0] = 'X'; break;
        case 1:  h->version = 2; break;
        case 2:  h->size = IMG_SIZE + PAGE_SIZE; break;          /* bigger than the data */
        case 3:  h->size = IMG_SIZE - 1; break;                  /* not whole pages */
        case 4:  h->count = BOOTFS_MAX_FILES + 1; break;
        case 5:  h->count = 0xffffffffu; break;
        case 6:  h->size = PAGE_SIZE; h->count = 57; break;      /* table past the end */
        case 7:  e0->offset = PAGE_SIZE + 8; break;              /* unaligned */
        case 8:  e0->offset = 0; break;                          /* over the header */
        case 9:  e0->size = 0; break;
        case 10: e1->size = 3 * PAGE_SIZE; break;                /* past the end */
        case 11: e1->offset = 0xfffffffffffff000ull; break;      /* offset + size wraps */
        case 12: e1->size = 0xffffffffffffff00ull; break;
        case 13: e1->offset = PAGE_SIZE; break;                  /* same page as "a" */
        case 14: e0->size = PAGE_SIZE + 1; break;                /* runs into "dir/b"'s page */
        case 15: memcpy(e1->name, "a", 2); break;                /* duplicate */
        case 16: memset(e0->name, 'x', BOOTFS_NAME_MAX); break;  /* no NUL */
        case 17: e0->name[0] = '\0'; break;                      /* empty */
        case 18: memcpy(e0->name, "/a", 3); break;
        case 19: memcpy(e0->name, "a/../b", 7); break;
        case 20: memcpy(e0->name, "a//b", 5); break;
        case 21: memcpy(e0->name, "a b", 4); break;
        case 22: e0->name[0] = '\x7f'; break;
        case 23: len = 23; memmove(img + IMG_SIZE - len, img, len); break;   /* truncated */
        }
        uint8_t *p = img + IMG_SIZE - len;
        why = NULL;
        if (bootfs_validate(p, len, &why) != ERR_INVALID_ARGS)
            ktest_fail("case %d accepted", c);
        KT_ASSERT(why != NULL);
    }

    /* Random damage to the header and entry table, and random truncation:
     * never a fault; anything accepted must really be sound. */
    unsigned accepted = 0;
    for (int it = 0; it < 400; it++) {
        make_image(img);
        unsigned n = 1 + rng() % 4;
        for (unsigned k = 0; k < n; k++) {
            uint64_t off = rng() % (sizeof(struct bootfs_header) + 2 * sizeof(struct bootfs_entry));
            if (rng() % 4 == 0 && off + 8 <= IMG_SIZE) {
                static const uint64_t vals[] = { 0, 1, PAGE_SIZE, IMG_SIZE, IMG_SIZE - 1,
                                                 ~0ull, 1ull << 63, 0xfffffffffffff000ull };
                uint64_t val = vals[rng() % (sizeof(vals) / sizeof(vals[0]))];
                memcpy(img + (off & ~7ull), &val, 8);
            } else {
                img[off] = (uint8_t)rng();
            }
        }
        uint64_t len = IMG_SIZE;
        if (rng() % 8 == 0) {
            len = rng() % IMG_SIZE;
            memmove(img + IMG_SIZE - len, img, len);
        }
        uint8_t *p = img + IMG_SIZE - len;
        if (bootfs_validate(p, len, &why) == OK) {
            check_accepted(p, len);
            accepted++;
        }
    }
    kprintf("bootfs_corrupt: %d directed cases rejected, 400 random: %u still valid\n",
            N_CASES, accepted);

    KT_EQ(vmo_unmap_kernel(v, img), OK);
    kobject_unref(vmo_kobject(v));
}
