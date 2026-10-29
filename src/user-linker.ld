ENTRY(_start)

SECTIONS {
    . = 0x00000000; /* Assuming OS will load this program at virtual address */

    .text ALIGN(4): {
        *crt0.s.o(.text)  /* Put the entrypoint in the front of executable */
        *(.text)
    }

    /* .rodata before .data / .bss so the zero-filled .bss is not stored in the flat binary
       (the kernel maps fresh zero pages on demand) */
    .rodata ALIGN(4): {
        *(.rodata*)
    }

    .data ALIGN(4): {
        *(.data)
    }

    .bss ALIGN(4): { /* 9 May Edit: Fixes for static storage variables */
        *(COMMON)
        *(.bss)
    }
    _linker_user_program_end = .;
    ASSERT ((_linker_user_program_end <= 1 * 1024 * 1024), "Error: User program linking result is more than 1 MiB")

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
