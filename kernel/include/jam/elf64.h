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

struct elf64_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

_Static_assert(sizeof(struct elf64_ehdr) == 64, "ELF64 header layout");
_Static_assert(sizeof(struct elf64_phdr) == 56, "ELF64 program header layout");
