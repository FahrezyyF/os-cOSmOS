; C runtime start-up for cOSmOS user programs (bonus: basic C runtime, argc / argv).
;
; The kernel (process/process.c, setup_user_stack) starts every process with:
;   [esp]                 argc
;   [esp + 4]             argv[0] ... argv[argc - 1], NULL
;   [esp + 4*(argc+2)]    envp[0] ... NULL
; so a program is a plain  int main(int argc, char *argv[], char *envp[])
; and its return value goes to the kernel as the exit status.

global _start
extern main
extern environ

section .text
_start:
    xor  ebp, ebp                ; outermost frame for backtraces
    mov  eax, [esp]              ; argc
    lea  ebx, [esp + 4]          ; argv
    lea  ecx, [ebx + eax*4 + 4]  ; envp = argv + argc + 1
    mov  [environ], ecx
    push ecx                     ; main(argc, argv, envp)
    push ebx
    push eax
    call main
    add  esp, 12

    ; Exit program (terminate process): SYS_TERMINATE = 32, ebx = main's return value
    mov  ebx, eax
    mov  eax, 32
    int  0x30

    ; Is it possible to reach here? No: the kernel never returns from SYS_TERMINATE.
.hang:
    jmp  .hang
