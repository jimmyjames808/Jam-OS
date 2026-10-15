/* Verified pager VMOs (see vmo_verify.h): the table of page hashes and
 * the check of a page against it.
 *
 * The table is kept in whole pages of the buddy allocator (128 hashes
 * each), listed by pages of their own (512 addresses each): a program of
 * VMO_VERIFIED_MAX (1 GiB) has a table of 2048 pages (8 MiB) listed by 4
 * pages, never one big allocation (no block of several pages that a
 * fragmented machine may not have), and every page of it charged. It is
 * written once, before the VMO is sealed with it, and only read after, so
 * the check takes no lock: it runs with none held (pager_supply) or under
 * the VMO lock (vmo_pager_publish's second look), and takes nothing
 * itself. */
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sha256.h>
#include <jam/string.h>
#include <jam/vmo.h>
#include <jam/vmo_verify.h>

#define HASHES_PER_PAGE (PAGE_SIZE / VMO_VERIFIED_HASH_BYTES)   /* 128 */
#define LIST_PER_PAGE   (PAGE_SIZE / sizeof(uint8_t *))         /* 512 */
#define TABLE_MAX_PAGES (VMO_VERIFIED_MAX / PAGE_SIZE / HASHES_PER_PAGE)
#define LIST_MAX        ((TABLE_MAX_PAGES + LIST_PER_PAGE - 1) / LIST_PER_PAGE)   /* 4 */
_Static_assert(VMO_VERIFIED_HASH_BYTES == SHA256_BYTES, "the table holds SHA-256 digests");

struct vmo_verify {
    struct job *job;      /* charged a page for each page below (a reference), or NULL */
    uint64_t    pages;    /* pages of the VMO the table covers */
    uint64_t    npages;   /* pages of hashes, each HASHES_PER_PAGE of them */
    /* Hash page i is list[i / LIST_PER_PAGE][i % LIST_PER_PAGE] (kernel
     * virtual addresses); a page not made yet is NULL. */
    uint8_t   **list[LIST_MAX];
};

/* A zeroed page charged to vf's job, or NULL. */
static void *charged_page(struct vmo_verify *vf)
{
    if (job_charge(vf->job, JOB_LIMIT_PAGES, 1) != OK)
        return NULL;
    struct page *p = pmm_alloc_pages(0, PMM_ZERO);
    if (!p) {
        job_uncharge(vf->job, JOB_LIMIT_PAGES, 1);
        return NULL;
    }
    return page_to_virt(p);
}

static void charged_free(struct vmo_verify *vf, void *va)
{
    pmm_free_pages(virt_to_page(va), 0);
    job_uncharge(vf->job, JOB_LIMIT_PAGES, 1);
}

/* Free vf and its pages, uncharging them; pages not yet made are NULL. */
void vmo_verify_free(struct vmo_verify *vf)
{
    for (uint64_t l = 0; l < LIST_MAX; l++) {
        if (!vf->list[l])
            continue;
        for (uint64_t k = 0; k < LIST_PER_PAGE; k++)
            if (vf->list[l][k])
                charged_free(vf, vf->list[l][k]);
        charged_free(vf, vf->list[l]);
    }
    job_unref(vf->job);
    kfree(vf);
}

/* Page i of the table: a fresh page, charged, filled from table's bytes
 * [i * PAGE_SIZE, i * PAGE_SIZE + len) and zero past them; its list page
 * first if it is the first on it. */
static status_t copy_page(struct vmo_verify *vf, struct vmo *table, uint64_t i, uint64_t len)
{
    uint8_t ***list = &vf->list[i / LIST_PER_PAGE];
    if (!*list && !(*list = charged_page(vf)))
        return ERR_NO_MEMORY;
    uint8_t **slot = &(*list)[i % LIST_PER_PAGE];
    if (!(*slot = charged_page(vf)))
        return ERR_NO_MEMORY;
    return vmo_read(table, i * PAGE_SIZE, *slot, len);   /* vmo_verify_free's from here on */
}

status_t vmo_verify_new(struct job *job, struct vmo *table, uint64_t pages,
                        struct vmo_verify **out)
{
    if (pages > VMO_VERIFIED_MAX >> PAGE_SHIFT)
        return ERR_OUT_OF_RANGE;
    uint64_t bytes = pages * VMO_VERIFIED_HASH_BYTES;   /* 8 MiB at most: no overflow */
    if (vmo_size(table) < bytes)
        return ERR_INVALID_ARGS;
    struct vmo_verify *vf = kzalloc(sizeof(*vf));
    if (!vf)
        return ERR_NO_MEMORY;
    job_ref(job);
    vf->job = job;
    vf->pages = pages;
    vf->npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;   /* TABLE_MAX_PAGES at most */
    status_t st = OK;
    for (uint64_t i = 0; i < vf->npages && st == OK; i++) {
        uint64_t len = bytes - i * PAGE_SIZE < PAGE_SIZE ? bytes - i * PAGE_SIZE : PAGE_SIZE;
        st = copy_page(vf, table, i, len);
    }
    if (st != OK) {
        vmo_verify_free(vf);
        return st;
    }
    *out = vf;
    return OK;
}

bool vmo_verify_match(const struct vmo_verify *vf, uint64_t idx, const void *page)
{
    if (idx >= vf->pages)
        return false;   /* no approved bytes for it */
    uint8_t digest[SHA256_BYTES];
    sha256(page, PAGE_SIZE, digest);
    uint64_t i = idx / HASHES_PER_PAGE;
    const uint8_t *want = vf->list[i / LIST_PER_PAGE][i % LIST_PER_PAGE] +
                          (idx % HASHES_PER_PAGE) * VMO_VERIFIED_HASH_BYTES;
    return memcmp(digest, want, SHA256_BYTES) == 0;
}

bool vmo_verify_supply(struct vmo *v, uint64_t idx, const void *page, bool *checked)
{
    const struct vmo_verify *vf = vmo_verifier(v);
    *checked = vf != NULL;
    return !vf || vmo_verify_match(vf, idx, page);
}
