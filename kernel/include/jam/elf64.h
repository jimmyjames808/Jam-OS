/* The ELF64 on-disk structures and constants the ELF parser (proc/elf.c)
 * and its tests need. Only what static x86-64 executables use. */
#pragma once

#include <stdint.h>

#define EI_NIDENT     16
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6
#define EI_OSABI      7
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1
#define ELFOSABI_SYSV 0
#define ELFOSABI_GNU  3

#define ET_REL        1
#define ET_EXEC       2
#define ET_DYN        3
#define EM_X86_64     62

#define PT_NULL       0
#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_INTERP     3
#define PT_NOTE       4
#define PT_SHLIB      5
#define PT_PHDR       6
#define PT_TLS        7

#define PF_X          1u
#define PF_W          2u
#define PF_R          4u

/* The file header (ELF-64 Object File Format, version 1.5, section 3). */
struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];   /* magic, class, byte order, version, OS ABI */
    uint16_t e_type;               /* ET_* */
    uint16_t e_machine;            /* EM_X86_64 */
    uint32_t e_version;            /* EV_CURRENT */
    uint64_t e_entry;              /* entry point */
    uint64_t e_phoff;              /* file offset of the program headers */
    uint64_t e_shoff;              /* file offset of the section headers */
    uint32_t e_flags;              /* processor flags (0 on x86-64) */
    uint16_t e_ehsize;             /* size of this header */
    uint16_t e_phentsize;          /* size of one program header */
    uint16_t e_phnum;              /* number of program headers */
    uint16_t e_shentsize;          /* size of one section header */
    uint16_t e_shnum;              /* number of section headers */
    uint16_t e_shstrndx;           /* section holding the section names */
};

/* A program header: one segment (same spec, section 6). */
struct elf64_phdr {
    uint32_t p_type;     /* PT_* */
    uint32_t p_flags;    /* PF_R / PF_W / PF_X */
    uint64_t p_offset;   /* where its bytes are in the file */
    uint64_t p_vaddr;    /* where it goes in memory */
    uint64_t p_paddr;    /* physical address (unused) */
    uint64_t p_filesz;   /* bytes in the file */
    uint64_t p_memsz;    /* bytes in memory (the rest is zero) */
    uint64_t p_align;    /* alignment of p_vaddr and p_offset */
};

_Static_assert(sizeof(struct elf64_ehdr) == 64, "ELF64 header layout");
_Static_assert(sizeof(struct elf64_phdr) == 56, "ELF64 program header layout");
