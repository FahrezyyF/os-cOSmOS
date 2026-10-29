global loader                        ; the entry symbol for ELF
global load_gdt                      ; load GDT table
global set_tss_register              ; set tss register to GDT entry
global kernel_execute_user_program   ; execute initial user program from kernel
global _multiboot_magic              ; eax handed over by GRUB (0x2BADB002)
global _multiboot_info_address       ; ebx handed over by GRUB: physical address of the multiboot info
extern kernel_setup                  ; kernel C entrypoint
extern _paging_kernel_page_directory ; kernel page directory
extern _paging_kernel_page_table     ; kernel page table

KERNEL_VIRTUAL_BASE equ 0xC0000000    ; kernel virtual memory
KERNEL_STACK_SIZE   equ 2097152       ; size of stack in bytes (2 MiB, book 2.6: FS CRUD needs a bigger stack)
MAGIC_NUMBER        equ 0x1BADB002    ; define the magic number constant
FLAGS               equ 0x0           ; multiboot flags
CHECKSUM            equ -MAGIC_NUMBER ; calculate the checksum (magic number + checksum + flags == 0)


section .bss
align 4                    ; align at 4 bytes
kernel_stack:              ; label points to beginning of memory
    resb KERNEL_STACK_SIZE ; reserve stack for the kernel


section .data
align 4
_multiboot_magic:        dd 0
_multiboot_info_address: dd 0


section .multiboot  ; GRUB multiboot header
align 4             ; the code must be 4 byte aligned
    dd MAGIC_NUMBER ; write the magic number to the machine code,
    dd FLAGS        ; the flags,
    dd CHECKSUM     ; and the checksum


; start of the text (code) section
section .setup.text
loader equ (loader_entrypoint - KERNEL_VIRTUAL_BASE)
loader_entrypoint:         ; the loader label (defined as entry point in linker script)
; cOSmOS addition: keep what GRUB passed in eax / ebx (RAM size, boot command line `fs=...`).
; Paging is still off, so the variables are written through their physical addresses.
    mov [_multiboot_magic - KERNEL_VIRTUAL_BASE], eax
    mov [_multiboot_info_address - KERNEL_VIRTUAL_BASE], ebx

; Build the initial 4 KiB identity/high-half mappings before paging is on.
    mov edi, _paging_kernel_page_table - KERNEL_VIRTUAL_BASE
    xor eax, eax
    mov ecx, 1024
.fill_kernel_page_table:
    mov edx, eax
    or  edx, 0x3
    mov [edi], edx
    add edi, 4
    add eax, 0x1000
    loop .fill_kernel_page_table

    mov eax, _paging_kernel_page_table - KERNEL_VIRTUAL_BASE
    or  eax, 0x3
    mov [_paging_kernel_page_directory - KERNEL_VIRTUAL_BASE], eax
    mov [_paging_kernel_page_directory - KERNEL_VIRTUAL_BASE + 0xC00], eax

    ; Set CR3 (CPU page register)
    mov eax, _paging_kernel_page_directory - KERNEL_VIRTUAL_BASE
    mov cr3, eax

    ; Disable 4 MiB pages; all mappings use 4 KiB page tables.
    mov eax, cr4
    and eax, 0xFFFFFFEF
    mov cr4, eax

    ; Enable paging
    mov eax, cr0
    or  eax, 0x80000000    ; PG flag
    mov cr0, eax

    ; Jump into higher half first, cannot use C because call stack is still not working
    lea eax, [loader_virtual]
    jmp eax

loader_virtual:
    mov dword [_paging_kernel_page_directory], 0
    invlpg [0]                                ; Delete identity mapping and invalidate TLB cache for first page
    mov esp, kernel_stack + KERNEL_STACK_SIZE ; Setup stack register to proper location
    call kernel_setup
.loop:
    jmp .loop                                 ; loop forever


section .text
; More details: https://en.wikibooks.org/wiki/X86_Assembly/Protected_Mode
load_gdt:
    cli
    mov  eax, [esp+4]
    lgdt [eax] ; Load GDT from GDTDescriptor, eax at this line will point GDTR location

    ; Set bit-0 (Protection Enable bit-flag) in Control Register 0 (CR0)
    ; This is optional, as usually GRUB already start with protected mode flag enabled
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax

    ; Far jump to update cs register
    ; Warning: Invalid GDT will raise exception in any instruction below
    jmp 0x8:flush_cs
flush_cs:
    ; Update all segment register
    mov ax, 10h
    mov ss, ax
    mov ds, ax
    mov es, ax
    ret


set_tss_register:
    mov ax, 0x28 | 0 ; GDT TSS Selector, ring 0
    ltr ax
    ret


kernel_execute_user_program:
    mov  eax, 0x20 | 0x3
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    ; Using iret (return instruction for interrupt) technique for privilege change
    ; Stack values will be loaded into these register:
    ; [esp] -> eip, [esp+4] -> cs, [esp+8] -> eflags, [] -> user esp, [] -> user ss
    mov  ecx, [esp+4] ; Save first (before pushing anything to stack) for last push
    push eax ; Stack segment selector (GDT_USER_DATA_SELECTOR), user privilege
    mov  eax, ecx
    add  eax, 0x400000 - 4
    push eax ; User space stack pointer (esp), move it into last 4 MiB
    pushf    ; eflags register state, when jump inside user program
    ; cOSmOS: the shell is also (re)started from inside interrupt handlers (exit, crash), where
    ; IF = 0. Force IF = 1 so the user program always runs with interrupts enabled.
    or   dword [esp], 0x200
    mov  eax, 0x18 | 0x3
    push eax ; Code segment selector (GDT_USER_CODE_SELECTOR), user privilege
    mov  eax, ecx
    push eax ; eip register to jump back

    iret
