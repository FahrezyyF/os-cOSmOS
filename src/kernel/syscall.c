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

static void print_colored(const char *text, uint32_t count, uint32_t color) {
    uint8_t attr = (color & 0xFF) == 0 ? DEFAULT_COLOR : (uint8_t) color;
    console_write_color(text, count, attr & 0xF, (attr >> 4) & 0xF);
}

static void sys_getchar_wait(uint32_t user_ptr) {
    char c = 0;
    for (;;) {
        get_keyboard_buffer(&c);
        if (c != 0)
            break;
        kernel_idle();
        __asm__ volatile("hlt"); // woken by the next interrupt (timer: 1 ms)
    }
    copy_out(user_ptr, &c, 1);
}

static void sys_sleep(uint32_t ms) {
    uint32_t start = timer_get_ms();
    while (timer_get_ms() - start < ms) {
        kernel_idle();
        __asm__ volatile("hlt");
    }
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
        .user_break        = user_get_break(),
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
        case SYS_GETCHAR: {
            char c;
            kernel_idle();
            get_keyboard_buffer(&c);
            copy_out(ebx, &c, 1);
            break;
        }
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
            sys_getchar_wait(ebx);
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
            if (!user_sbrk((int32_t) ebx, &old_break))
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
            sys_sleep(ebx);
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
            user_program_exit((int32_t) ebx);
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
        default:
            break; // unknown service: ignored
    }
}
