#ifndef _PROCESS_H
#define _PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "header/cpu/interrupt.h"
#include "header/memory/paging.h"
#include "header/filesystem/ext2.h"

/**
 * Process (book 4.1). Every user program runs as a process with its own virtual address
 * space (page directory), its own saved CPU context and a Process Control Block in the
 * static _process_list. Process 1 ("init") is the shell.
 *
 * Virtual memory of a process (header/memory/paging.h has the constants):
 *   0x00000000            flat binaries are loaded here (book 3.3.5)
 *   0x00001000            CEX / ELF programs: .text .rodata (read only) .data .bss (RW);
 *                         page 0 stays unmapped so a NULL dereference faults
 *   ... 0x00300000        user stack (1 MiB, grows down) for CEX / ELF programs
 *   0x00400000            stack top (argc, argv, envp are placed here) and heap base
 *   0x00400000 - break    heap, grown with sbrk() (bonus: dynamic process size)
 *   0xC0000000 - ...      kernel (shared by every page directory, supervisor only)
 */

#define PROCESS_NAME_LENGTH_MAX          32
#define PROCESS_PAGE_FRAME_COUNT_MAX     8    // kit value, now the INITIAL capacity of the growable page list
#define PROCESS_COUNT_MAX                16
#define PROCESS_FD_MAX                   8    // file descriptors per process (0 stdin, 1 stdout, 2 stderr)
#define PROCESS_REGION_MAX               8    // memory regions (sections, stack, heap)
#define PROCESS_CMDLINE_MAX              64
#define PROCESS_CWD_MAX                  256
#define PROCESS_ARG_MAX                  32   // argv / envp entries
#define PROCESS_ARG_BYTES_MAX            4096 // argv + envp strings
#define PROCESS_EXECUTABLE_MAX_SIZE      0x00300000u // 3 MiB: the image must end below the stack
#define PROCESS_STACK_SIZE               0x00100000u // 1 MiB stack for CEX / ELF programs
#define PROCESS_INIT_PID                 1

#define CPU_EFLAGS_BASE_FLAG               0x2
#define CPU_EFLAGS_FLAG_CARRY              0x1
#define CPU_EFLAGS_FLAG_PARITY             0x4
#define CPU_EFLAGS_FLAG_AUX_CARRY          0x10
#define CPU_EFLAGS_FLAG_ZERO               0x40
#define CPU_EFLAGS_FLAG_SIGN               0x80
#define CPU_EFLAGS_FLAG_TRAP               0x100
#define CPU_EFLAGS_FLAG_INTERRUPT_ENABLE   0x200
#define CPU_EFLAGS_FLAG_DIRECTION          0x400
#define CPU_EFLAGS_FLAG_OVERFLOW           0x800
#define CPU_EFLAGS_FLAG_IO_PRIVILEGE       0x3000
#define CPU_EFLAGS_FLAG_NESTED_TASK        0x4000
#define CPU_EFLAGS_FLAG_MODE               0x8000
#define CPU_EFLAGS_FLAG_RESUME             0x10000
#define CPU_EFLAGS_FLAG_VIRTUAL_8086       0x20000
#define CPU_EFLAGS_FLAG_ALIGNMENT_CHECK    0x40000
#define CPU_EFLAGS_FLAG_VINTERRUPT_FLAG    0x80000
#define CPU_EFLAGS_FLAG_VINTERRUPT_PENDING 0x100000
#define CPU_EFLAGS_FLAG_CPUID_INSTRUCTION  0x200000
#define CPU_EFLAGS_FLAG_AES_SCHEDULE_LOAD  0x40000000
#define CPU_EFLAGS_FLAG_ALTER_INSTRUCTION  0x80000000

// Return code constant for process_create_user_process()
#define PROCESS_CREATE_SUCCESS                   0
#define PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED 1
#define PROCESS_CREATE_FAIL_INVALID_ENTRYPOINT   2
#define PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY    3
#define PROCESS_CREATE_FAIL_FS_READ_FAILURE      4
// cOSmOS additions
#define PROCESS_CREATE_FAIL_INVALID_EXECUTABLE   5  // corrupt CEX / ELF header, bad checksum, ...
#define PROCESS_CREATE_FAIL_INVALID_ARGUMENT     6  // argv / envp too large, bad descriptor
#define PROCESS_CREATE_FAIL_NOT_EXECUTABLE       7  // no CEX / ELF header while one was required

// Exit status reported to a waiting parent for abnormal ends
#define PROCESS_EXIT_KILLED      137  // kill / Ctrl+C (128 + SIGKILL, like a UNIX shell shows it)
#define PROCESS_EXIT_CRASHED     139  // CPU exception (128 + SIGSEGV)
#define PROCESS_EXIT_DETACHED    (-2) // the child moved itself to the background (SYS_DETACH)

/**
 * Contain information needed for task to be able to get interrupted and resumed later
 *
 * @param cpu                         All CPU register state (cpu.stack.esp = user stack pointer)
 * @param eip                         CPU instruction counter to resume execution
 * @param eflags                      Flag register to load before resuming the execution
 * @param page_directory_virtual_addr CPU register CR3, containing pointer to active page directory
 *
 * Offsets are used by context-switch.s: cpu 0..47, eip 48, eflags 52, page directory 56.
 * Every saved context belongs to ring 3 (the kernel is never preempted, see scheduler.h),
 * so cs / ss are always the user selectors.
 */
struct Context {
    struct CPURegister    cpu;
    uint32_t              eip;
    uint32_t              eflags;
    struct PageDirectory *page_directory_virtual_addr;
} __attribute__((packed));

/**
 * Process states (book 4.1.2.2). The reference uses three states; INACTIVE marks a free
 * slot of _process_list.
 */
typedef enum PROCESS_STATE {
    PROCESS_STATE_INACTIVE = 0, // PCB slot unused
    PROCESS_STATE_READY,        // runnable, waiting for the CPU
    PROCESS_STATE_RUNNING,      // currently owns the CPU (exactly one at a time)
    PROCESS_STATE_BLOCKED,      // waiting for an event, see PROCESS_WAIT
} PROCESS_STATE;

// Why a BLOCKED process waits
typedef enum PROCESS_WAIT {
    PROCESS_WAIT_NONE = 0,
    PROCESS_WAIT_SLEEP,      // until metadata.wake_time_ms
    PROCESS_WAIT_KEY,        // keyboard input while it owns the keyboard
    PROCESS_WAIT_CHILD,      // a child process ends (or detaches)
    PROCESS_WAIT_PIPE_READ,  // data / end of file on a pipe
    PROCESS_WAIT_PIPE_WRITE, // room in a pipe
    PROCESS_WAIT_WINDOW,     // an event for one of its windows (compositor)
    PROCESS_WAIT_NET,        // a UDP datagram
} PROCESS_WAIT;

// Executable formats understood by the loader (process/exec.c)
typedef enum PROCESS_FORMAT {
    PROCESS_FORMAT_FLAT = 0, // book: raw binary loaded at 0x0
    PROCESS_FORMAT_CEX,      // cOSmOS Executable (bonus: simple executable file format)
    PROCESS_FORMAT_ELF,      // ELF32 (book 4.3.6: separate shell commands)
} PROCESS_FORMAT;

// Memory region of a process (bonus: dynamic process size)
#define PROCESS_REGION_READ   0x1
#define PROCESS_REGION_WRITE  0x2
#define PROCESS_REGION_EXEC   0x4

typedef enum PROCESS_REGION_KIND {
    PROCESS_REGION_IMAGE = 0, // flat binary: image + bss + stack in one region
    PROCESS_REGION_TEXT,
    PROCESS_REGION_RODATA,
    PROCESS_REGION_DATA,
    PROCESS_REGION_BSS,
    PROCESS_REGION_STACK,
    PROCESS_REGION_OTHER,
} PROCESS_REGION_KIND;

struct ProcessMemoryRegion {
    uint32_t start;  // page aligned
    uint32_t end;    // exclusive
    uint8_t  flags;  // PROCESS_REGION_*
    uint8_t  kind;   // PROCESS_REGION_KIND
};

// File descriptors: the console or one end of a kernel pipe (process/pipe.c)
typedef enum PROCESS_FD_TYPE {
    PROCESS_FD_NONE = 0,
    PROCESS_FD_CONSOLE,
    PROCESS_FD_PIPE_READ,
    PROCESS_FD_PIPE_WRITE,
} PROCESS_FD_TYPE;

struct Pipe;

struct ProcessFile {
    uint8_t      type;  // PROCESS_FD_TYPE
    struct Pipe *pipe;
};

/**
 * Structure data containing information about a process
 *
 * @param metadata Process metadata, contain various information about process
 * @param context  Process context used for context saving & switching
 * @param memory   Memory used for the process
 * @param files    Open file descriptors (stdin / stdout redirection for pipelines)
 */
struct ProcessControlBlock {
    struct {
        uint32_t      pid;
        uint32_t      ppid;
        char          name[PROCESS_NAME_LENGTH_MAX];
        char          cmdline[PROCESS_CMDLINE_MAX];
        PROCESS_STATE state;
        PROCESS_WAIT  wait_reason;
        uint32_t      wait_arg;       // pid / pipe end / window id the process waits for
        uint32_t      wake_time_ms;   // PROCESS_WAIT_SLEEP
        bool          wait_done;      // PROCESS_WAIT_CHILD: the child has ended...
        int32_t       wait_status;    // ...with this status
        uint8_t       priority;       // 0 low, 1 normal, 2 high (time slice length)
        uint8_t       format;         // PROCESS_FORMAT
        bool          kill_pending;   // Ctrl+C / kill while the process was inside the kernel
        bool          timed_wait;     // a restartable call with a deadline (wake_time_ms) is in progress
        bool          detached;       // runs in the background, not owned by a waiting parent
        uint32_t      start_ms;
        uint32_t      cpu_ms;         // CPU time (1 tick = 1 ms)
        uint32_t      switches;       // how often it was given the CPU
        uint32_t      cwd_inode;
        char          cwd[PROCESS_CWD_MAX];
    } metadata;

    struct Context context;

    struct {
        // Every user page mapped for the process, grown with krealloc() (no fixed limit)
        void                     **virtual_addr_used;
        uint32_t                   page_frame_used_count;
        uint32_t                   page_list_capacity;
        uint32_t                   peak_pages;
        struct ProcessMemoryRegion regions[PROCESS_REGION_MAX];
        uint32_t                   region_count;
        uint32_t                   heap_base;
        uint32_t                   heap_break;
        uint32_t                   image_size;   // bytes of the executable file
        uint32_t                   entry;
    } memory;

    struct ProcessFile files[PROCESS_FD_MAX];

    // x87 FPU state (fnsave image), saved when the process leaves the CPU
    uint8_t fpu_state[108];
    bool    fpu_saved;
};

/**
 * Process manager state
 *
 * @param active_process_count Number of PCB slots in use
 * @param last_pid             Last PID handed out (PIDs are never reused until wrap-around)
 * @param foreground_pid       Process that owns the keyboard (shell, or the program it waits for)
 * @param init_pid             PID of the current init process (the shell)
 */
struct ProcessManagerState {
    uint32_t active_process_count;
    uint32_t last_pid;
    uint32_t foreground_pid;
    uint32_t init_pid;
    uint32_t created_total;
    uint32_t init_respawns;
};

extern struct ProcessControlBlock _process_list[PROCESS_COUNT_MAX];
extern struct ProcessManagerState process_manager_state;

/**
 * Everything needed to start a program (the book's process_create_user_process() builds
 * one from its EXT2DriverRequest). Strings may live in kernel or (already validated) user
 * memory of the CURRENT address space: they are copied before the new address space is used.
 */
struct ProcessCreateInfo {
    uint32_t     parent_inode;     // directory holding the executable
    const char  *name;             // executable file name
    uint8_t      name_len;
    uint32_t     load_address;     // flat binaries: request.buf (0x0)
    uint32_t     max_size;         // request.buffer_size
    uint32_t     argc;
    const char *const *argv;       // may be NULL: argv = { name }
    uint32_t     envc;
    const char *const *envp;
    uint32_t     ppid;             // 0 = created by the kernel
    const char  *cwd;              // NULL = "/"
    uint32_t     cwd_inode;
    int32_t      stdin_fd;         // descriptor of the CREATOR to use as stdin (-1 = console)
    int32_t      stdout_fd;        // same for stdout
    bool         foreground;       // take the keyboard
    uint8_t      priority;
    bool         require_header;   // reject flat binaries (PROCESS_CREATE_FAIL_NOT_EXECUTABLE)
};

/* --- Book interface --- */

/**
 * Get currently running process PCB pointer
 *
 * @return Will return NULL if there's no running process
 */
struct ProcessControlBlock* process_get_current_running_pcb_pointer(void);

/**
 * Create new user process and setup the virtual address space.
 * All available return code is defined with macro "PROCESS_CREATE_*"
 *
 * @note          This procedure assumes no reentrancy in ISR
 * @param request Appropriate read request for the executable
 * @return        Process creation return code
 */
int32_t process_create_user_process(struct EXT2DriverRequest request);

/**
 * Destroy process then release page directory and process control block
 *
 * @param pid Process ID to delete
 * @return    True if process destruction success
 */
bool process_destroy(uint32_t pid);

// Kit helpers
int32_t  process_list_get_inactive_index(void);
uint32_t process_generate_new_pid(void);

/* --- cOSmOS extensions --- */

/**
 * Create a process (see ProcessCreateInfo). The new process is READY, it runs when the
 * scheduler picks it.
 * @param pid_out Receives the PID (may be NULL)
 * @return PROCESS_CREATE_* code
 */
int32_t process_create(const struct ProcessCreateInfo *info, uint32_t *pid_out);

// Human readable text for a PROCESS_CREATE_* code
const char *process_create_error_name(int32_t code);

struct ProcessControlBlock *process_get_by_pid(uint32_t pid);
// PCB whose page directory is `page_dir` (also during loading, before it ever ran)
struct ProcessControlBlock *process_get_by_page_directory(struct PageDirectory *page_dir);
// PCB of the address space currently in CR3, NULL for the kernel page directory
struct ProcessControlBlock *process_get_by_current_address_space(void);

/**
 * End a process with an exit status: its parent is woken (if it waits), its children are
 * re-parented to init, every page, swap slot, pipe end, window and the page directory are
 * released. Destroying the running process does not return to it (the caller schedules).
 */
bool process_terminate(uint32_t pid, int32_t status);

/**
 * Ask a process to die (Ctrl+C, `kill`). A process that is not inside the kernel right now
 * is destroyed at once; otherwise it is destroyed on its way back to user mode.
 * @return false when the PID does not exist or is init
 */
bool process_request_kill(uint32_t pid);

// The current process ends (SYS_EXIT): never returns, the scheduler runs the next one
void process_exit_current(int32_t status) __attribute__((noreturn));

// Address-space policy for the page fault handler and syscall pointer checks
bool process_address_is_valid(struct ProcessControlBlock *pcb, uint32_t address);
// Whether a user page may be writable (some writable region or the heap touches it)
bool process_page_is_writable(struct ProcessControlBlock *pcb, uint32_t page);
// Region holding an address (NULL when none, heap included as an implicit region)
const struct ProcessMemoryRegion *process_find_region(struct ProcessControlBlock *pcb, uint32_t address);

/**
 * Move the program break of the current process (SYS_SBRK).
 * @return false when the break would leave [heap_base, USER_HEAP_LIMIT) or memory is short
 */
bool process_sbrk(struct ProcessControlBlock *pcb, int32_t increment, uint32_t *old_break);

// Page list bookkeeping, called by paging.c whenever a user page is mapped / unmapped
void process_on_user_page_mapped(struct PageDirectory *page_dir, uint32_t virtual_addr);
void process_on_user_page_unmapped(struct PageDirectory *page_dir, uint32_t virtual_addr);

// Keyboard ownership (foreground process)
bool process_owns_keyboard(struct ProcessControlBlock *pcb);
void process_set_foreground(uint32_t pid);

/**
 * Keyboard hook, called by the keyboard ISR for Ctrl+C: kills the foreground program
 * (never init). @return true when the key was consumed
 */
bool process_keyboard_interrupt(void);

// The process moves itself to the background (SYS_DETACH): its waiting parent is released
void process_detach(struct ProcessControlBlock *pcb);

/**
 * Exit status of an already finished process (kept for the last few exits so a parent can
 * still collect it). @return false if unknown
 */
bool process_lookup_exit_status(uint32_t pid, int32_t *status);

// Descriptor helpers (SYS_FD_*), -1 when the table is full
int32_t process_fd_install(struct ProcessControlBlock *pcb, uint8_t type, struct Pipe *pipe);
bool    process_fd_close(struct ProcessControlBlock *pcb, int32_t fd);
struct ProcessFile *process_fd_get(struct ProcessControlBlock *pcb, int32_t fd);

// Count of resident / swapped pages of a process (walks its page tables)
void process_count_pages(struct ProcessControlBlock *pcb, uint32_t *resident, uint32_t *swapped);

// Name of a state / wait reason / format for ps
const char *process_state_name(PROCESS_STATE state, PROCESS_WAIT wait);
const char *process_format_name(uint8_t format);

#endif
