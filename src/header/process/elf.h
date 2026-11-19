#ifndef _ELF_H
#define _ELF_H

#include <stdint.h>
#include <stdbool.h>

/**
 * The parts of ELF32 (System V ABI, i386) the kernel loader and the tools use
 * (book 4.3.6: separate shell commands are ELF executables).
 */

#define ELF_MAGIC0     0x7F
#define ELF_MAGIC1     'E'
#define ELF_MAGIC2     'L'
#define ELF_MAGIC3     'F'
#define ELF_CLASS_32   1
#define ELF_DATA_LSB   1
#define ELF_TYPE_EXEC  2
#define ELF_MACHINE_386 3

#define ELF_PT_NULL    0
#define ELF_PT_LOAD    1

#define ELF_PF_X       0x1
#define ELF_PF_W       0x2
#define ELF_PF_R       0x4

#define ELF_SHT_PROGBITS 1
#define ELF_SHT_NOBITS   8
#define ELF_SHF_WRITE    0x1
#define ELF_SHF_ALLOC    0x2
#define ELF_SHF_EXEC     0x4

struct ELF32Header {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct ELF32ProgramHeader {
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
} __attribute__((packed));

struct ELF32SectionHeader {
    uint32_t name;
    uint32_t type;
    uint32_t flags;
    uint32_t addr;
    uint32_t offset;
    uint32_t size;
    uint32_t link;
    uint32_t info;
    uint32_t addralign;
    uint32_t entsize;
} __attribute__((packed));

static inline bool elf_has_magic(const uint8_t *file, uint32_t size) {
    return size >= 4 && file[0] == ELF_MAGIC0 && file[1] == ELF_MAGIC1 && file[2] == ELF_MAGIC2 &&
           file[3] == ELF_MAGIC3;
}

#endif
