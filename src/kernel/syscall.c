#include <stddef.h>
#include "header/kernel/syscall.h"
#include "header/kernel/user.h"
#include "header/kernel/boot.h"
#include "header/kernel/statusbar.h"
#include "header/kernel-entrypoint.h"
#include "header/cpu/interrupt.h"
#include "header/filesystem/vfs.h"
#include "header/memory/paging.h"
#include "header/memory/swap.h"
#include "header/memory/kheap.h"
#include "header/text/console.h"
#include "header/text/framebuffer.h"
#include "header/graphics/graphics.h"
#include "header/driver/keyboard.h"
#include "header/driver/timer.h"
#include "header/driver/rtc.h"
#include "header/driver/mouse.h"
#include "header/driver/speaker.h"
#include "header/driver/power.h"
#include "header/driver/disk.h"
#include "header/stdlib/string.h"
#include "header/process/process.h"
#include "header/process/scheduler.h"
#include "header/process/pipe.h"
#include "header/graphics/compositor.h"
#include "header/net/net.h"

/**
 * Kernel side of the system call ABI (header/kernel/syscall.h).
 * Every pointer coming from ring 3 is checked with user_range_is_valid() /
 * user_string_is_valid() before the kernel touches it, and request structures are copied
 * into kernel memory first so the program cannot change them halfway through a call.
 */

#define DEFAULT_COLOR 0x07 // light gray on black, used when a program passes color 0

// Copy a user structure into the kernel; false when the pointer is bad
static bool copy_in(void *dst, uint32_t user_ptr, uint32_t size) {
    if (!user_range_is_valid((void*) user_ptr, size))
        return false;
    memcpy(dst, (void*) user_ptr, size);
    return true;
}

static bool copy_out(uint32_t user_ptr, const void *src, uint32_t size) {
    if (!user_range_is_valid((void*) user_ptr, size))
        return false;
    memcpy((void*) user_ptr, src, size);
    return true;
}

static void put_retcode(uint32_t user_ptr, int8_t code) {
    copy_out(user_ptr, &code, sizeof(code));
}

/**
 * Bring an EXT2DriverRequest into the kernel: the structure and its name are copied, the
 * data buffer is only validated (it may be large; the drivers move it sector by sector).
 */
enum BufferCheck {
    BUFFER_IGNORED,      // delete()
    BUFFER_USED,         // read(), read_directory()
    BUFFER_UNLESS_DIR,   // write(): a new folder has no data
};

static bool fetch_request(uint32_t user_ptr, struct EXT2DriverRequest *request, char name[SYSCALL_NAME_MAX + 1],
                          enum BufferCheck check) {
    if (!copy_in(request, user_ptr, sizeof(*request)))
        return false;
    if (!copy_in(name, (uint32_t) request->name, request->name_len))
        return false;
    name[request->name_len] = '\0';
    request->name = name;
    if (check == BUFFER_IGNORED || (check == BUFFER_UNLESS_DIR && request->is_directory))
        return true;
    return user_range_is_valid(request->buf, request->buffer_size);
}

static uint32_t last_console_writer = 0;

static void print_colored(const char *text, uint32_t count, uint32_t color) {
    uint8_t attr = (color & 0xFF) == 0 ? DEFAULT_COLOR : (uint8_t) color;
    // A background program starts its output on a fresh line instead of in the middle of
    // the line the shell is editing (the shell redisplays its line afterwards)
    struct ProcessControlBlock *me = process_get_current_running_pcb_pointer();
    uint32_t pid = me != NULL ? me->metadata.pid : 0;
    if (count > 0 && pid != last_console_writer && pid != process_manager_state.foreground_pid) {
        uint8_t row, col;
        console_get_cursor(&row, &col);
        if (col != 0)
            console_putchar('\n');
    }
    if (count > 0)
        last_console_writer = pid;
    console_write_color(text, count, attr & 0xF, (attr >> 4) & 0xF);
}

static struct ProcessControlBlock *current(void) {
    return process_get_current_running_pcb_pointer();
}

// Book syscall 4 (non-blocking): only the process that owns the keyboard gets keys
static void sys_getchar(uint32_t user_ptr) {
    char c = 0;
    kernel_idle();
    if (process_owns_keyboard(current()))
        get_keyboard_buffer(&c);
    copy_out(user_ptr, &c, 1);
}

// Blocking getchar: the process sleeps (restartable) until it owns the keyboard and a key is there
static void sys_getchar_wait(const struct InterruptFrame *frame, uint32_t user_ptr) {
    char c = 0;
    if (process_owns_keyboard(current()))
        get_keyboard_buffer(&c);
    if (c == 0) {
        kernel_idle();
        scheduler_block_current(frame, PROCESS_WAIT_KEY, 0, true);
    }
    copy_out(user_ptr, &c, 1);
}

static void sys_sleep(const struct InterruptFrame *frame, uint32_t ms) {
    struct ProcessControlBlock *pcb = current();
    if (ms == 0 || pcb == NULL)
        scheduler_yield_current(frame);
    pcb->metadata.wake_time_ms = timer_get_ms() + ms;
    scheduler_block_current(frame, PROCESS_WAIT_SLEEP, 0, false); // returns to user when woken
}

static void sys_meminfo(uint32_t user_ptr) {
    struct SwapStats swap;
    swap_get_stats(&swap);
    struct SyscallMemInfo info = {
        .total_frames      = page_manager_state.total_page_frame_count,
        .free_frames       = page_manager_state.free_page_frame_count,
        .kernel_frames     = KERNEL_RESERVED_PAGE_FRAME_COUNT,
        .kernel_image_frames = paging_kernel_image_frames(),
        .kernel_pool_free  = paging_kernel_pool_free(),
        .user_pages        = page_manager_state.user_page_count,
        .page_tables       = page_manager_state.page_table_count,
        .kernel_heap_pages = page_manager_state.kernel_heap_pages,
        .swap_total        = swap.total_slots,
        .swap_used         = swap.used_slots,
        .page_faults       = swap.page_faults,
        .demand_zero       = swap.demand_zero,
        .swap_ins          = swap.swap_ins,
        .swap_outs         = swap.swap_outs,
        .failed_faults     = swap.failed,
        .user_break        = current() != NULL ? current()->memory.heap_break : 0,
        .system_memory_mb  = SYSTEM_MEMORY_MB,
    };
    copy_out(user_ptr, &info, sizeof(info));
}

static void sys_kheap_info(uint32_t user_ptr) {
    struct HeapStats st;
    kheap_get_stats(&st);
    struct SyscallHeapInfo info = {
        .committed_bytes = st.committed_bytes,
        .used_bytes      = st.used_bytes,
        .used_chunks     = st.used_chunks,
        .free_bytes      = st.free_bytes,
        .free_chunks     = st.free_chunks,
        .largest_free    = st.largest_free,
        .alloc_count     = st.alloc_count,
        .free_count      = st.free_count,
    };
    copy_out(user_ptr, &info, sizeof(info));
}

static void sys_time(uint32_t user_ptr) {
    struct RTCTime t;
    rtc_read(&t);
    struct SyscallTime out = {
        .year = t.year, .month = t.month, .day = t.day,
        .hour = t.hour, .minute = t.minute, .second = t.second,
        .unix_time = rtc_unix_time(),
    };
    copy_out(user_ptr, &out, sizeof(out));
}

static void sys_sysinfo(uint32_t user_ptr) {
    struct SyscallSysInfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.os_name, "cOSmOS", sizeof(info.os_name) - 1);
    strncpy(info.version, COSMOS_VERSION, sizeof(info.version) - 1);
    strncpy(info.fs_name, vfs_name(), sizeof(info.fs_name) - 1);
    info.graphics        = framebuffer_is_graphics();
    info.width           = info.graphics ? graphics_get_width() : 0;
    info.height          = info.graphics ? graphics_get_height() : 0;
    info.bga_version     = info.graphics ? graphics_get_bga_version() : 0;
    info.lfb_address     = info.graphics ? graphics_get_lfb_address() : 0;
    info.cols            = framebuffer_get_cols();
    info.rows            = framebuffer_get_rows();
    info.ram_kb          = boot_info.managed_ram_kb;
    info.detected_ram_kb = boot_info.detected_ram_kb;
    info.kernel_image_kb = ((uint32_t) &_linker_kernel_physical_addr_end - (uint32_t) &_linker_kernel_physical_addr_start) / 1024;
    info.kernel_stack_kb = 2048;
    info.uptime_ms       = timer_get_ms();
    info.mouse           = mouse_is_present();
    info.mouse_wheel     = mouse_has_wheel();
    struct SwapStats swap;
    swap_get_stats(&swap);
    info.swap = swap.available;
    struct ATADriveInfo drive;
    ata_identify(vfs_drive(), &drive);
    if (drive.present) {
        strncpy(info.disk_model, drive.model, sizeof(info.disk_model) - 1);
        info.disk_sectors = drive.sector_count;
    }
    strncpy(info.cmdline, boot_info.cmdline, sizeof(info.cmdline) - 1);
    copy_out(user_ptr, &info, sizeof(info));
}

static void sys_mouse(uint32_t user_ptr) {
    struct SyscallMouse out = {.present = mouse_is_present(), .has_wheel = mouse_has_wheel()};
    if (out.present) {
        struct MouseState m;
        mouse_get_state(&m);
        out.x       = m.x;
        out.y       = m.y;
        out.buttons = m.buttons;
        out.packets = m.packets;
        if (m.changed)
            framebuffer_pointer_move(m.x, m.y);
    }
    copy_out(user_ptr, &out, sizeof(out));
}

/* -- file system extensions -- */

static void sys_fs_rename(uint32_t user_ptr, uint32_t ret_ptr) {
    struct SyscallRename r;
    char name[SYSCALL_NAME_MAX + 1], new_name[SYSCALL_NAME_MAX + 1];
    if (!copy_in(&r, user_ptr, sizeof(r)) ||
        !copy_in(name, (uint32_t) r.name, r.name_len) || !copy_in(new_name, (uint32_t) r.new_name, r.new_name_len)) {
        put_retcode(ret_ptr, -1);
        return;
    }
    put_retcode(ret_ptr, vfs_rename(r.parent, name, r.name_len, r.new_parent, new_name, r.new_name_len));
}

static void sys_fs_stat(uint32_t user_ptr, uint32_t ret_ptr) {
    struct SyscallStat st;
    char name[SYSCALL_NAME_MAX + 1];
    if (!copy_in(&st, user_ptr, sizeof(st)) || !copy_in(name, (uint32_t) st.name, st.name_len)) {
        put_retcode(ret_ptr, -1);
        return;
    }
    const char *user_name = st.name;
    st.name = name;
    int8_t ret = vfs_stat(&st);
    st.name = user_name;
    copy_out(user_ptr, &st, sizeof(st));
    put_retcode(ret_ptr, ret);
}

static void sys_fs_find(uint32_t user_ptr) {
    struct SyscallFind f;
    char name[SYSCALL_NAME_MAX + 1];
    if (!copy_in(&f, user_ptr, sizeof(f)) || !copy_in(name, (uint32_t) f.name, f.name_len))
        return;
    if (f.max_hits > 4096)
        f.max_hits = 4096;
    if (!user_range_is_valid(f.hits, f.max_hits * sizeof(struct SyscallFindHit)))
        return;
    struct SyscallFindHit *hits = kmalloc((f.max_hits ? f.max_hits : 1) * sizeof(*hits));
    if (hits == NULL)
        return;
    f.total = vfs_find(name, f.name_len, hits, f.max_hits);
    uint32_t n = f.total < f.max_hits ? f.total : f.max_hits;
    copy_out((uint32_t) f.hits, hits, n * sizeof(*hits));
    kfree(hits);
    copy_out(user_ptr + offsetof(struct SyscallFind, total), &f.total, sizeof(f.total));
}

static void sys_fs_name_of(uint32_t user_ptr, uint32_t ret_ptr) {
    struct SyscallNameOf *out = kmalloc(sizeof(*out));
    if (out == NULL || !copy_in(&out->inode, user_ptr, sizeof(out->inode))) {
        kfree(out);
        put_retcode(ret_ptr, -1);
        return;
    }
    memset(out->name, 0, sizeof(out->name));
    bool ok = vfs_name_of(out->inode, &out->parent, out->name);
    if (ok)
        copy_out(user_ptr, out, sizeof(*out));
    kfree(out);
    put_retcode(ret_ptr, ok ? 0 : 1);
}

/* ================================ CHAPTER 4: PROCESSES ============================= */

#define EXEC_STRING_MAX 1024

// Copy a user string array (argv / envp) into one kernel block; pointers go to `out`
static bool copy_string_array(uint32_t user_array, uint32_t count, char *block, uint32_t block_size,
                              uint32_t *used, const char **out) {
    if (count == 0)
        return true;
    if (count > PROCESS_ARG_MAX || !user_range_is_valid((void*) user_array, count * sizeof(char*)))
        return false;
    const char *const *array = (const char *const *) user_array;
    for (uint32_t i = 0; i < count; i++) {
        const char *s = array[i];
        if (!user_string_is_valid(s, EXEC_STRING_MAX))
            return false;
        uint32_t len = (uint32_t) strlen(s);
        if (*used + len + 1 > block_size)
            return false;
        memcpy(block + *used, s, len + 1);
        out[i] = block + *used;
        *used += len + 1;
    }
    return true;
}

static void sys_process_create(uint32_t user_ptr, uint32_t result_ptr) {
    struct ProcessControlBlock *me = current();
    struct SyscallExec ex;
    int32_t result = -PROCESS_CREATE_FAIL_INVALID_ARGUMENT;
    char name[SYSCALL_NAME_MAX + 1];
    struct {
        const char *argv[PROCESS_ARG_MAX];
        const char *envp[PROCESS_ARG_MAX];
        char        block[PROCESS_ARG_BYTES_MAX];
    } *args = kmalloc(sizeof(*args));
    uint32_t used = 0;
    if (args == NULL || me == NULL || !copy_in(&ex, user_ptr, sizeof(ex)) || ex.name_len == 0 ||
        !copy_in(name, (uint32_t) ex.name, ex.name_len) ||
        !copy_string_array((uint32_t) ex.argv, ex.argv ? ex.argc : 0, args->block, sizeof(args->block), &used, args->argv) ||
        !copy_string_array((uint32_t) ex.envp, ex.envp ? ex.envc : 0, args->block, sizeof(args->block), &used, args->envp)) {
        kfree(args);
        copy_out(result_ptr, &result, sizeof(result));
        return;
    }
    name[ex.name_len] = '\0';
    struct ProcessCreateInfo info = {
        .parent_inode = ex.parent_inode,
        .name         = name,
        .name_len     = ex.name_len,
        .load_address = USER_PROGRAM_BASE,
        .max_size     = 0,
        .argc         = ex.argv ? ex.argc : 0,
        .argv         = ex.argv ? args->argv : NULL,
        .envc         = ex.envp ? ex.envc : 0,
        .envp         = ex.envp ? args->envp : NULL,
        .ppid         = me->metadata.pid,
        .cwd          = me->metadata.cwd,
        .cwd_inode    = me->metadata.cwd_inode,
        .stdin_fd     = ex.stdin_fd,
        .stdout_fd    = ex.stdout_fd,
        .foreground   = (ex.flags & SYSCALL_EXEC_FOREGROUND) != 0,
        .priority       = ex.priority,
        .require_header = (ex.flags & SYSCALL_EXEC_REQUIRE_HEADER) != 0,
    };
    uint32_t pid = 0;
    int32_t code = process_create(&info, &pid);
    result = code == PROCESS_CREATE_SUCCESS ? (int32_t) pid : -code;
    kfree(args);
    copy_out(result_ptr, &result, sizeof(result));
}

static void sys_process_kill(uint32_t pid, uint32_t ret_ptr) {
    struct ProcessControlBlock *target = process_get_by_pid(pid);
    if (target == NULL) {
        put_retcode(ret_ptr, 1);
        return;
    }
    if (pid == process_manager_state.init_pid) {
        put_retcode(ret_ptr, 2);
        return;
    }
    put_retcode(ret_ptr, 0);
    if (target == current())
        process_exit_current(PROCESS_EXIT_KILLED);
    // The target is not inside the kernel (only one process runs, and it is us): destroy now
    process_terminate(pid, PROCESS_EXIT_KILLED);
}

static void fill_process_info(struct SyscallProcessInfo *info, struct ProcessControlBlock *p) {
    memset(info, 0, sizeof(*info));
    info->pid  = p->metadata.pid;
    info->ppid = p->metadata.ppid;
    strncpy(info->name, p->metadata.name, sizeof(info->name) - 1);
    strncpy(info->cmdline, p->metadata.cmdline, sizeof(info->cmdline) - 1);
    strncpy(info->state, process_state_name(p->metadata.state, p->metadata.wait_reason), sizeof(info->state) - 1);
    strncpy(info->format, process_format_name(p->metadata.format), sizeof(info->format) - 1);
    info->priority   = p->metadata.priority;
    info->foreground = p->metadata.pid == process_manager_state.foreground_pid;
    info->detached   = p->metadata.detached;
    info->init       = p->metadata.pid == process_manager_state.init_pid;
    info->pages      = p->memory.page_frame_used_count;
    process_count_pages(p, &info->pages_resident, &info->pages_swapped);
    info->peak_pages = p->memory.peak_pages;
    info->heap_bytes = p->memory.heap_break - p->memory.heap_base;
    info->image_size = p->memory.image_size;
    info->cpu_ms     = p->metadata.cpu_ms;
    info->start_ms   = p->metadata.start_ms;
    info->switches   = p->metadata.switches;
}

static void sys_process_info(uint32_t array_ptr, uint32_t max, uint32_t count_ptr) {
    uint32_t n = 0;
    if (max > PROCESS_COUNT_MAX)
        max = PROCESS_COUNT_MAX;
    if (!user_range_is_valid((void*) array_ptr, max * sizeof(struct SyscallProcessInfo)))
        max = 0;
    struct SyscallProcessInfo info;
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        struct ProcessControlBlock *p = &_process_list[i];
        if (p->metadata.state == PROCESS_STATE_INACTIVE || p->metadata.name[0] == '\0')
            continue;
        if (n < max) {
            fill_process_info(&info, p);
            copy_out(array_ptr + n * sizeof(info), &info, sizeof(info));
        }
        n++;
    }
    copy_out(count_ptr, &n, sizeof(n));
}

static const char *region_kind_name(uint8_t kind) {
    switch (kind) {
        case PROCESS_REGION_IMAGE:  return "image";
        case PROCESS_REGION_TEXT:   return "text";
        case PROCESS_REGION_RODATA: return "rodata";
        case PROCESS_REGION_DATA:   return "data";
        case PROCESS_REGION_BSS:    return "bss";
        case PROCESS_REGION_STACK:  return "stack";
        default:                    return "other";
    }
}

static void sys_process_memory(uint32_t user_ptr, uint32_t ret_ptr) {
    struct SyscallProcessMemory *m = kmalloc(sizeof(*m));
    if (m == NULL || !copy_in(m, user_ptr, sizeof(*m))) {
        kfree(m);
        put_retcode(ret_ptr, 1);
        return;
    }
    struct ProcessControlBlock *p = m->pid == 0 ? current() : process_get_by_pid(m->pid);
    if (p == NULL) {
        kfree(m);
        put_retcode(ret_ptr, 1);
        return;
    }
    uint32_t *list_ptr = m->page_list, list_max = m->page_list_max;
    memset(m, 0, sizeof(*m));
    m->pid = p->metadata.pid;
    strncpy(m->name, p->metadata.name, sizeof(m->name) - 1);
    strncpy(m->format, process_format_name(p->metadata.format), sizeof(m->format) - 1);
    m->region_count = p->memory.region_count;
    for (uint32_t i = 0; i < p->memory.region_count && i < SYSCALL_REGION_MAX; i++) {
        m->regions[i].start = p->memory.regions[i].start;
        m->regions[i].end   = p->memory.regions[i].end;
        m->regions[i].flags = p->memory.regions[i].flags;
        strncpy(m->regions[i].kind, region_kind_name(p->memory.regions[i].kind), sizeof(m->regions[i].kind) - 1);
    }
    m->heap_base          = p->memory.heap_base;
    m->heap_break         = p->memory.heap_break;
    m->entry              = p->memory.entry;
    m->pages              = p->memory.page_frame_used_count;
    m->page_list_capacity = p->memory.page_list_capacity;
    m->page_list_address  = (uint32_t) p->memory.virtual_addr_used;
    m->peak_pages         = p->memory.peak_pages;
    process_count_pages(p, &m->pages_resident, &m->pages_swapped);
    m->page_list            = list_ptr;
    m->page_list_max        = list_max;
    uint32_t n = m->pages < list_max ? m->pages : list_max;
    if (n > 0 && p->memory.virtual_addr_used != NULL &&
        copy_out((uint32_t) list_ptr, p->memory.virtual_addr_used, n * sizeof(uint32_t)))
        m->page_list_copied = n;
    copy_out(user_ptr, m, sizeof(*m));
    kfree(m);
    put_retcode(ret_ptr, 0);
}

static void sys_wait(const struct InterruptFrame *frame, uint32_t user_ptr) {
    struct ProcessControlBlock *me = current();
    struct SyscallWait w;
    if (me == NULL || !copy_in(&w, user_ptr, sizeof(w)))
        return;
    if (me->metadata.wait_done) { // woken up by the child's exit: report it
        me->metadata.wait_done = false;
        w.pid    = me->metadata.wait_arg;
        w.status = me->metadata.wait_status;
        w.result = (int32_t) w.pid;
        copy_out(user_ptr, &w, sizeof(w));
        return;
    }
    bool child_running = false;
    for (uint32_t i = 0; i < PROCESS_COUNT_MAX; i++) {
        struct ProcessControlBlock *p = &_process_list[i];
        if (p->metadata.state == PROCESS_STATE_INACTIVE || p->metadata.ppid != me->metadata.pid)
            continue;
        if (w.pid != 0 && p->metadata.pid != w.pid)
            continue;
        if (p->metadata.detached && w.pid != 0) { // already in the background: nothing to wait for
            w.status = PROCESS_EXIT_DETACHED;
            w.result = (int32_t) p->metadata.pid;
            copy_out(user_ptr, &w, sizeof(w));
            return;
        }
        if (!p->metadata.detached)
            child_running = true;
    }
    if (child_running) {
        if (w.flags & SYSCALL_WAIT_NOHANG) {
            w.result = 0;
            copy_out(user_ptr, &w, sizeof(w));
            return;
        }
        scheduler_block_current(frame, PROCESS_WAIT_CHILD, w.pid, true);
    }
    int32_t status;
    if (w.pid != 0 && process_lookup_exit_status(w.pid, &status)) {
        w.status = status;
        w.result = (int32_t) w.pid;
    } else {
        w.result = -1;
    }
    copy_out(user_ptr, &w, sizeof(w));
}

static void sys_pipe(uint32_t fds_ptr, uint32_t ret_ptr) {
    struct ProcessControlBlock *me = current();
    int32_t fds[2] = {-1, -1};
    struct Pipe *pipe = me != NULL ? pipe_create() : NULL;
    if (pipe == NULL || !user_range_is_valid((void*) fds_ptr, sizeof(fds))) {
        if (pipe != NULL)
            pipe_close_reader(pipe); // never opened: frees it
        put_retcode(ret_ptr, 1);
        return;
    }
    fds[0] = process_fd_install(me, PROCESS_FD_PIPE_READ, pipe);
    fds[1] = fds[0] < 0 ? -1 : process_fd_install(me, PROCESS_FD_PIPE_WRITE, pipe);
    if (fds[1] < 0) {
        if (fds[0] >= 0)
            process_fd_close(me, fds[0]);
        else
            pipe_close_reader(pipe);
        put_retcode(ret_ptr, 1);
        return;
    }
    copy_out(fds_ptr, fds, sizeof(fds));
    put_retcode(ret_ptr, 0);
}

static void sys_fd_io(const struct InterruptFrame *frame, uint32_t user_ptr, bool write) {
    struct SyscallIO io;
    if (!copy_in(&io, user_ptr, sizeof(io)))
        return;
    struct ProcessFile *f = process_fd_get(current(), io.fd);
    io.result = -1;
    if (f != NULL && user_range_is_valid(io.buf, io.len)) {
        if (write && f->type == PROCESS_FD_CONSOLE) {
            print_colored(io.buf, io.len, io.color);
            io.result = (int32_t) io.len;
        } else if (write && f->type == PROCESS_FD_PIPE_WRITE) {
            int32_t n = pipe_write(f->pipe, io.buf, io.len);
            if (n == 0 && io.len > 0) // full: wait until the reader made room
                scheduler_block_current(frame, PROCESS_WAIT_PIPE_WRITE, (uint32_t) io.fd, true);
            io.result = n;
        } else if (!write && f->type == PROCESS_FD_PIPE_READ) {
            int32_t n = pipe_read(f->pipe, (void*) io.buf, io.len);
            if (n < 0) // empty while a writer still exists: wait for data or end of file
                scheduler_block_current(frame, PROCESS_WAIT_PIPE_READ, (uint32_t) io.fd, true);
            io.result = n;
        } else if (!write && f->type == PROCESS_FD_CONSOLE) {
            io.result = -2; // console input is read with getchar
        }
    }
    copy_out(user_ptr + offsetof(struct SyscallIO, result), &io.result, sizeof(io.result));
}

static void sys_sched_info(uint32_t user_ptr) {
    struct SchedulerStats st;
    scheduler_get_stats(&st);
    struct SyscallSchedInfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.algorithm, "round robin (priority time slices)", sizeof(info.algorithm) - 1);
    info.uptime_ms             = timer_get_ms();
    info.idle_ms               = st.idle_ms;
    info.busy_ms               = st.busy_ms;
    info.context_switches      = st.context_switches;
    info.preemptions           = st.preemptions;
    info.blocks                = st.blocks;
    info.slice_ms[0]           = scheduler_slice_ms(PROCESS_PRIORITY_LOW);
    info.slice_ms[1]           = scheduler_slice_ms(PROCESS_PRIORITY_NORMAL);
    info.slice_ms[2]           = scheduler_slice_ms(PROCESS_PRIORITY_HIGH);
    info.timer_hz              = TIMER_FREQUENCY_HZ;
    info.process_count         = process_manager_state.active_process_count;
    info.process_max           = PROCESS_COUNT_MAX;
    info.created_total         = process_manager_state.created_total;
    info.init_respawns         = process_manager_state.init_respawns;
    info.pipes_used            = pipe_count_used();
    info.page_directories_used = paging_page_directory_count_used();
    info.page_directories_max  = PAGING_DIRECTORY_TABLE_MAX_COUNT;
    copy_out(user_ptr, &info, sizeof(info));
}

void syscall(struct InterruptFrame frame) {
    uint32_t eax = frame.cpu.general.eax;
    uint32_t ebx = frame.cpu.general.ebx;
    uint32_t ecx = frame.cpu.general.ecx;
    uint32_t edx = frame.cpu.general.edx;
    // The int 0x30 gate cleared IF; system calls run with interrupts on (keyboard, timer)
    __asm__ volatile("sti");

    struct EXT2DriverRequest request;
    char name[SYSCALL_NAME_MAX + 1];
    switch (eax) {
        case SYS_FS_READ:
            if (!fetch_request(ebx, &request, name, BUFFER_USED)) {
                put_retcode(ecx, -1);
                break;
            }
            put_retcode(ecx, vfs_read(request));
            break;
        case SYS_FS_READ_DIRECTORY: {
            if (!fetch_request(ebx, &request, name, BUFFER_USED)) {
                put_retcode(ecx, -1);
                break;
            }
            int8_t ret = vfs_read_directory(&request);
            // buffer too small: the driver reports the size it needs in buffer_size
            copy_out(ebx + offsetof(struct EXT2DriverRequest, buffer_size), &request.buffer_size, sizeof(uint32_t));
            put_retcode(ecx, ret);
            break;
        }
        case SYS_FS_WRITE:
            if (!fetch_request(ebx, &request, name, BUFFER_UNLESS_DIR)) {
                put_retcode(ecx, -1);
                break;
            }
            put_retcode(ecx, vfs_write(&request));
            break;
        case SYS_FS_DELETE:
            if (!fetch_request(ebx, &request, name, BUFFER_IGNORED)) {
                put_retcode(ecx, -1);
                break;
            }
            put_retcode(ecx, vfs_delete(request));
            break;
        case SYS_GETCHAR:
            sys_getchar(ebx);
            break;
        case SYS_PUTCHAR: {
            // The book's table passes the character itself, its example a pointer to it:
            // accept both (a real char buffer never sits in the first 256 bytes, where the
            // program code starts; 0xFFFFFF80+ is a sign-extended char such as CP437 0xC4)
            char c = (char) ebx;
            bool is_char = ebx < 256 || ebx >= 0xFFFFFF80u;
            if (!is_char && !copy_in(&c, ebx, 1))
                break;
            print_colored(&c, 1, ecx);
            break;
        }
        case SYS_PUTS:
            if (user_range_is_valid((void*) ebx, ecx))
                print_colored((const char*) ebx, ecx, edx);
            break;
        case SYS_KEYBOARD_ACTIVATE:
            keyboard_state_activate();
            break;
        case SYS_GETCHAR_WAIT:
            sys_getchar_wait(&frame, ebx);
            break;
        case SYS_CLEAR_SCREEN:
            console_clear();
            break;
        case SYS_SET_CURSOR: {
            uint8_t top, bottom;
            console_get_region(&top, &bottom);
            console_set_cursor((uint8_t) (top + ebx), (uint8_t) ecx);
            break;
        }
        case SYS_GET_CURSOR: {
            uint8_t top, bottom, row, col;
            console_get_region(&top, &bottom);
            console_get_cursor(&row, &col);
            row -= top;
            copy_out(ebx, &row, 1);
            copy_out(ecx, &col, 1);
            break;
        }
        case SYS_SCREEN_INFO: {
            uint8_t top, bottom;
            console_get_region(&top, &bottom);
            struct SyscallScreenInfo info = {
                .rows     = (uint8_t) (bottom - top),
                .cols     = framebuffer_get_cols(),
                .graphics = framebuffer_is_graphics(),
                .width    = framebuffer_is_graphics() ? graphics_get_width() : 0,
                .height   = framebuffer_is_graphics() ? graphics_get_height() : 0,
            };
            copy_out(ebx, &info, sizeof(info));
            break;
        }
        case SYS_CONSOLE_SCROLL: {
            uint32_t offset = console_scroll_view((int32_t) ebx);
            statusbar_draw();
            if (ecx != 0)
                copy_out(ecx, &offset, sizeof(offset));
            break;
        }
        case SYS_FS_RENAME:
            sys_fs_rename(ebx, ecx);
            break;
        case SYS_FS_STAT:
            sys_fs_stat(ebx, ecx);
            break;
        case SYS_FS_FIND:
            sys_fs_find(ebx);
            break;
        case SYS_FS_NAME_OF:
            sys_fs_name_of(ebx, ecx);
            break;
        case SYS_FS_INFO: {
            struct SyscallFsInfo info;
            vfs_info(&info);
            copy_out(ebx, &info, sizeof(info));
            break;
        }
        case SYS_SBRK: {
            uint32_t old_break = 0xFFFFFFFFu;
            if (current() == NULL || !process_sbrk(current(), (int32_t) ebx, &old_break))
                old_break = 0xFFFFFFFFu;
            copy_out(ecx, &old_break, sizeof(old_break));
            break;
        }
        case SYS_MEMINFO:
            sys_meminfo(ebx);
            break;
        case SYS_TIME:
            sys_time(ebx);
            break;
        case SYS_UPTIME: {
            uint32_t ms = timer_get_ms();
            copy_out(ebx, &ms, sizeof(ms));
            break;
        }
        case SYS_SLEEP:
            sys_sleep(&frame, ebx);
            break;
        case SYS_BEEP:
            speaker_beep(ebx > 20000 ? 20000 : ebx, ecx > 5000 ? 5000 : ecx);
            break;
        case SYS_POWER:
            if (ebx == SYS_POWER_REBOOT)
                power_reboot();
            power_shutdown();
            break;
        case SYS_EXIT:
        case SYS_TERMINATE:
            process_exit_current((int32_t) ebx);
            break;
        case SYS_MOUSE:
            sys_mouse(ebx);
            break;
        case SYS_SYSINFO:
            sys_sysinfo(ebx);
            break;
        case SYS_SET_STATUS:
            if (user_string_is_valid((const char*) ebx, SYSCALL_NAME_MAX)) {
                statusbar_set_text((const char*) ebx);
                statusbar_draw();
            }
            break;
        case SYS_FS_CHECK: {
            struct SyscallFsCheck result;
            vfs_check(&result);
            copy_out(ebx, &result, sizeof(result));
            break;
        }
        case SYS_KHEAP_INFO:
            sys_kheap_info(ebx);
            break;
        case SYS_PROCESS_CREATE:
            sys_process_create(ebx, ecx);
            break;
        case SYS_PROCESS_KILL:
            sys_process_kill(ebx, ecx);
            break;
        case SYS_PROCESS_INFO:
            sys_process_info(ebx, ecx, edx);
            break;
        case SYS_WAIT:
            sys_wait(&frame, ebx);
            break;
        case SYS_YIELD:
            scheduler_yield_current(&frame);
            break;
        case SYS_GETPID: {
            struct ProcessControlBlock *me = current();
            uint32_t pid = me ? me->metadata.pid : 0, ppid = me ? me->metadata.ppid : 0;
            copy_out(ebx, &pid, sizeof(pid));
            if (ecx != 0)
                copy_out(ecx, &ppid, sizeof(ppid));
            break;
        }
        case SYS_SET_PRIORITY: {
            struct ProcessControlBlock *p = ebx == 0 ? current() : process_get_by_pid(ebx);
            if (p == NULL || ecx > PROCESS_PRIORITY_HIGH) {
                put_retcode(edx, 1);
                break;
            }
            p->metadata.priority = (uint8_t) ecx;
            put_retcode(edx, 0);
            break;
        }
        case SYS_DETACH:
            if (current() != NULL)
                process_detach(current());
            break;
        case SYS_PIPE:
            sys_pipe(ebx, ecx);
            break;
        case SYS_FD_READ:
            sys_fd_io(&frame, ebx, false);
            break;
        case SYS_FD_WRITE:
            sys_fd_io(&frame, ebx, true);
            break;
        case SYS_FD_CLOSE:
            process_fd_close(current(), (int32_t) ebx);
            break;
        case SYS_FD_INFO: {
            struct ProcessFile *f = process_fd_get(current(), (int32_t) ebx);
            uint8_t type = SYSCALL_FD_NONE;
            if (f != NULL)
                type = f->type == PROCESS_FD_CONSOLE ? SYSCALL_FD_CONSOLE
                     : f->type == PROCESS_FD_PIPE_READ ? SYSCALL_FD_PIPE_READ : SYSCALL_FD_PIPE_WRITE;
            copy_out(ecx, &type, 1);
            break;
        }
        case SYS_CHDIR: {
            struct ProcessControlBlock *me = current();
            if (me != NULL && user_string_is_valid((const char*) ebx, PROCESS_CWD_MAX - 1) && ((const char*) ebx)[0] == '/') {
                strncpy(me->metadata.cwd, (const char*) ebx, PROCESS_CWD_MAX - 1);
                me->metadata.cwd[PROCESS_CWD_MAX - 1] = '\0';
                me->metadata.cwd_inode = ecx;
            }
            break;
        }
        case SYS_GETCWD: {
            struct ProcessControlBlock *me = current();
            if (me == NULL || ecx == 0)
                break;
            uint32_t len = (uint32_t) strlen(me->metadata.cwd) + 1;
            if (len > ecx)
                len = ecx;
            char tmp[PROCESS_CWD_MAX];
            memcpy(tmp, me->metadata.cwd, len);
            tmp[len - 1] = '\0';
            copy_out(ebx, tmp, len);
            break;
        }
        case SYS_SCHED_INFO:
            sys_sched_info(ebx);
            break;
        case SYS_WIDGET_SET: {
            struct ProcessControlBlock *me = current();
            if (me != NULL && user_string_is_valid((const char*) ebx, STATUSBAR_WIDGET_TEXT_MAX)) {
                statusbar_set_widget(me->metadata.pid, (const char*) ebx, (uint8_t) ecx);
                statusbar_draw();
            }
            break;
        }
        case SYS_PROCESS_MEMORY:
            sys_process_memory(ebx, ecx);
            break;
        case SYS_WIN_CREATE:
        case SYS_WIN_PRESENT:
        case SYS_WIN_EVENT:
        case SYS_WIN_DESTROY:
        case SYS_WIN_SET_TITLE:
        case SYS_WIN_INFO:
            compositor_syscall(&frame, eax, ebx, ecx, edx);
            break;
        case SYS_NET_INFO:
        case SYS_NET_UDP_BIND:
        case SYS_NET_UDP_SEND:
        case SYS_NET_UDP_RECV:
        case SYS_NET_UDP_CLOSE:
        case SYS_NET_PING:
            net_syscall(&frame, eax, ebx, ecx, edx);
            break;
        default:
            break; // unknown service: ignored
    }
}
