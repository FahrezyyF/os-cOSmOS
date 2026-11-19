#include "header/process/exec.h"
#include "header/process/cex.h"
#include "header/process/elf.h"
#include "header/memory/paging.h"
#include "header/stdlib/string.h"

// CEX / ELF images must end below the stack region
#define USER_IMAGE_LIMIT (USER_STACK_TOP - PROCESS_STACK_SIZE)

/* ================================ REGIONS ========================================== */

static bool ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
    return a0 < b1 && b0 < a1;
}

static bool add_region(struct ProcessControlBlock *pcb, uint32_t start, uint32_t end, uint8_t flags, uint8_t kind) {
    if (end <= start)
        return true; // empty section: nothing to map
    if (pcb->memory.region_count >= PROCESS_REGION_MAX)
        return false;
    for (uint32_t i = 0; i < pcb->memory.region_count; i++) {
        const struct ProcessMemoryRegion *r = &pcb->memory.regions[i];
        if (ranges_overlap(start, end, r->start, r->end))
            return false;
    }
    struct ProcessMemoryRegion *r = &pcb->memory.regions[pcb->memory.region_count++];
    r->start = start;
    r->end   = end;
    r->flags = flags;
    r->kind  = kind;
    return true;
}

// A page is writable when any region touching it is writable (sections may share a page
// when a file uses the minimum 2 KiB alignment)
static bool page_is_writable(const struct ProcessControlBlock *pcb, uint32_t page) {
    for (uint32_t i = 0; i < pcb->memory.region_count; i++) {
        const struct ProcessMemoryRegion *r = &pcb->memory.regions[i];
        if ((r->flags & PROCESS_REGION_WRITE) && ranges_overlap(page, page + PAGE_FRAME_SIZE, r->start, r->end))
            return true;
    }
    return false;
}

// After loading: clear R/W on every page that only belongs to read-only regions
static void apply_protection(struct ProcessControlBlock *pcb) {
    struct PageDirectory *dir = pcb->context.page_directory_virtual_addr;
    for (uint32_t i = 0; i < pcb->memory.region_count; i++) {
        const struct ProcessMemoryRegion *r = &pcb->memory.regions[i];
        if (r->flags & PROCESS_REGION_WRITE)
            continue;
        for (uint32_t page = r->start & PTE_ADDRESS_MASK; page < r->end; page += PAGE_FRAME_SIZE) {
            if (page_is_writable(pcb, page))
                continue;
            uint32_t *pte = paging_get_pte(dir, page, false);
            if (pte == NULL || !(*pte & (PTE_PRESENT | PTE_SWAPPED)))
                continue;
            *pte &= ~PTE_WRITABLE; // a swapped-out page keeps it too (swap_in restores the flags)
            if (*pte & PTE_PRESENT)
                flush_single_tlb((void*) (uintptr_t) page);
        }
    }
}

static void copy_to_user(uint32_t vaddr, const uint8_t *src, uint32_t len) {
    // CR3 is the new process: pages are allocated by the page fault handler as we go
    memcpy((void*) (uintptr_t) vaddr, src, len);
}

static bool is_power_of_two(uint32_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

/* ================================ FLAT BINARY ====================================== */

static int32_t load_flat(struct ProcessControlBlock *pcb, const uint8_t *file, uint32_t size, uint32_t load) {
    if (load >= USER_STACK_TOP || size > USER_STACK_TOP - load || size > USER_PROGRAM_MAX_SIZE)
        return PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
    // Book chapter 3 layout: image, bss and stack share [load, 4 MiB)
    if (!add_region(pcb, load & PTE_ADDRESS_MASK, USER_STACK_TOP,
                    PROCESS_REGION_READ | PROCESS_REGION_WRITE | PROCESS_REGION_EXEC, PROCESS_REGION_IMAGE))
        return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
    copy_to_user(load, file, size);
    pcb->metadata.format = PROCESS_FORMAT_FLAT;
    pcb->memory.entry    = load;
    return PROCESS_CREATE_SUCCESS;
}

/* ================================ CEX ============================================== */

const char *cex_validate(const uint8_t *file, uint32_t size) {
    if (size < sizeof(struct CEXHeader) || !cex_has_magic(file, size))
        return "bad magic";
    struct CEXHeader h;
    memcpy(&h, file, sizeof(h));
    if (h.version != CEX_VERSION)
        return "unsupported version";
    if (h.header_size != sizeof(struct CEXHeader) || h.section_entry_size != sizeof(struct CEXSection))
        return "bad header size";
    if (h.file_size != size)
        return "file size mismatch (truncated?)";
    if (h.checksum != cex_checksum(file, size))
        return "checksum mismatch";
    if (h.section_count == 0 || h.section_count > CEX_MAX_SECTIONS)
        return "bad section count";
    uint32_t table_end = h.section_table_offset + h.section_count * sizeof(struct CEXSection);
    if (h.section_table_offset < sizeof(struct CEXHeader) || table_end > size || table_end < h.section_table_offset)
        return "section table outside the file";
    if (h.image_base >= h.image_end || h.image_end > USER_IMAGE_LIMIT)
        return "image does not fit below the stack";

    struct CEXSection sec[CEX_MAX_SECTIONS];
    memcpy(sec, file + h.section_table_offset, h.section_count * sizeof(struct CEXSection));
    bool entry_ok = false;
    for (uint32_t i = 0; i < h.section_count; i++) {
        const struct CEXSection *s = &sec[i];
        uint32_t end = s->vaddr + s->mem_size;
        if (!is_power_of_two(s->alignment) || s->alignment < CEX_MIN_ALIGNMENT)
            return "section alignment below 2 KiB";
        if (s->vaddr % s->alignment != 0)
            return "section not aligned";
        if (end < s->vaddr || s->vaddr < h.image_base || end > h.image_end)
            return "section outside the image";
        if (s->file_size > s->mem_size)
            return "section file size > memory size";
        if (s->file_size > 0 && (s->file_offset < table_end || s->file_offset + s->file_size > size ||
                                 s->file_offset + s->file_size < s->file_offset))
            return "section data outside the file";
        switch (s->type) {
            case CEX_SECTION_TEXT:
                if (s->flags != (CEX_FLAG_READ | CEX_FLAG_EXEC)) return ".text must be R+X";
                break;
            case CEX_SECTION_RODATA:
                if (s->flags != CEX_FLAG_READ) return ".rodata must be R";
                break;
            case CEX_SECTION_DATA:
                if (s->flags != (CEX_FLAG_READ | CEX_FLAG_WRITE)) return ".data must be R+W";
                break;
            case CEX_SECTION_BSS:
                if (s->flags != (CEX_FLAG_READ | CEX_FLAG_WRITE)) return ".bss must be R+W";
                if (s->file_size != 0) return ".bss has file data";
                break;
            default:
                return "unknown section type";
        }
        for (uint32_t j = 0; j < i; j++)
            if (s->mem_size && sec[j].mem_size && ranges_overlap(s->vaddr, end, sec[j].vaddr, sec[j].vaddr + sec[j].mem_size))
                return "sections overlap";
        if ((s->flags & CEX_FLAG_EXEC) && h.entry >= s->vaddr && h.entry < end)
            entry_ok = true;
    }
    if (!entry_ok)
        return "entry point outside .text";
    return NULL;
}

static uint8_t cex_kind(uint16_t type) {
    switch (type) {
        case CEX_SECTION_TEXT:   return PROCESS_REGION_TEXT;
        case CEX_SECTION_RODATA: return PROCESS_REGION_RODATA;
        case CEX_SECTION_DATA:   return PROCESS_REGION_DATA;
        default:                 return PROCESS_REGION_BSS;
    }
}

static int32_t load_cex(struct ProcessControlBlock *pcb, const uint8_t *file, uint32_t size) {
    if (cex_validate(file, size) != NULL)
        return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
    struct CEXHeader h;
    memcpy(&h, file, sizeof(h));
    for (uint32_t i = 0; i < h.section_count; i++) {
        struct CEXSection s;
        memcpy(&s, file + h.section_table_offset + i * sizeof(s), sizeof(s));
        uint8_t flags = PROCESS_REGION_READ;
        if (s.flags & CEX_FLAG_WRITE) flags |= PROCESS_REGION_WRITE;
        if (s.flags & CEX_FLAG_EXEC)  flags |= PROCESS_REGION_EXEC;
        if (!add_region(pcb, s.vaddr, s.vaddr + s.mem_size, flags, cex_kind(s.type)))
            return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
    }
    if (!add_region(pcb, USER_STACK_TOP - PROCESS_STACK_SIZE, USER_STACK_TOP,
                    PROCESS_REGION_READ | PROCESS_REGION_WRITE, PROCESS_REGION_STACK))
        return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
    // Contents: file bytes are copied, the rest of each section (all of .bss) is demand-zero
    for (uint32_t i = 0; i < h.section_count; i++) {
        struct CEXSection s;
        memcpy(&s, file + h.section_table_offset + i * sizeof(s), sizeof(s));
        if (s.file_size > 0)
            copy_to_user(s.vaddr, file + s.file_offset, s.file_size);
    }
    pcb->metadata.format = PROCESS_FORMAT_CEX;
    pcb->memory.entry    = h.entry;
    return PROCESS_CREATE_SUCCESS;
}

/* ================================ ELF32 ============================================ */

static int32_t load_elf(struct ProcessControlBlock *pcb, const uint8_t *file, uint32_t size) {
    struct ELF32Header h;
    if (size < sizeof(h))
        return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
    memcpy(&h, file, sizeof(h));
    if (h.ident[4] != ELF_CLASS_32 || h.ident[5] != ELF_DATA_LSB || h.type != ELF_TYPE_EXEC ||
        h.machine != ELF_MACHINE_386 || h.phentsize != sizeof(struct ELF32ProgramHeader) || h.phnum == 0 ||
        h.phnum > 16 || h.phoff + (uint32_t) h.phnum * sizeof(struct ELF32ProgramHeader) > size)
        return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;

    bool entry_ok = false;
    for (uint32_t pass = 0; pass < 2; pass++) { // pass 0: validate + regions, pass 1: copy
        for (uint32_t i = 0; i < h.phnum; i++) {
            struct ELF32ProgramHeader ph;
            memcpy(&ph, file + h.phoff + i * sizeof(ph), sizeof(ph));
            if (ph.type != ELF_PT_LOAD || ph.memsz == 0)
                continue;
            uint32_t end = ph.vaddr + ph.memsz;
            if (pass == 1) {
                if (ph.filesz > 0)
                    copy_to_user(ph.vaddr, file + ph.offset, ph.filesz);
                continue;
            }
            if (ph.filesz > ph.memsz || end < ph.vaddr || end > USER_IMAGE_LIMIT ||
                ph.offset + ph.filesz > size || ph.offset + ph.filesz < ph.offset)
                return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
            uint8_t flags = 0, kind;
            if (ph.flags & ELF_PF_R) flags |= PROCESS_REGION_READ;
            if (ph.flags & ELF_PF_W) flags |= PROCESS_REGION_WRITE;
            if (ph.flags & ELF_PF_X) flags |= PROCESS_REGION_EXEC;
            kind = (flags & PROCESS_REGION_EXEC) ? PROCESS_REGION_TEXT
                 : (flags & PROCESS_REGION_WRITE) ? PROCESS_REGION_DATA : PROCESS_REGION_RODATA;
            if (!add_region(pcb, ph.vaddr, end, flags | PROCESS_REGION_READ, kind))
                return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
            if ((flags & PROCESS_REGION_EXEC) && h.entry >= ph.vaddr && h.entry < end)
                entry_ok = true;
        }
        if (pass == 0) {
            if (!entry_ok)
                return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
            if (!add_region(pcb, USER_STACK_TOP - PROCESS_STACK_SIZE, USER_STACK_TOP,
                            PROCESS_REGION_READ | PROCESS_REGION_WRITE, PROCESS_REGION_STACK))
                return PROCESS_CREATE_FAIL_INVALID_EXECUTABLE;
        }
    }
    pcb->metadata.format = PROCESS_FORMAT_ELF;
    pcb->memory.entry    = h.entry;
    return PROCESS_CREATE_SUCCESS;
}

/* ================================ DISPATCH ========================================= */

int32_t exec_load_image(struct ProcessControlBlock *pcb, const uint8_t *file, uint32_t size,
                        uint32_t flat_load_address) {
    int32_t code;
    if (cex_has_magic(file, size))
        code = load_cex(pcb, file, size);
    else if (elf_has_magic(file, size))
        code = load_elf(pcb, file, size);
    else
        code = load_flat(pcb, file, size, flat_load_address);
    if (code == PROCESS_CREATE_SUCCESS) {
        apply_protection(pcb);
        pcb->memory.image_size = size;
    }
    return code;
}
