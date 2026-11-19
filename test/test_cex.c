// Host test for the CEX executable format: the shell converted by elf2cex must pass the
// kernel's validator (src/process/exec.c, cex_validate), and every kind of corruption must
// be rejected.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "header/process/cex.h"
#include "header/process/exec.h"

// exec.c's loader parts reference the paging layer; the validator does not need it
uint32_t *paging_get_pte(struct PageDirectory *d, uint32_t v, bool c) { (void) d; (void) v; (void) c; return NULL; }
void flush_single_tlb(void *v) { (void) v; }

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint8_t *load(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (uint32_t) ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*size);
    if (fread(d, 1, *size, f) != *size) { fclose(f); free(d); return NULL; }
    fclose(f);
    return d;
}

static void reseal(uint8_t *f, uint32_t size) {
    struct CEXHeader h;
    memcpy(&h, f, sizeof(h));
    h.checksum = cex_checksum(f, size);
    memcpy(f, &h, sizeof(h));
}

static const char *mutate(const uint8_t *orig, uint32_t size, void (*fn)(uint8_t *, uint32_t), bool fix_crc) {
    static uint8_t *copy;
    free(copy);
    copy = malloc(size);
    memcpy(copy, orig, size);
    fn(copy, size);
    if (fix_crc)
        reseal(copy, size);
    return cex_validate(copy, size);
}

static struct CEXSection *section(uint8_t *f, int i) {
    return (struct CEXSection *) (f + sizeof(struct CEXHeader) + i * sizeof(struct CEXSection));
}
static void flip_text_byte(uint8_t *f, uint32_t s) { (void) s; f[section(f, 0)->file_offset + 5] ^= 0xFF; }
static void bad_magic(uint8_t *f, uint32_t s) { (void) s; f[0] = 'X'; }
static void misalign(uint8_t *f, uint32_t s) { (void) s; section(f, 1)->vaddr += 1024; }
static void small_align(uint8_t *f, uint32_t s) { (void) s; section(f, 0)->alignment = 1024; }
static void writable_text(uint8_t *f, uint32_t s) { (void) s; section(f, 0)->flags |= CEX_FLAG_WRITE; }
static void entry_outside(uint8_t *f, uint32_t s) { (void) s; ((struct CEXHeader *) f)->entry = 0x200000; }
static void too_big(uint8_t *f, uint32_t s) { (void) s; ((struct CEXHeader *) f)->image_end = 0x00380000; }
static void overlap(uint8_t *f, uint32_t s) { (void) s; section(f, 1)->vaddr = section(f, 0)->vaddr; }
static void data_outside(uint8_t *f, uint32_t s) { section(f, 0)->file_size = s; }
static void bss_with_data(uint8_t *f, uint32_t s) {
    (void) s;
    struct CEXHeader *h = (struct CEXHeader *) f;
    for (uint32_t i = 0; i < h->section_count; i++)
        if (section(f, (int) i)->type == CEX_SECTION_BSS)
            section(f, (int) i)->file_size = 4;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "bin/shell";
    uint32_t size;
    uint8_t *f = load(path, &size);
    printf("[cex] validator on %s\n", path);
    if (f == NULL) {
        printf("cannot read %s (build it with `make user-shell`)\n", path);
        return 1;
    }
    CHECK(cex_has_magic(f, size));
    const char *why = cex_validate(f, size);
    CHECK(why == NULL);
    if (why) printf("  valid file rejected: %s\n", why);
    struct CEXHeader h;
    memcpy(&h, f, sizeof(h));
    CHECK(h.section_count >= 3 && h.entry == 0x1000 && h.image_base == 0x1000);
    for (uint32_t i = 0; i < h.section_count; i++) {
        struct CEXSection *s = section(f, (int) i);
        CHECK(s->alignment >= CEX_MIN_ALIGNMENT && s->vaddr % s->alignment == 0);
    }
    CHECK(mutate(f, size, flip_text_byte, false) != NULL);            // checksum catches it
    CHECK(mutate(f, size, bad_magic, true) != NULL);
    CHECK(mutate(f, size, misalign, true) != NULL);
    CHECK(mutate(f, size, small_align, true) != NULL);
    CHECK(mutate(f, size, writable_text, true) != NULL);
    CHECK(mutate(f, size, entry_outside, true) != NULL);
    CHECK(mutate(f, size, too_big, true) != NULL);
    CHECK(mutate(f, size, overlap, true) != NULL);
    CHECK(mutate(f, size, data_outside, true) != NULL);
    CHECK(mutate(f, size, bss_with_data, true) != NULL);
    CHECK(cex_validate(f, size - 1) != NULL);                         // truncated
    // CRC-32 known answer ("123456789" -> 0xCBF43926)
    CHECK(cex_crc32_update(0, (const uint8_t *) "123456789", 9) == 0xCBF43926u);
    printf("cex: %d checks, %d failures\n", checks, failures);
    free(f);
    return failures != 0;
}
