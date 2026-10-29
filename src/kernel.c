#include <stdint.h>
#include <stdbool.h>
#include "header/cpu/gdt.h"
#include "header/kernel-entrypoint.h"
#include "header/text/framebuffer.h"
#include "header/text/console.h"
#include "header/cpu/interrupt.h"
#include "header/cpu/idt.h"
#include "header/driver/keyboard.h"
#include "header/driver/serial.h"
#include "header/driver/timer.h"
#include "header/driver/mouse.h"
#include "header/driver/speaker.h"
#include "header/filesystem/vfs.h"
#include "header/memory/paging.h"
#include "header/memory/kheap.h"
#include "header/kernel/boot.h"
#include "header/kernel/splash.h"
#include "header/kernel/statusbar.h"
#include "header/kernel/user.h"
#include "header/stdlib/string.h"

#ifdef COSMOS_FORCE_TEXT_MODE
#define PREFER_GRAPHICS false
#else
#define PREFER_GRAPHICS true
#endif

#define BOOT_DATA_LIMIT 0x400000u // GRUB's data is read through the boot page table (first 4 MiB)

struct BootInfo boot_info = {.root_fs = VFS_EXT2};

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static uint32_t parse_uint(const char *s) {
    uint32_t value = 0;
    while (*s >= '0' && *s <= '9')
        value = value * 10 + (uint32_t) (*s++ - '0');
    return value;
}

// Read RAM size and the GRUB menu entry options (fs=, video=, mem=) from the multiboot info
static void parse_multiboot(void) {
    if (_multiboot_magic != MULTIBOOT_BOOTLOADER_MAGIC || _multiboot_info_address >= BOOT_DATA_LIMIT)
        return;
    const struct MultibootInfo *mbi = (const struct MultibootInfo*) PHYS_TO_VIRT(_multiboot_info_address);
    if (mbi->flags & MULTIBOOT_INFO_MEMORY)
        boot_info.detected_ram_kb = mbi->mem_upper + 1024; // mem_upper counts from 1 MiB
    if (!(mbi->flags & MULTIBOOT_INFO_CMDLINE) || mbi->cmdline >= BOOT_DATA_LIMIT)
        return;

    const char *cmdline = (const char*) PHYS_TO_VIRT(mbi->cmdline);
    strncpy(boot_info.cmdline, cmdline, sizeof(boot_info.cmdline) - 1);
    for (const char *p = cmdline; *p != '\0'; p++) {
        if (p != cmdline && p[-1] != ' ')
            continue; // only look at the start of each word
        if (starts_with(p, "fs=cosmo"))
            boot_info.root_fs = VFS_COSMOFS;
        else if (starts_with(p, "fs=ext2"))
            boot_info.root_fs = VFS_EXT2;
        else if (starts_with(p, "video=text"))
            boot_info.text_mode = true;
        else if (starts_with(p, "mem="))
            boot_info.mem_limit_mb = parse_uint(p + 4);
    }
}

static uint32_t memory_for_paging_kb(void) {
    uint32_t kb = boot_info.detected_ram_kb;
    if (boot_info.mem_limit_mb >= 4 && (kb == 0 || boot_info.mem_limit_mb * 1024 < kb))
        kb = boot_info.mem_limit_mb * 1024;
    return kb;
}

static void boot_log(const char *line) {
    serial_printf("[boot] %s\n", line);
    splash_step(line, 85);
}

void kernel_setup(void) {
    // Chapter 0 & 1: CPU tables and interrupts
    load_gdt(&_gdt_gdtr);
    pic_remap();
    initialize_idt();
    serial_init();
    serial_printf("[boot] cOSmOS " COSMOS_VERSION " kernel_setup() at 0x%08x (higher half)\n", (uint32_t) &kernel_setup);

    // Chapter 3.1: paging (frame allocator, physmap) and the kernel heap
    parse_multiboot();
    paging_init(memory_for_paging_kb());
    kheap_init();
    boot_info.managed_ram_kb = page_manager_state.total_page_frame_count * (PAGE_FRAME_SIZE / 1024);
    serial_printf("[boot] cmdline \"%s\", RAM detected %u KiB, managed %u KiB (SYSTEM_MEMORY_MB=%u), %u free frames\n",
                  boot_info.cmdline, boot_info.detected_ram_kb, boot_info.managed_ram_kb, SYSTEM_MEMORY_MB,
                  page_manager_state.free_page_frame_count);

    // Display: BGA 1024x768x32 graphics when available, VGA text mode otherwise
    bool graphics = framebuffer_init(PREFER_GRAPHICS && !boot_info.text_mode);
    console_init();
    // Scrollback sized from free RAM: ~1/32 of it, between 64 and 2000 lines
    uint32_t line_bytes = framebuffer_get_cols() * 2u;
    uint32_t lines = page_manager_state.free_page_frame_count * (PAGE_FRAME_SIZE / 32) / line_bytes;
    lines = lines < 64 ? 64 : (lines > 2000 ? 2000 : lines);
    bool scrollback = console_enable_scrollback(lines);
    serial_printf("[boot] display: %s, %ux%u cells, scrollback %u lines\n", graphics ? "BGA graphics" : "VGA text",
                  framebuffer_get_cols(), framebuffer_get_rows(), scrollback ? lines : 0);

    timer_init(TIMER_FREQUENCY_HZ);
    activate_keyboard_interrupt();
    keyboard_state_activate();

    splash_begin();
    splash_step("Higher-half kernel, paging, GDT, IDT, PIC, PIT and keyboard ready", 20);

    uint16_t pointer_w, pointer_h;
    framebuffer_get_pointer_space(&pointer_w, &pointer_h);
    bool mouse = mouse_init(pointer_w, pointer_h);
    serial_printf("[boot] ps/2 mouse: %s\n", mouse ? (mouse_has_wheel() ? "wheel mouse" : "present") : "absent");
    splash_step(mouse ? "PS/2 mouse detected (IRQ12)" : "No PS/2 mouse", 40);

    // Chapter 2 + 3.2: the root file system chosen in the GRUB menu, and swap on the same disk
    splash_step(boot_info.root_fs == VFS_EXT2 ? "Mounting EXT2 root..." : "Mounting CosmoFS root...", 60);
    bool root = vfs_mount_root(boot_info.root_fs, boot_log);
    splash_step(root ? "Starting the shell in user mode - welcome aboard!" : "No root file system!", 100);
    speaker_play_startup_chime();
    splash_end(1200);
    statusbar_init();

    // Chapter 3.3: Task State Segment, then jump into the shell (ring 3)
    gdt_install_tss();
    set_tss_register();
    set_tss_kernel_current_stack();
    user_launch_shell();
}
