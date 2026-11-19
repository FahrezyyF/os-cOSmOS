#ifndef _CEX_H
#define _CEX_H

#include <stdint.h>
#include <stdbool.h>

/**
 * CEX - cOSmOS EXecutable, version 1 (bonus: Simple Executable File Format).
 *
 * File layout (all fields little endian):
 *
 *   +---------------------------+  offset 0
 *   | struct CEXHeader (64 B)   |  magic "CEX\x7f", entry point, image range, checksum
 *   +---------------------------+  header.section_table_offset
 *   | struct CEXSection [n]     |  32 B each: name, type, R/W/X flags, vaddr, sizes
 *   +---------------------------+
 *   | section contents          |  .text, .rodata, .data (each 16-byte aligned in the file);
 *   |                           |  .bss has no bytes in the file (file_size = 0)
 *   +---------------------------+
 *
 * Rules checked by the kernel loader (process/exec.c, cex_validate()):
 *   - every section's vaddr is a multiple of its alignment, alignment is a power of two
 *     and at least CEX_MIN_ALIGNMENT (2 KiB). cOSmOS links with 4 KiB alignment, so
 *     every section starts on its own page and gets its own page protection;
 *   - sections lie inside [image_base, image_end), do not overlap, and stay in user space;
 *   - file_size <= mem_size, file data inside the file; .bss has file_size 0;
 *   - the entry point is inside an executable section;
 *   - checksum = CRC-32 (IEEE) of the whole file with the checksum field taken as 0.
 *
 * Loading: .text (R+X) and .rodata (R) are mapped read-only, .data / .bss (R+W) writable,
 * the part of a section past file_size (all of .bss) is zero-filled.
 *
 * Programs are linked as ELF32 with src/user-linker.ld and converted by the host tool
 * src/external/elf2cex.c, so user C code never deals with any of this.
 */

#define CEX_MAGIC0          'C'
#define CEX_MAGIC1          'E'
#define CEX_MAGIC2          'X'
#define CEX_MAGIC3          0x7F
#define CEX_VERSION         1
#define CEX_MIN_ALIGNMENT   2048u
#define CEX_MAX_SECTIONS    16
#define CEX_FILE_ALIGNMENT  16u

#define CEX_SECTION_TEXT    1
#define CEX_SECTION_RODATA  2
#define CEX_SECTION_DATA    3
#define CEX_SECTION_BSS     4

#define CEX_FLAG_READ       0x1
#define CEX_FLAG_WRITE      0x2
#define CEX_FLAG_EXEC       0x4

#define CEX_HEADER_FLAG_NONE 0

struct CEXHeader {
    uint8_t  magic[4];             // 'C' 'E' 'X' 0x7F
    uint16_t version;              // CEX_VERSION
    uint16_t header_size;          // sizeof(struct CEXHeader)
    uint32_t flags;                // CEX_HEADER_FLAG_*
    uint32_t entry;                // virtual address of _start
    uint32_t image_base;           // lowest section address
    uint32_t image_end;            // end of the highest section (end of .bss)
    uint32_t stack_size;           // stack the program asks for (0 = default)
    uint32_t section_count;
    uint32_t section_table_offset; // file offset of the section table
    uint32_t section_entry_size;   // sizeof(struct CEXSection)
    uint32_t file_size;            // total size of the file
    uint32_t checksum;             // CRC-32, computed with this field = 0
    char     name[16];             // program name (informational)
} __attribute__((packed));

struct CEXSection {
    char     name[8];      // ".text", ".rodata", ".data", ".bss"
    uint16_t type;         // CEX_SECTION_*
    uint16_t flags;        // CEX_FLAG_*
    uint32_t vaddr;
    uint32_t mem_size;
    uint32_t file_offset;  // 0 when file_size == 0
    uint32_t file_size;
    uint32_t alignment;    // in memory, power of two >= CEX_MIN_ALIGNMENT
} __attribute__((packed));

_Static_assert(sizeof(struct CEXHeader) == 64, "CEX header is 64 bytes");
_Static_assert(sizeof(struct CEXSection) == 32, "CEX section entry is 32 bytes");

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), bitwise: shared by kernel and tools
static inline uint32_t cex_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Checksum of a whole CEX file image, treating header.checksum as zero
static inline uint32_t cex_checksum(const uint8_t *file, uint32_t size) {
    const uint32_t field = 44; // offsetof(struct CEXHeader, checksum)
    uint8_t zero[4] = {0, 0, 0, 0};
    uint32_t crc = cex_crc32_update(0, file, field);
    crc = cex_crc32_update(crc, zero, 4);
    return cex_crc32_update(crc, file + field + 4, size - field - 4);
}

static inline bool cex_has_magic(const uint8_t *file, uint32_t size) {
    return size >= 4 && file[0] == CEX_MAGIC0 && file[1] == CEX_MAGIC1 && file[2] == CEX_MAGIC2 &&
           file[3] == CEX_MAGIC3;
}

static inline const char *cex_section_type_name(uint16_t type) {
    switch (type) {
        case CEX_SECTION_TEXT:   return "TEXT";
        case CEX_SECTION_RODATA: return "RODATA";
        case CEX_SECTION_DATA:   return "DATA";
        case CEX_SECTION_BSS:    return "BSS";
        default:                 return "?";
    }
}

#endif
