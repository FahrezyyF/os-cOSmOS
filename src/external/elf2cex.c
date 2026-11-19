// External program: elf2cex, runs on the host (bonus: simple executable file format).
// Converts an ELF32 i386 executable linked with src/user-linker.ld into a CEX file
// (header/process/cex.h), the format the cOSmOS loader prefers.
//
// usage: elf2cex <input.elf> <output.cex> [program name]
//
// Each allocated ELF section (.text .rodata .data .bss) becomes one CEX section with its
// address, size, alignment and permissions; .bss keeps only its size. The checksum is a
// CRC-32 of the result, verified by the kernel before anything is loaded.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "header/process/cex.h"
#include "header/process/elf.h"

static uint8_t *read_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(n > 0 ? (size_t) n : 1);
    if (data == NULL || fread(data, 1, (size_t) n, f) != (size_t) n) {
        fclose(f);
        free(data);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t) n;
    return data;
}

static int fail(const char *what) {
    fprintf(stderr, "elf2cex: %s\n", what);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: elf2cex <input.elf> <output.cex> [program name]\n");
        return 2;
    }
    uint32_t size;
    uint8_t *elf = read_file(argv[1], &size);
    if (elf == NULL)
        return fail("cannot read the input file");
    struct ELF32Header eh;
    if (size < sizeof(eh) || !elf_has_magic(elf, size))
        return fail("not an ELF file");
    memcpy(&eh, elf, sizeof(eh));
    if (eh.ident[4] != ELF_CLASS_32 || eh.ident[5] != ELF_DATA_LSB || eh.type != ELF_TYPE_EXEC ||
        eh.machine != ELF_MACHINE_386)
        return fail("not an ELF32 i386 executable");
    if (eh.shentsize != sizeof(struct ELF32SectionHeader) || eh.shoff + (uint64_t) eh.shnum * eh.shentsize > size ||
        eh.shstrndx >= eh.shnum)
        return fail("bad section header table");

    struct ELF32SectionHeader *sh = calloc(eh.shnum, sizeof(*sh));
    memcpy(sh, elf + eh.shoff, (size_t) eh.shnum * sizeof(*sh));
    const char *strtab = (const char*) elf + sh[eh.shstrndx].offset;

    struct CEXSection sections[CEX_MAX_SECTIONS];
    const uint8_t *contents[CEX_MAX_SECTIONS];
    uint32_t count = 0, image_base = 0xFFFFFFFFu, image_end = 0;
    for (uint32_t i = 0; i < eh.shnum; i++) {
        if (!(sh[i].flags & ELF_SHF_ALLOC) || sh[i].size == 0)
            continue;
        const char *name = strtab + sh[i].name;
        struct CEXSection s;
        memset(&s, 0, sizeof(s));
        strncpy(s.name, name, sizeof(s.name));
        if (strcmp(name, ".text") == 0) {
            s.type = CEX_SECTION_TEXT;
            s.flags = CEX_FLAG_READ | CEX_FLAG_EXEC;
        } else if (strcmp(name, ".rodata") == 0) {
            s.type = CEX_SECTION_RODATA;
            s.flags = CEX_FLAG_READ;
        } else if (strcmp(name, ".data") == 0) {
            s.type = CEX_SECTION_DATA;
            s.flags = CEX_FLAG_READ | CEX_FLAG_WRITE;
        } else if (strcmp(name, ".bss") == 0) {
            s.type = CEX_SECTION_BSS;
            s.flags = CEX_FLAG_READ | CEX_FLAG_WRITE;
        } else {
            fprintf(stderr, "elf2cex: unexpected allocated section %s (check the linker script)\n", name);
            return 1;
        }
        if (count == CEX_MAX_SECTIONS)
            return fail("too many sections");
        s.vaddr     = sh[i].addr;
        s.mem_size  = sh[i].size;
        s.alignment = (sh[i].addr % 4096 == 0) ? 4096 : 2048;
        if (sh[i].addr % CEX_MIN_ALIGNMENT != 0)
            return fail("section not aligned to 2 KiB (check the linker script)");
        if (sh[i].type == ELF_SHT_NOBITS) {
            contents[count] = NULL;
        } else {
            if (sh[i].offset + (uint64_t) sh[i].size > size)
                return fail("section data outside the ELF file");
            s.file_size     = sh[i].size;
            contents[count] = elf + sh[i].offset;
        }
        if (s.vaddr < image_base)
            image_base = s.vaddr;
        if (s.vaddr + s.mem_size > image_end)
            image_end = s.vaddr + s.mem_size;
        sections[count++] = s;
    }
    if (count == 0)
        return fail("no allocated sections");

    // Layout: header, section table, section data (16-byte aligned)
    uint32_t offset = (uint32_t) (sizeof(struct CEXHeader) + count * sizeof(struct CEXSection));
    for (uint32_t i = 0; i < count; i++) {
        if (sections[i].file_size == 0)
            continue;
        offset = (offset + CEX_FILE_ALIGNMENT - 1) & ~(CEX_FILE_ALIGNMENT - 1);
        sections[i].file_offset = offset;
        offset += sections[i].file_size;
    }
    uint32_t out_size = offset;
    uint8_t *out = calloc(1, out_size);

    struct CEXHeader h;
    memset(&h, 0, sizeof(h));
    h.magic[0] = CEX_MAGIC0;
    h.magic[1] = CEX_MAGIC1;
    h.magic[2] = CEX_MAGIC2;
    h.magic[3] = CEX_MAGIC3;
    h.version              = CEX_VERSION;
    h.header_size          = sizeof(struct CEXHeader);
    h.flags                = CEX_HEADER_FLAG_NONE;
    h.entry                = eh.entry;
    h.image_base           = image_base;
    h.image_end            = image_end;
    h.stack_size           = 0;
    h.section_count        = count;
    h.section_table_offset = sizeof(struct CEXHeader);
    h.section_entry_size   = sizeof(struct CEXSection);
    h.file_size            = out_size;
    const char *pname = argc > 3 ? argv[3] : argv[2];
    const char *slash = strrchr(pname, '/');
    strncpy(h.name, slash ? slash + 1 : pname, sizeof(h.name));
    memcpy(out, &h, sizeof(h));
    memcpy(out + sizeof(h), sections, count * sizeof(struct CEXSection));
    for (uint32_t i = 0; i < count; i++)
        if (sections[i].file_size > 0)
            memcpy(out + sections[i].file_offset, contents[i], sections[i].file_size);
    h.checksum = cex_checksum(out, out_size);
    memcpy(out, &h, sizeof(h));

    FILE *f = fopen(argv[2], "wb");
    if (f == NULL || fwrite(out, 1, out_size, f) != out_size)
        return fail("cannot write the output file");
    fclose(f);
    printf("elf2cex: %s -> %s: %u bytes (ELF %u), entry 0x%x, %u sections, crc32 %08x\n", argv[1], argv[2],
           out_size, size, h.entry, count, h.checksum);
    free(out);
    free(sh);
    free(elf);
    return 0;
}
