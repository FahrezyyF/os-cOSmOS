#include "header/process/process.h"
#include "header/process/scheduler.h"
#include "header/process/exec.h"
#include "header/process/cex.h"
#include "header/process/elf.h"
#include "header/process/pipe.h"
#include "header/memory/paging.h"
#include "header/memory/swap.h"
#include "header/memory/kheap.h"
#include "header/filesystem/vfs.h"
#include "header/stdlib/string.h"
#include "header/cpu/gdt.h"
#include "header/driver/timer.h"
#include "header/driver/serial.h"
#include "header/kernel/statusbar.h"
#include "header/graphics/compositor.h"
#include "header/net/net.h"

struct ProcessControlBlock _process_list[PROCESS_COUNT_MAX];

struct ProcessManagerState process_manager_state = {
    .active_process_count = 0,
    .last_pid             = 0,
    .foreground_pid       = 0,
    .init_pid             = 0,
};

// Exit statuses of the last few processes, so a parent can collect one after the fact
// (no zombie PCBs are kept: a finished process frees its slot at once)
#define EXIT_RECORD_COUNT 16
static struct {
    uint32_t pid;
    int32_t  status;
} exit_records[EXIT_RECORD_COUNT];
static uint32_t exit_record_next = 0;

static uint32_t ceil_div(uint32_t a, uint32_t b) {
    return (a + b - 1) / b;
}

/* ================================ LOOKUP =========================================== */

struct ProcessControlBlock* process_get_current_running_pcb_pointer(void) {
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++)
        if (_process_list[i].metadata.state == PROCESS_STATE_RUNNING)
            return &_process_list[i];
    return NULL;
}

struct ProcessControlBlock *process_get_by_pid(uint32_t pid) {
    if (pid == 0)
        return NULL;
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++)
        if (_process_list[i].metadata.state != PROCESS_STATE_INACTIVE && _process_list[i].metadata.pid == pid)
            return &_process_list[i];
    return NULL;
}

struct ProcessControlBlock *process_get_by_page_directory(struct PageDirectory *page_dir) {
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++)
        if (_process_list[i].metadata.state != PROCESS_STATE_INACTIVE &&
            _process_list[i].context.page_directory_virtual_addr == page_dir)
            return &_process_list[i];
    return NULL;
}

struct ProcessControlBlock *process_get_by_current_address_space(void) {
    struct PageDirectory *dir = paging_get_current_page_directory_addr();
    if (dir == &_paging_kernel_page_directory)
        return NULL;
    return process_get_by_page_directory(dir);
}

int32_t process_list_get_inactive_index(void) {
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++)
        if (_process_list[i].metadata.state == PROCESS_STATE_INACTIVE)
            return (int32_t) i;
    return -1;
}

uint32_t process_generate_new_pid(void) {
    for (;;) {
        process_manager_state.last_pid++;
        if (process_manager_state.last_pid == 0)
            process_manager_state.last_pid = 1;
        if (process_get_by_pid(process_manager_state.last_pid) == NULL)
            return process_manager_state.last_pid;
    }
}

const char *process_create_error_name(int32_t code) {
    switch (code) {
        case PROCESS_CREATE_SUCCESS:                   return "success";
        case PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED: return "too many processes";
        case PROCESS_CREATE_FAIL_INVALID_ENTRYPOINT:   return "invalid entrypoint";
        case PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY:    return "not enough memory";
        case PROCESS_CREATE_FAIL_FS_READ_FAILURE:      return "cannot read the executable";
        case PROCESS_CREATE_FAIL_INVALID_EXECUTABLE:   return "invalid executable format";
        case PROCESS_CREATE_FAIL_INVALID_ARGUMENT:     return "invalid arguments";
        case PROCESS_CREATE_FAIL_NOT_EXECUTABLE:       return "not an executable";
        default:                                       return "unknown error";
    }
}

const char *process_state_name(PROCESS_STATE state, PROCESS_WAIT wait) {
    switch (state) {
        case PROCESS_STATE_READY:   return "ready";
        case PROCESS_STATE_RUNNING: return "running";
        case PROCESS_STATE_BLOCKED:
            switch (wait) {
                case PROCESS_WAIT_SLEEP:      return "sleep";
                case PROCESS_WAIT_KEY:        return "key";
                case PROCESS_WAIT_CHILD:      return "wait";
                case PROCESS_WAIT_PIPE_READ:  return "pipe-r";
                case PROCESS_WAIT_PIPE_WRITE: return "pipe-w";
                case PROCESS_WAIT_WINDOW:     return "window";
                case PROCESS_WAIT_NET:        return "net";
                default:                      return "blocked";
            }
        default: return "inactive";
    }
}

const char *process_format_name(uint8_t format) {
    switch (format) {
        case PROCESS_FORMAT_CEX: return "CEX";
        case PROCESS_FORMAT_ELF: return "ELF";
        default:                 return "flat";
    }
}

/* ================================ PAGE LIST (dynamic process size) =================== */

void process_on_user_page_mapped(struct PageDirectory *page_dir, uint32_t virtual_addr) {
    struct ProcessControlBlock *pcb = process_get_by_page_directory(page_dir);
    if (pcb == NULL)
        return;
    if (pcb->memory.page_frame_used_count == pcb->memory.page_list_capacity) {
        uint32_t cap = pcb->memory.page_list_capacity == 0 ? PROCESS_PAGE_FRAME_COUNT_MAX
                                                            : pcb->memory.page_list_capacity * 2;
        void **bigger = krealloc(pcb->memory.virtual_addr_used, cap * sizeof(void*));
        if (bigger == NULL)
            return; // untracked pages are still freed with the page tables at exit
        pcb->memory.virtual_addr_used  = bigger;
        pcb->memory.page_list_capacity = cap;
    }
    pcb->memory.virtual_addr_used[pcb->memory.page_frame_used_count++] = (void*) virtual_addr;
    if (pcb->memory.page_frame_used_count > pcb->memory.peak_pages)
        pcb->memory.peak_pages = pcb->memory.page_frame_used_count;
}

void process_on_user_page_unmapped(struct PageDirectory *page_dir, uint32_t virtual_addr) {
    struct ProcessControlBlock *pcb = process_get_by_page_directory(page_dir);
    if (pcb == NULL || pcb->memory.virtual_addr_used == NULL)
        return;
    for (uint32_t i = pcb->memory.page_frame_used_count; i-- > 0;) {
        if ((uint32_t) pcb->memory.virtual_addr_used[i] == virtual_addr) {
            pcb->memory.virtual_addr_used[i] = pcb->memory.virtual_addr_used[--pcb->memory.page_frame_used_count];
            return;
        }
    }
}

void process_count_pages(struct ProcessControlBlock *pcb, uint32_t *resident, uint32_t *swapped) {
    *resident = *swapped = 0;
    struct PageDirectory *dir = pcb->context.page_directory_virtual_addr;
    for (uint32_t i = 0; i < pcb->memory.page_frame_used_count && pcb->memory.virtual_addr_used; i++) {
        uint32_t *pte = paging_get_pte(dir, (uint32_t) pcb->memory.virtual_addr_used[i], false);
        if (pte == NULL)
            continue;
        if (*pte & PTE_PRESENT)
            (*resident)++;
        else if (*pte & PTE_SWAPPED)
            (*swapped)++;
    }
}

/* ================================ MEMORY POLICY ==================================== */

const struct ProcessMemoryRegion *process_find_region(struct ProcessControlBlock *pcb, uint32_t address) {
    static struct ProcessMemoryRegion heap_view;
    for (uint32_t i = 0; i < pcb->memory.region_count; i++)
        if (address >= pcb->memory.regions[i].start && address < pcb->memory.regions[i].end)
            return &pcb->memory.regions[i];
    if (address >= pcb->memory.heap_base && address < pcb->memory.heap_break) {
        heap_view.start = pcb->memory.heap_base;
        heap_view.end   = pcb->memory.heap_break;
        heap_view.flags = PROCESS_REGION_READ | PROCESS_REGION_WRITE;
        heap_view.kind  = PROCESS_REGION_OTHER;
        return &heap_view;
    }
    return NULL;
}

bool process_page_is_writable(struct ProcessControlBlock *pcb, uint32_t page) {
    page &= PTE_ADDRESS_MASK;
    if (page + PAGE_FRAME_SIZE > pcb->memory.heap_base && page < pcb->memory.heap_break)
        return true;
    for (uint32_t i = 0; i < pcb->memory.region_count; i++) {
        const struct ProcessMemoryRegion *r = &pcb->memory.regions[i];
        if ((r->flags & PROCESS_REGION_WRITE) && page < r->end && r->start < page + PAGE_FRAME_SIZE)
            return true;
    }
    return false;
}

bool process_address_is_valid(struct ProcessControlBlock *pcb, uint32_t address) {
    return pcb != NULL && address < USER_SPACE_LIMIT && process_find_region(pcb, address) != NULL;
}

bool process_sbrk(struct ProcessControlBlock *pcb, int32_t increment, uint32_t *old_break) {
    uint32_t old = pcb->memory.heap_break;
    if (increment > 0) {
        if ((uint32_t) increment > USER_HEAP_LIMIT - old)
            return false;
        // Commit limit: free RAM + free swap must be able to back the new memory
        if (!paging_allocate_check((uint32_t) increment))
            return false;
        pcb->memory.heap_break = old + (uint32_t) increment;
    } else if (increment < 0) {
        uint32_t decrement = (uint32_t) (-(int64_t) increment);
        if (decrement > old - pcb->memory.heap_base)
            return false;
        pcb->memory.heap_break = old - decrement;
        // Give back every page that is now entirely above the break
        uint32_t first = (pcb->memory.heap_break + PAGE_FRAME_SIZE - 1) & ~(PAGE_FRAME_SIZE - 1);
        for (uint32_t page = first; page < old; page += PAGE_FRAME_SIZE)
            paging_free_user_page_frame(pcb->context.page_directory_virtual_addr, (void*) page);
    }
    *old_break = old;
    return true;
}

/* ================================ FILE DESCRIPTORS ================================= */

static void fd_retain(struct ProcessFile *f) {
    if (f->type == PROCESS_FD_PIPE_READ)
        pipe_add_reader(f->pipe);
    else if (f->type == PROCESS_FD_PIPE_WRITE)
        pipe_add_writer(f->pipe);
}

static void fd_release(struct ProcessFile *f) {
    if (f->type == PROCESS_FD_PIPE_READ)
        pipe_close_reader(f->pipe);
    else if (f->type == PROCESS_FD_PIPE_WRITE)
        pipe_close_writer(f->pipe);
    f->type = PROCESS_FD_NONE;
    f->pipe = NULL;
}

int32_t process_fd_install(struct ProcessControlBlock *pcb, uint8_t type, struct Pipe *pipe) {
    for (int32_t fd = 0; fd < PROCESS_FD_MAX; fd++) {
        struct ProcessFile *f = &pcb->files[fd];
        if (f->type != PROCESS_FD_NONE)
            continue;
        f->type = type;
        f->pipe = pipe;
        fd_retain(f);
        return fd;
    }
    return -1;
}

struct ProcessFile *process_fd_get(struct ProcessControlBlock *pcb, int32_t fd) {
    if (pcb == NULL || fd < 0 || fd >= PROCESS_FD_MAX || pcb->files[fd].type == PROCESS_FD_NONE)
        return NULL;
    return &pcb->files[fd];
}

bool process_fd_close(struct ProcessControlBlock *pcb, int32_t fd) {
    struct ProcessFile *f = process_fd_get(pcb, fd);
    if (f == NULL)
        return false;
    fd_release(f);
    return true;
}

// Child descriptor `slot` = creator's descriptor `fd` (or the console)
static void fd_inherit(struct ProcessControlBlock *child, int32_t slot, struct ProcessControlBlock *creator, int32_t fd) {
    struct ProcessFile *src = creator != NULL && fd >= 0 ? process_fd_get(creator, fd) : NULL;
    struct ProcessFile *dst = &child->files[slot];
    if (src == NULL) {
        dst->type = PROCESS_FD_CONSOLE;
        dst->pipe = NULL;
        return;
    }
    *dst = *src;
    fd_retain(dst);
}

/* ================================ KEYBOARD OWNERSHIP =============================== */

bool process_owns_keyboard(struct ProcessControlBlock *pcb) {
    return pcb != NULL && pcb->metadata.pid == process_manager_state.foreground_pid && !compositor_has_keyboard_focus();
}

void process_set_foreground(uint32_t pid) {
    if (process_manager_state.foreground_pid != pid) {
        process_manager_state.foreground_pid = pid;
        statusbar_mark_dirty();
    }
}

bool process_keyboard_interrupt(void) {
    struct ProcessControlBlock *fg = process_get_by_pid(process_manager_state.foreground_pid);
    if (fg == NULL || fg->metadata.pid == process_manager_state.init_pid)
        return false; // the shell gets ^C as a normal key (cancels the line)
    fg->metadata.kill_pending = true;
    scheduler_request_resched();
    return true;
}

/* ================================ CREATION ========================================= */

// argv / envp / cwd copied into one kernel block before the address space changes
struct ArgBlock {
    uint32_t argc;
    uint32_t envc;
    uint32_t offsets[2 * PROCESS_ARG_MAX];
    uint32_t used;
    char     strings[PROCESS_ARG_BYTES_MAX];
};

static bool arg_push(struct ArgBlock *b, const char *s, uint32_t index) {
    uint32_t len = (uint32_t) strlen(s);
    if (b->used + len + 1 > PROCESS_ARG_BYTES_MAX)
        return false;
    b->offsets[index] = b->used;
    memcpy(b->strings + b->used, s, len + 1);
    b->used += len + 1;
    return true;
}

static int32_t build_args(struct ArgBlock *b, const struct ProcessCreateInfo *info, const char *name) {
    memset(b, 0, sizeof(*b) - sizeof(b->strings));
    if (info->argc > PROCESS_ARG_MAX || info->envc > PROCESS_ARG_MAX)
        return PROCESS_CREATE_FAIL_INVALID_ARGUMENT;
    if (info->argv == NULL || info->argc == 0) {
        b->argc = 1;
        if (!arg_push(b, name, 0))
            return PROCESS_CREATE_FAIL_INVALID_ARGUMENT;
    } else {
        b->argc = info->argc;
        for (uint32_t i = 0; i < info->argc; i++)
            if (info->argv[i] == NULL || !arg_push(b, info->argv[i], i))
                return PROCESS_CREATE_FAIL_INVALID_ARGUMENT;
    }
    b->envc = info->envp != NULL ? info->envc : 0;
    for (uint32_t i = 0; i < b->envc; i++)
        if (info->envp[i] == NULL || !arg_push(b, info->envp[i], PROCESS_ARG_MAX + i))
            return PROCESS_CREATE_FAIL_INVALID_ARGUMENT;
    return PROCESS_CREATE_SUCCESS;
}

/**
 * Initial user stack (System V i386 style, read by crt0.s):
 *   esp ->  argc
 *           argv[0] .. argv[argc-1], NULL
 *           envp[0] .. envp[envc-1], NULL
 *           (strings, up to USER_STACK_TOP)
 * Runs with the new page directory loaded.
 */
static uint32_t setup_user_stack(const struct ArgBlock *b) {
    uint32_t strings = (USER_STACK_TOP - ((b->used + 3) & ~3u));
    memcpy((void*) strings, b->strings, b->used);
    uint32_t words = 1 + b->argc + 1 + b->envc + 1;
    uint32_t esp   = (strings - words * 4) & ~0xFu;
    uint32_t *sp   = (uint32_t*) esp;
    uint32_t k = 0;
    sp[k++] = b->argc;
    for (uint32_t i = 0; i < b->argc; i++)
        sp[k++] = strings + b->offsets[i];
    sp[k++] = 0;
    for (uint32_t i = 0; i < b->envc; i++)
        sp[k++] = strings + b->offsets[PROCESS_ARG_MAX + i];
    sp[k++] = 0;
    return esp;
}

static void set_cmdline(struct ProcessControlBlock *pcb, const struct ArgBlock *b) {
    char *dst = pcb->metadata.cmdline;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < b->argc && pos + 1 < PROCESS_CMDLINE_MAX; i++) {
        const char *s = b->strings + b->offsets[i];
        if (i > 0)
            dst[pos++] = ' ';
        while (*s && pos + 1 < PROCESS_CMDLINE_MAX)
            dst[pos++] = *s++;
    }
    dst[pos] = '\0';
}

// Release everything a (possibly half-built) PCB owns, the slot becomes INACTIVE
static void pcb_release(struct ProcessControlBlock *pcb) {
    struct PageDirectory *dir = pcb->context.page_directory_virtual_addr;
    for (int32_t fd = 0; fd < PROCESS_FD_MAX; fd++)
        fd_release(&pcb->files[fd]);
    if (dir != NULL) {
        if (paging_get_current_page_directory_addr() == dir)
            paging_use_page_directory(&_paging_kernel_page_directory);
        paging_unregister_address_space(dir);
        // Free the pages through the PCB page list first (the list is detached so the
        // unmapped hook does not edit it while we walk it), then whatever else is left
        void   **list  = pcb->memory.virtual_addr_used;
        uint32_t count = pcb->memory.page_frame_used_count;
        pcb->memory.virtual_addr_used     = NULL;
        pcb->memory.page_frame_used_count = 0;
        for (uint32_t i = 0; i < count; i++)
            paging_free_user_page_frame(dir, list[i]);
        kfree(list);
        paging_free_page_directory(dir);
    } else {
        kfree(pcb->memory.virtual_addr_used);
    }
    memset(pcb, 0, sizeof(*pcb));
    pcb->metadata.state = PROCESS_STATE_INACTIVE;
    if (process_manager_state.active_process_count > 0)
        process_manager_state.active_process_count--;
}

int32_t process_create(const struct ProcessCreateInfo *info, uint32_t *pid_out) {
    int32_t retcode = PROCESS_CREATE_SUCCESS;
    struct ArgBlock *args = NULL;
    uint8_t *image = NULL;
    struct ProcessControlBlock *new_pcb = NULL;
    struct PageDirectory *old_dir = paging_get_current_page_directory_addr();
    struct ProcessControlBlock *creator = process_get_by_pid(info->ppid);

    if (process_manager_state.active_process_count >= PROCESS_COUNT_MAX) {
        retcode = PROCESS_CREATE_FAIL_MAX_PROCESS_EXCEEDED;
        goto exit_cleanup;
    }

    // Ensure entrypoint is not located at kernel's section at higher half
    if (info->load_address >= KERNEL_VIRTUAL_ADDRESS_BASE) {
        retcode = PROCESS_CREATE_FAIL_INVALID_ENTRYPOINT;
        goto exit_cleanup;
    }

    // Executable: name and size (the whole file is read into the kernel first)
    char name[SYSCALL_NAME_MAX + 1];
    if (info->name_len == 0 || info->name == NULL) {
        retcode = PROCESS_CREATE_FAIL_FS_READ_FAILURE;
        goto exit_cleanup;
    }
    memcpy(name, info->name, info->name_len);
    name[info->name_len] = '\0';
    struct SyscallStat st = {.parent = info->parent_inode, .name = name, .name_len = info->name_len};
    if (!vfs_ready() || vfs_stat(&st) != 0 || st.type != SYSCALL_TYPE_FILE || st.size == 0) {
        retcode = PROCESS_CREATE_FAIL_FS_READ_FAILURE;
        goto exit_cleanup;
    }
    uint32_t limit = info->max_size != 0 && info->max_size < PROCESS_EXECUTABLE_MAX_SIZE ? info->max_size
                                                                                         : PROCESS_EXECUTABLE_MAX_SIZE;

    // Check whether memory is enough for the executable and additional frame for user stack
    uint32_t page_frame_count_needed = ceil_div(st.size + PAGE_FRAME_SIZE, PAGE_FRAME_SIZE);
    if (st.size > limit || !paging_allocate_check(page_frame_count_needed * PAGE_FRAME_SIZE)) {
        retcode = PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
        goto exit_cleanup;
    }

    args  = kmalloc(sizeof(*args));
    image = kmalloc(st.size);
    if (args == NULL || image == NULL) {
        retcode = PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
        goto exit_cleanup;
    }
    retcode = build_args(args, info, name);
    if (retcode != PROCESS_CREATE_SUCCESS)
        goto exit_cleanup;
    struct EXT2DriverRequest read_request = {
        .buf          = image,
        .name         = name,
        .name_len     = info->name_len,
        .parent_inode = info->parent_inode,
        .buffer_size  = st.size,
        .is_directory = false,
    };
    if (vfs_read(read_request) != 0) {
        retcode = PROCESS_CREATE_FAIL_FS_READ_FAILURE;
        goto exit_cleanup;
    }

    // Process PCB
    int32_t p_index = process_list_get_inactive_index();
    new_pcb = &(_process_list[p_index]);
    memset(new_pcb, 0, sizeof(*new_pcb));
    new_pcb->metadata.pid   = process_generate_new_pid();
    new_pcb->metadata.state = PROCESS_STATE_BLOCKED; // not schedulable while it is being built
    process_manager_state.active_process_count++;

    // Virtual address space (book 4.1.3.1)
    struct PageDirectory *dir = paging_create_new_page_directory();
    if (dir == NULL) {
        retcode = PROCESS_CREATE_FAIL_NOT_ENOUGH_MEMORY;
        goto exit_cleanup;
    }
    new_pcb->context.page_directory_virtual_addr = dir;
    new_pcb->memory.heap_base  = USER_HEAP_BASE;
    new_pcb->memory.heap_break = USER_HEAP_BASE;
    paging_register_address_space(dir);

    if (info->require_header && !cex_has_magic(image, st.size) && !elf_has_magic(image, st.size)) {
        retcode = PROCESS_CREATE_FAIL_NOT_EXECUTABLE;
        goto exit_cleanup;
    }

    // Load executable (book 4.1.3.2): switch to the new address space for the copy
    paging_use_page_directory(dir);
    retcode = exec_load_image(new_pcb, image, st.size, info->load_address);
    uint32_t user_esp = 0;
    if (retcode == PROCESS_CREATE_SUCCESS)
        user_esp = setup_user_stack(args);
    paging_use_page_directory(old_dir);
    if (retcode != PROCESS_CREATE_SUCCESS)
        goto exit_cleanup;

    // Context initialization (book 4.1.3.3): user segments (RPL 3), interrupts enabled
    struct Context *ctx = &new_pcb->context;
    memset(&ctx->cpu, 0, sizeof(ctx->cpu));
    uint32_t user_data = GDT_USER_DATA_SEGMENT_SELECTOR | 0x3;
    ctx->cpu.segment.ds = ctx->cpu.segment.es = ctx->cpu.segment.fs = ctx->cpu.segment.gs = user_data;
    ctx->cpu.stack.esp  = user_esp;
    ctx->cpu.stack.ebp  = 0;
    ctx->eip            = new_pcb->memory.entry;
    ctx->eflags         = CPU_EFLAGS_BASE_FLAG | CPU_EFLAGS_FLAG_INTERRUPT_ENABLE;

    // Process metadata (book 4.1.3.4)
    strncpy(new_pcb->metadata.name, name, PROCESS_NAME_LENGTH_MAX - 1);
    set_cmdline(new_pcb, args);
    new_pcb->metadata.ppid      = info->ppid;
    new_pcb->metadata.priority  = info->priority > PROCESS_PRIORITY_HIGH ? PROCESS_PRIORITY_NORMAL : info->priority;
    new_pcb->metadata.start_ms  = timer_get_ms();
    new_pcb->metadata.cwd_inode = info->cwd_inode != 0 ? info->cwd_inode : vfs_root();
    strncpy(new_pcb->metadata.cwd, info->cwd != NULL && info->cwd[0] == '/' ? info->cwd : "/", PROCESS_CWD_MAX - 1);
    fd_inherit(new_pcb, 0, creator, info->stdin_fd);
    fd_inherit(new_pcb, 1, creator, info->stdout_fd);
    fd_inherit(new_pcb, 2, NULL, -1); // stderr is always the console
    new_pcb->metadata.state = PROCESS_STATE_READY;
    process_manager_state.created_total++;
    if (info->foreground)
        process_set_foreground(new_pcb->metadata.pid);
    if (pid_out != NULL)
        *pid_out = new_pcb->metadata.pid;
    serial_printf("[proc] created pid %u \"%s\" (%s, %u bytes, entry 0x%x, esp 0x%x, ppid %u)\n",
                  new_pcb->metadata.pid, new_pcb->metadata.cmdline, process_format_name(new_pcb->metadata.format),
                  st.size, ctx->eip, user_esp, info->ppid);
    statusbar_mark_dirty();
    new_pcb = NULL; // success: keep it

exit_cleanup:
    if (new_pcb != NULL)
        pcb_release(new_pcb);
    kfree(image);
    kfree(args);
    return retcode;
}

int32_t process_create_user_process(struct EXT2DriverRequest request) {
    struct ProcessCreateInfo info = {
        .parent_inode = request.parent_inode,
        .name         = request.name,
        .name_len     = request.name_len,
        .load_address = (uint32_t) request.buf,
        .max_size     = request.buffer_size,
        .ppid         = 0,
        .stdin_fd     = -1,
        .stdout_fd    = -1,
        .foreground   = true,
        .priority     = PROCESS_PRIORITY_NORMAL,
    };
    uint32_t pid = 0;
    int32_t code = process_create(&info, &pid);
    if (code == PROCESS_CREATE_SUCCESS && process_manager_state.init_pid == 0)
        process_manager_state.init_pid = pid; // the first process is init (the shell)
    return code;
}

/* ================================ TERMINATION ====================================== */

static void record_exit(uint32_t pid, int32_t status) {
    exit_records[exit_record_next].pid    = pid;
    exit_records[exit_record_next].status = status;
    exit_record_next = (exit_record_next + 1) % EXIT_RECORD_COUNT;
}

bool process_lookup_exit_status(uint32_t pid, int32_t *status) {
    for (uint32_t i = 0; i < EXIT_RECORD_COUNT; i++) {
        if (exit_records[i].pid == pid && pid != 0) {
            *status = exit_records[i].status;
            return true;
        }
    }
    return false;
}

// Wake a parent blocked in SYS_WAIT for `pid` (or for any child)
static void notify_parent(struct ProcessControlBlock *child, int32_t status) {
    struct ProcessControlBlock *parent = process_get_by_pid(child->metadata.ppid);
    if (parent == NULL || parent->metadata.state != PROCESS_STATE_BLOCKED ||
        parent->metadata.wait_reason != PROCESS_WAIT_CHILD)
        return;
    if (parent->metadata.wait_arg != child->metadata.pid && parent->metadata.wait_arg != 0)
        return;
    parent->metadata.wait_done   = true;
    parent->metadata.wait_status = status;
    parent->metadata.wait_arg    = child->metadata.pid;
    parent->metadata.state       = PROCESS_STATE_READY;
    scheduler_request_resched();
}

void process_detach(struct ProcessControlBlock *pcb) {
    if (pcb->metadata.detached)
        return;
    pcb->metadata.detached = true;
    if (process_manager_state.foreground_pid == pcb->metadata.pid) {
        struct ProcessControlBlock *parent = process_get_by_pid(pcb->metadata.ppid);
        process_set_foreground(parent != NULL ? parent->metadata.pid : process_manager_state.init_pid);
    }
    notify_parent(pcb, PROCESS_EXIT_DETACHED);
    statusbar_mark_dirty();
}

bool process_terminate(uint32_t pid, int32_t status) {
    struct ProcessControlBlock *pcb = process_get_by_pid(pid);
    if (pcb == NULL)
        return false;
    serial_printf("[proc] pid %u \"%s\" ended with status %d (%u pages at peak, %u ms CPU)\n", pid,
                  pcb->metadata.name, (int) status, pcb->memory.peak_pages, pcb->metadata.cpu_ms);
    record_exit(pid, status);

    // Keyboard goes back to whoever waited for this program
    if (process_manager_state.foreground_pid == pid) {
        struct ProcessControlBlock *parent = process_get_by_pid(pcb->metadata.ppid);
        uint32_t next = parent != NULL ? parent->metadata.pid : process_manager_state.init_pid;
        process_set_foreground(next == pid ? 0 : next);
    }
    notify_parent(pcb, status);
    // Orphans are adopted by init (no orphan processes)
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        struct ProcessControlBlock *p = &_process_list[i];
        if (p != pcb && p->metadata.state != PROCESS_STATE_INACTIVE && p->metadata.ppid == pid)
            p->metadata.ppid = pid == process_manager_state.init_pid ? 0 : process_manager_state.init_pid;
    }
    if (pid == process_manager_state.init_pid)
        process_manager_state.init_pid = 0; // the scheduler starts a new shell

    compositor_on_process_exit(pid);
    statusbar_remove_widget(pid);
    net_on_process_exit(pid);
    pcb_release(pcb);
    statusbar_mark_dirty();
    return true;
}

bool process_destroy(uint32_t pid) {
    return process_terminate(pid, PROCESS_EXIT_KILLED);
}

bool process_request_kill(uint32_t pid) {
    struct ProcessControlBlock *pcb = process_get_by_pid(pid);
    if (pcb == NULL || pid == process_manager_state.init_pid)
        return false;
    pcb->metadata.kill_pending = true;
    scheduler_request_resched();
    return true;
}

void process_exit_current(int32_t status) {
    __asm__ volatile("cli");
    struct ProcessControlBlock *pcb = process_get_current_running_pcb_pointer();
    if (pcb != NULL)
        process_terminate(pcb->metadata.pid, status);
    scheduler_switch_to_next_process();
}
