/*
 * cOSmOS user programs (book 4.3.4 / 4.3.6 linker, extended).
 *
 * The output is an ELF32 executable: /bin commands are installed as ELF, the shell and the
 * applications are converted to CEX by src/external/elf2cex.c (bonus: simple executable
 * file format). Every section starts on its own 4 KiB page (a multiple of the 2 KiB
 * minimum of the CEX format) so the loader can map .text / .rodata read-only and
 * .data / .bss writable. Page 0 is left out: a NULL pointer dereference faults.
 * The page alignment is applied to the ADDRESSES only (`. = ALIGN(4K)`), not to the
 * sections, so with `ld -n` the ELF files in /bin carry no 4 KiB padding between sections.
 */
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);  /* R + X */
    rodata PT_LOAD FLAGS(4);  /* R     */
    data   PT_LOAD FLAGS(6);  /* R + W (.data and .bss) */
}

SECTIONS {
    . = 0x1000;

    . = ALIGN(4K);
    .text : {
        *crt0.s.o(.text)          /* the entry point first */
        *(.text .text.*)
    } :text

    . = ALIGN(4K);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4K);
    .data : {
        *(.data .data.*)
    } :data

    . = ALIGN(4K);
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    _linker_user_program_end = .;
    /* The image must end below the 1 MiB stack that sits under 0x400000 */
    ASSERT ((_linker_user_program_end <= 3 * 1024 * 1024), "Error: user program does not fit below the stack (3 MiB)")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
