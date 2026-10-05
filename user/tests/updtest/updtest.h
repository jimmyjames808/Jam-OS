/* updtest's shared parts: the build as files on /data, the offer to init
 * and its answer, and /esp's files once a stick write has brought it back
 * (main.c, which has the program's header and its cases); the boot
 * menu's cases (menu.c). */
#pragma once

#include <os.h>
#include <update.h>

#define DIR         "/data/update/"
#define ANSWER_WAIT (300 * NS_PER_S)  /* init copies and hashes ~10 MB, and may write the stick */

/* One offer: the manifest's text and the files, each a VMO and the
 * length claimed for it. */
struct build {
    char     manifest[UPDATE_MANIFEST_MAX];
    uint32_t manifest_len;
    handle_t vmo[UPDATE_FILES];
    uint64_t bytes[UPDATE_FILES];
    uint32_t flags;   /* the offer's (UPDATE_OFFER_CHECK_ONLY) */
};

extern handle_t initctl;     /* init's control channel */
extern unsigned failures;    /* cases that didn't go as expected */

/* The build whose files are in folder dir ("/data/update/", ...). */
status_t load_dir(struct build *b, const char *dir);
/* Offer b (its VMOs duplicated: b keeps its own) with `handles` of them
 * and this magic; init's answer into *a. */
status_t offer(const struct build *b, unsigned handles, uint32_t magic, struct update_answer *a);
/* One case: b offered, the answer must be `why` (about file `file`). */
void     expect(const char *name, const struct build *b, unsigned handles, uint32_t magic,
                uint32_t why, uint32_t file);
/* /esp's file at path into *v (n bytes): ERR_NOT_FOUND if /esp is there
 * and the file isn't (/esp comes back a moment after a stick write). */
status_t esp_file(const char *path, handle_t *v, uint64_t *n);
