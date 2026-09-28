/* Static ELF64 executables: parse and validate into a load plan (Track C).
 * Nothing is mapped here; userboot (phase 2) and the libos loader apply
 * the plan. */
#pragma once

#include <stdint.h>
#include <jam/status.h>

#define ELF_MAX_SEGMENTS 8

struct elf_segment {
    uint64_t vaddr;      /* where the segment's first byte goes */
    uint64_t memsz;      /* bytes in memory (>= filesz; the rest is zero) */
    uint64_t file_off;   /* where its bytes are in the file */
    uint64_t filesz;
    unsigned flags;      /* ASPACE_READ / ASPACE_WRITE / ASPACE_EXEC */
};

struct elf_plan {
    uint64_t           entry;
    unsigned           nseg;
    struct elf_segment seg[ELF_MAX_SEGMENTS];
};

/* Validate image[0..size): ET_EXEC, x86-64, little-endian, 1..ELF_MAX_SEGMENTS
 * PT_LOAD segments, each inside [USER_BASE, USER_TOP), not overlapping,
 * never writable and executable, vaddr and file offset congruent mod 4 KiB,
 * file ranges inside the image, entry inside an executable segment.
 * ERR_INVALID_ARGS (with a log line saying why) otherwise. Never reads
 * outside the image. */
status_t elf_parse(const void *image, uint64_t size, struct elf_plan *out);
/* The same checks without the log line: *why says what is wrong (NULL on
 * success). For callers that expect rejections, like the fuzz test. */
status_t elf_check(const void *image, uint64_t size, struct elf_plan *out, const char **why);
