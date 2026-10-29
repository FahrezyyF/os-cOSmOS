global _start
extern main

section .text
_start:
    call main
    ; main() returned: hand its return value to the kernel (SYS_EXIT = 26), which starts a
    ; fresh shell. The book's crt0 spins with `jmp $` instead; that is kept as a fallback.
    mov  ebx, eax
    mov  eax, 26
    int  0x30
    jmp  $
