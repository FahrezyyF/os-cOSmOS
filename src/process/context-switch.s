; Context switch (book 4.2.2.1): load a process context and jump into it with iret.
;
; void process_context_switch(struct Context ctx)  -- ctx is passed BY VALUE on the stack
;
; struct Context layout (header/process/process.h):
;   +0  edi   +4  esi                    (cpu.index)
;   +8  ebp   +12 esp                    (cpu.stack, esp = user stack pointer)
;   +16 ebx   +20 edx   +24 ecx  +28 eax (cpu.general)
;   +32 gs    +36 fs    +40 es   +44 ds  (cpu.segment)
;   +48 eip   +52 eflags                 +56 page directory (CR3 is loaded by the C caller)
;
; Every saved context is a ring 3 context, so the iret frame always carries the user stack:
;   [esp] eip, [esp+4] cs, [esp+8] eflags, [esp+12] user esp, [esp+16] user ss

global process_context_switch

CTX_EDI     equ 0
CTX_ESI     equ 4
CTX_EBP     equ 8
CTX_ESP     equ 12
CTX_EBX     equ 16
CTX_EDX     equ 20
CTX_ECX     equ 24
CTX_EAX     equ 28
CTX_GS      equ 32
CTX_FS      equ 36
CTX_ES      equ 40
CTX_DS      equ 44
CTX_EIP     equ 48
CTX_EFLAGS  equ 52

USER_CODE_SELECTOR equ 0x18 | 0x3     ; GDT_USER_CODE_SEGMENT_SELECTOR, RPL 3
USER_DATA_SELECTOR equ 0x20 | 0x3     ; GDT_USER_DATA_SEGMENT_SELECTOR, RPL 3

section .text
process_context_switch:
    ; 1. Keep the base address of the ctx argument (first argument, above the return address)
    lea  ecx, [esp + 4]

    ; 2. iret stack: ss, esp, eflags, cs, eip (pushed in reverse order of the pops)
    push dword USER_DATA_SELECTOR
    push dword [ecx + CTX_ESP]
    mov  eax, [ecx + CTX_EFLAGS]
    or   eax, 0x200                ; a user context always runs with interrupts enabled
    and  eax, ~0x3000              ; and IOPL 0
    push eax
    push dword USER_CODE_SELECTOR
    push dword [ecx + CTX_EIP]

    ; 3. Load all registers from ctx: segments first (flat user data segment, DPL 3, still
    ;    usable for kernel reads of the kernel stack), then the general purpose registers
    mov  eax, [ecx + CTX_GS]
    mov  gs, ax
    mov  eax, [ecx + CTX_FS]
    mov  fs, ax
    mov  eax, [ecx + CTX_ES]
    mov  es, ax
    mov  eax, [ecx + CTX_DS]
    mov  ds, ax

    mov  edi, [ecx + CTX_EDI]
    mov  esi, [ecx + CTX_ESI]
    mov  ebp, [ecx + CTX_EBP]
    mov  ebx, [ecx + CTX_EBX]
    mov  edx, [ecx + CTX_EDX]
    mov  eax, [ecx + CTX_EAX]
    ; 4. Cleanup: ecx was the base pointer, load it last
    mov  ecx, [ecx + CTX_ECX]

    ; 5. Jump into the process
    iret
