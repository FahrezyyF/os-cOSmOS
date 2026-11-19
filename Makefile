# Compiler & linker
ASM           = nasm
LIN           = ld
CC            = gcc
HOSTCC        = gcc
QEMU          = qemu-system-i386

# Directory
SOURCE_FOLDER = src
OUTPUT_FOLDER = bin
OBJ_FOLDER    = $(OUTPUT_FOLDER)/obj
TEST_FOLDER   = test
ROOTFS_FOLDER = rootfs
ISO_NAME      = OS2025
DISK_NAME     = storage
COSMO_DISK    = cosmo
# 4 MiB file system region + 8 MiB swap partition (book 3.2)
DISK_SIZE     = 12M
DISK_BYTES    = 12582912

# Boot options (see README "Cara Run"):
#   make run FS=cosmo          boot the CosmoFS root (default: EXT2)
#   make run GRUB_TIMEOUT=0    skip the GRUB menu
#   make run SYSTEM_MEMORY_MB=6, make run QEMU_MEMORY=8M   test paging + swap with little RAM
#   make run DISPLAY_MODE=text 80x25 VGA text console
FS            ?= ext2
GRUB_TIMEOUT  ?= 3
QEMU_MEMORY   ?= 128M

# Flags
WARNING_CFLAG = -Wall -Wextra -Werror
DEBUG_CFLAG   = -fshort-wchar -g
STRIP_CFLAG   = -nostdlib -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding
CFLAGS        = $(DEBUG_CFLAG) $(WARNING_CFLAG) $(STRIP_CFLAG) -m32 -c -I$(SOURCE_FOLDER) -MMD -MP $(KERNEL_DEFINES)
USER_CFLAGS   = $(DEBUG_CFLAG) $(WARNING_CFLAG) $(STRIP_CFLAG) -m32 -c -I$(SOURCE_FOLDER) -MMD -MP -fno-pie -O2 \
                -ffunction-sections -fdata-sections
AFLAGS        = -f elf32 -g -F dwarf
LFLAGS        = -T $(SOURCE_FOLDER)/linker.ld -melf_i386 -z noexecstack
HOST_CFLAGS   = -std=gnu11 -g -O1 -Wall -Wextra -Werror -I$(SOURCE_FOLDER)
SANITIZE      = -fsanitize=address,undefined -fno-omit-frame-pointer

ifeq ($(DISPLAY_MODE),text)
KERNEL_DEFINES += -DCOSMOS_FORCE_TEXT_MODE
endif
ifdef SYSTEM_MEMORY_MB
KERNEL_DEFINES += -DSYSTEM_MEMORY_MB=$(SYSTEM_MEMORY_MB)
endif

# GRUB menu entry booted by default (src/menu.lst): 0 = EXT2 root, 1 = CosmoFS root
ifeq ($(FS),cosmo)
GRUB_DEFAULT  = 1
else
GRUB_DEFAULT  = 0
endif

# Kernel sources: everything under src/ except host programs (src/external) and user programs (src/user)
KERNEL_C_SOURCES   := $(shell find $(SOURCE_FOLDER) -name '*.c' -not -path '$(SOURCE_FOLDER)/external/*' -not -path '$(SOURCE_FOLDER)/user/*' | sort)
KERNEL_ASM_SOURCES := $(shell find $(SOURCE_FOLDER) -name '*.s' -not -path '$(SOURCE_FOLDER)/user/*' | sort)
KERNEL_OBJECTS     := $(patsubst $(SOURCE_FOLDER)/%.c,$(OBJ_FOLDER)/%.o,$(KERNEL_C_SOURCES)) \
                      $(patsubst $(SOURCE_FOLDER)/%.s,$(OBJ_FOLDER)/%.s.o,$(KERNEL_ASM_SOURCES))

# User programs (book 3.3.3 / 4.3): crt0 first, the C library pieces shared with the kernel,
# and per program its own objects. --gc-sections keeps only the code a program uses.
USER_OBJ_FOLDER    = $(OBJ_FOLDER)/user
STAGE_FOLDER       = $(OUTPUT_FOLDER)/rootfs
USER_CRT0          = $(USER_OBJ_FOLDER)/user/crt0.s.o
USER_LIB_SOURCES   = $(SOURCE_FOLDER)/user/ulib.c $(SOURCE_FOLDER)/user/regex.c $(SOURCE_FOLDER)/user/gfx.c $(SOURCE_FOLDER)/text/font8x16.c \
                     $(SOURCE_FOLDER)/stdlib/string.c $(SOURCE_FOLDER)/stdlib/format.c $(SOURCE_FOLDER)/stdlib/heap.c
USER_SHLIB_SOURCES = $(SOURCE_FOLDER)/user/shlib.c $(SOURCE_FOLDER)/user/cmd_fs.c $(SOURCE_FOLDER)/user/cmd_text.c \
                     $(SOURCE_FOLDER)/user/cmd_sys.c $(SOURCE_FOLDER)/user/cmd_proc.c
SHELL_SOURCES      = $(SOURCE_FOLDER)/user/shell.c $(SOURCE_FOLDER)/user/cmd_table.c
USER_LIB           = $(patsubst $(SOURCE_FOLDER)/%.c,$(USER_OBJ_FOLDER)/%.o,$(USER_LIB_SOURCES))
USER_SHLIB         = $(patsubst $(SOURCE_FOLDER)/%.c,$(USER_OBJ_FOLDER)/%.o,$(USER_SHLIB_SOURCES))
SHELL_OBJECTS      = $(patsubst $(SOURCE_FOLDER)/%.c,$(USER_OBJ_FOLDER)/%.o,$(SHELL_SOURCES))
USER_LDFLAGS       = -melf_i386 -z noexecstack -z max-page-size=0x1000 --gc-sections --no-warn-rwx-segments
USER_OBJECTS       = $(USER_CRT0) $(USER_LIB) $(USER_SHLIB) $(SHELL_OBJECTS)
ELF2CEX            = $(OUTPUT_FOLDER)/elf2cex

# Separate shell commands, installed as ELF executables in /bin (book 4.3.6)
BIN_COMMANDS = ls pwd mkdir touch cat cp rm mv find grep tree stat echo write head tail wc sort hexdump \
               ps kill nice pmap sched execinfo date uptime sleep beep free heap memtest sysinfo df fsck \
               crash reboot shutdown
# Applications, installed as CEX executables in /apps (book 4.3.3 / 4.3.4); snake lives in
# /games, which is NOT in the default PATH (the PATH bonus demo adds it)
APPS         = hello clock counter top paint bounce gclock ifconfig ping nslookup udpsend udpecho vplay
GAMES        = snake
BIN_TARGETS  = $(addprefix $(STAGE_FOLDER)/bin/,$(BIN_COMMANDS))
APP_TARGETS  = $(addprefix $(STAGE_FOLDER)/apps/,$(APPS)) $(STAGE_FOLDER)/apps/hello-flat
GAME_TARGETS = $(addprefix $(STAGE_FOLDER)/games/,$(GAMES))
# /media: CVID videos for vplay (a generated demo clip; `make video VIDEO=file.mp4` adds yours)
MEDIA_TARGETS = $(STAGE_FOLDER)/media/demo.cvid
USER_PROGRAMS = $(OUTPUT_FOLDER)/shell $(BIN_TARGETS) $(APP_TARGETS) $(GAME_TARGETS) $(MEDIA_TARGETS)

# QEMU: storage.bin (EXT2) = primary master, cosmo.bin (CosmoFS) = primary slave
QEMU_DRIVES   = -drive file=$(OUTPUT_FOLDER)/$(DISK_NAME).bin,format=raw,if=ide,index=0,media=disk \
                -drive file=$(OUTPUT_FOLDER)/$(COSMO_DISK).bin,format=raw,if=ide,index=1,media=disk
# Network card for the UDP stack: QEMU user network (10.0.2.0/24), host UDP port 5555 -> guest 5555
QEMU_NET      = -netdev user,id=net0,hostfwd=udp::5555-:5555 -device rtl8139,netdev=net0
QEMU_FLAGS    = -m $(QEMU_MEMORY) -vga std $(QEMU_DRIVES) -cdrom $(OUTPUT_FOLDER)/$(ISO_NAME).iso -serial file:$(OUTPUT_FOLDER)/serial.log \
                $(QEMU_NET)
QEMU_SOUND    = -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

.PHONY: all build run run-sound debug clean kernel iso disk disks inserter cosmofs-tool tools test \
        user-shell user-programs insert-shell insert-samples video FORCE

all: build
build: iso

run: build insert-shell
	@$(QEMU) $(QEMU_FLAGS)

# Same as run, with the PC speaker routed to PulseAudio (WSLg / Linux desktop)
run-sound: build insert-shell
	@$(QEMU) $(QEMU_FLAGS) $(QEMU_SOUND)

# Wait for gdb on localhost:1234 (what the VS Code "Kernel" launch config does)
debug: build insert-shell
	@$(QEMU) -s -S $(QEMU_FLAGS)

clean:
	rm -rf $(OBJ_FOLDER) $(OUTPUT_FOLDER)/*.o $(OUTPUT_FOLDER)/$(ISO_NAME).iso $(OUTPUT_FOLDER)/kernel $(OUTPUT_FOLDER)/iso
	rm -f $(OUTPUT_FOLDER)/inserter $(OUTPUT_FOLDER)/cosmofs-tool $(OUTPUT_FOLDER)/test-* $(OUTPUT_FOLDER)/shell $(OUTPUT_FOLDER)/shell_elf
	rm -f -r $(STAGE_FOLDER) $(ELF2CEX)

kernel: $(OUTPUT_FOLDER)/kernel

# Rebuild everything when DISPLAY_MODE / SYSTEM_MEMORY_MB change
DEFINES_STAMP = $(OBJ_FOLDER)/.defines
$(DEFINES_STAMP): FORCE
	@mkdir -p $(OBJ_FOLDER)
	@echo '$(KERNEL_DEFINES)' | cmp -s - $@ || echo '$(KERNEL_DEFINES)' > $@

$(OUTPUT_FOLDER)/kernel: $(KERNEL_OBJECTS) $(SOURCE_FOLDER)/linker.ld
	@$(LIN) $(LFLAGS) $(KERNEL_OBJECTS) -o $@
	@echo Linking object files and generate elf32...

$(OBJ_FOLDER)/%.o: $(SOURCE_FOLDER)/%.c $(DEFINES_STAMP)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $< -o $@

$(OBJ_FOLDER)/%.s.o: $(SOURCE_FOLDER)/%.s
	@mkdir -p $(dir $@)
	@$(ASM) $(AFLAGS) $< -o $@

iso: kernel
	@mkdir -p $(OUTPUT_FOLDER)/iso/boot/grub
	@cp $(OUTPUT_FOLDER)/kernel     $(OUTPUT_FOLDER)/iso/boot/
	@cp other/grub1                 $(OUTPUT_FOLDER)/iso/boot/grub/
	@sed -e 's/^default .*/default $(GRUB_DEFAULT)/' -e 's/^timeout .*/timeout $(GRUB_TIMEOUT)/' \
		$(SOURCE_FOLDER)/menu.lst > $(OUTPUT_FOLDER)/iso/boot/grub/menu.lst
	@cd $(OUTPUT_FOLDER) && genisoimage -R \
		-b boot/grub/grub1   \
		-no-emul-boot        \
		-boot-load-size 4    \
		-A os                \
		-input-charset utf8  \
		-quiet               \
		-boot-info-table     \
		-o $(ISO_NAME).iso   \
		iso
	@rm -r $(OUTPUT_FOLDER)/iso/

# Disk images are only created when missing, so `make run` never wipes data. Older 4 MiB
# images (Milestone 2) are grown in place: the file system keeps its data, swap gets the rest.
disks:
	@for f in $(OUTPUT_FOLDER)/$(DISK_NAME).bin $(OUTPUT_FOLDER)/$(COSMO_DISK).bin; do \
		if [ ! -f $$f ]; then \
			qemu-img create -f raw $$f $(DISK_SIZE) > /dev/null; \
		elif [ $$(stat -c %s $$f) -lt $(DISK_BYTES) ]; then \
			truncate -s $(DISK_SIZE) $$f && echo "$$f grown to $(DISK_SIZE) (swap partition added, data kept)"; \
		fi; \
	done

# Recreate (wipe) both disk images; `make insert-shell` / the kernel format them again
disk:
	@qemu-img create -f raw $(OUTPUT_FOLDER)/$(DISK_NAME).bin $(DISK_SIZE)
	@qemu-img create -f raw $(OUTPUT_FOLDER)/$(COSMO_DISK).bin $(DISK_SIZE)

# Host-side tools (book: 3.3.1 External Program: Inserter)
inserter:
	@$(HOSTCC) -Wno-builtin-declaration-mismatch -g -I$(SOURCE_FOLDER) \
		$(SOURCE_FOLDER)/stdlib/string.c \
		$(SOURCE_FOLDER)/filesystem/ext2.c \
		$(SOURCE_FOLDER)/external/external-inserter.c \
		-o $(OUTPUT_FOLDER)/inserter

# Same command line as the inserter, for CosmoFS images
cosmofs-tool:
	@$(HOSTCC) $(HOST_CFLAGS) \
		$(SOURCE_FOLDER)/filesystem/cosmofs.c \
		$(SOURCE_FOLDER)/external/external-cosmofs-tool.c \
		-o $(OUTPUT_FOLDER)/cosmofs-tool

tools: inserter cosmofs-tool $(ELF2CEX)

# elf2cex: ELF32 -> CEX converter (bonus: simple executable file format)
$(ELF2CEX): $(SOURCE_FOLDER)/external/elf2cex.c $(SOURCE_FOLDER)/header/process/cex.h $(SOURCE_FOLDER)/header/process/elf.h
	@$(HOSTCC) $(HOST_CFLAGS) $(SOURCE_FOLDER)/external/elf2cex.c -o $@

# The shell: `shell_elf` keeps the symbols for gdb (book 3.4.2.1), `shell` is its CEX version
user-shell: $(OUTPUT_FOLDER)/shell
user-programs: $(USER_PROGRAMS)

$(OUTPUT_FOLDER)/shell_elf: $(USER_CRT0) $(SHELL_OBJECTS) $(USER_SHLIB) $(USER_LIB) $(SOURCE_FOLDER)/user-linker.ld
	@$(LIN) -T $(SOURCE_FOLDER)/user-linker.ld $(USER_LDFLAGS) $(USER_CRT0) $(SHELL_OBJECTS) $(USER_SHLIB) $(USER_LIB) -o $@
	@echo Linking the shell: ELF32 with symbols for debugging...

$(OUTPUT_FOLDER)/shell: $(OUTPUT_FOLDER)/shell_elf $(ELF2CEX)
	@$(ELF2CEX) $< $@ shell > /dev/null
	@echo "Converting the shell to CEX ($$(stat -c %s $@) bytes)..."

# /bin/<command>: command.c compiled with -DCOMMAND_FUNCTION=cmd_<command>, stripped ELF
$(USER_OBJ_FOLDER)/bin/%.o: $(SOURCE_FOLDER)/user/bin/command.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -DCOMMAND_FUNCTION=cmd_$* $< -o $@

$(STAGE_FOLDER)/bin/%: $(USER_OBJ_FOLDER)/bin/%.o $(USER_CRT0) $(USER_SHLIB) $(USER_LIB) $(SOURCE_FOLDER)/user-linker.ld
	@mkdir -p $(dir $@)
	@$(LIN) -T $(SOURCE_FOLDER)/user-linker.ld $(USER_LDFLAGS) -n -s $(USER_CRT0) $< $(USER_SHLIB) $(USER_LIB) -o $@

# /apps/<app> and /games/<game>: ELF linked, then converted to CEX
$(USER_OBJ_FOLDER)/elf/%.elf: $(USER_OBJ_FOLDER)/user/apps/%.o $(USER_CRT0) $(USER_LIB) $(SOURCE_FOLDER)/user-linker.ld
	@mkdir -p $(dir $@)
	@$(LIN) -T $(SOURCE_FOLDER)/user-linker.ld $(USER_LDFLAGS) $(USER_CRT0) $< $(USER_LIB) -o $@

$(STAGE_FOLDER)/apps/%: $(USER_OBJ_FOLDER)/elf/%.elf $(ELF2CEX)
	@mkdir -p $(dir $@)
	@$(ELF2CEX) $< $@ $* > /dev/null

$(STAGE_FOLDER)/games/%: $(USER_OBJ_FOLDER)/elf/%.elf $(ELF2CEX)
	@mkdir -p $(dir $@)
	@$(ELF2CEX) $< $@ $* > /dev/null

# The same hello as a flat binary (third format of the loader, book chapter 3 style)
$(STAGE_FOLDER)/apps/hello-flat: $(USER_OBJ_FOLDER)/user/apps/hello.o $(USER_CRT0) $(USER_LIB) $(SOURCE_FOLDER)/user-linker-flat.ld
	@mkdir -p $(dir $@)
	@$(LIN) -T $(SOURCE_FOLDER)/user-linker-flat.ld -melf_i386 -z noexecstack --gc-sections --oformat=binary \
		$(USER_CRT0) $< $(USER_LIB) -o $@

$(STAGE_FOLDER)/media/demo.cvid: tools/video2cvid.py
	@mkdir -p $(dir $@)
	@python3 tools/video2cvid.py --demo $@ > /dev/null

# Bonus "Bad Apple!!": convert a video you provide (needs ffmpeg on the host), then
# `make insert-shell` / `make run` copies it to /media; play it with `vplay /media/<name>.cvid`
#   make video VIDEO=~/bad_apple.mp4 [VIDEO_NAME=badapple] [VIDEO_SIZE=160x120] [VIDEO_FPS=30]
VIDEO_NAME ?= badapple
VIDEO_SIZE ?= 160x120
VIDEO_FPS  ?= 30
video:
	@test -n "$(VIDEO)" || (echo "usage: make video VIDEO=path/to/video.mp4" && false)
	@mkdir -p $(STAGE_FOLDER)/media
	@python3 tools/video2cvid.py "$(VIDEO)" $(STAGE_FOLDER)/media/$(VIDEO_NAME).cvid --size $(VIDEO_SIZE) --fps $(VIDEO_FPS)

$(USER_OBJ_FOLDER)/%.o: $(SOURCE_FOLDER)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) $< -o $@

$(USER_OBJ_FOLDER)/%.s.o: $(SOURCE_FOLDER)/%.s
	@mkdir -p $(dir $@)
	@$(ASM) $(AFLAGS) $< -o $@

# Sample files and shell scripts from rootfs/ (-k: entries that already exist are kept, so
# changes made inside the OS survive the next `make run`)
define insert_samples
	@for f in $(wildcard $(ROOTFS_FOLDER)/*); do \
		./$(OUTPUT_FOLDER)/inserter -k -r $$f 1 $(OUTPUT_FOLDER)/$(DISK_NAME).bin > /dev/null || exit 1; \
		./$(OUTPUT_FOLDER)/cosmofs-tool -k -r $$f 1 $(OUTPUT_FOLDER)/$(COSMO_DISK).bin > /dev/null || exit 1; \
	done
endef

# Root inode / node of both file systems is 1. The programs go into both images, so either
# root file system can be picked in the GRUB menu. -f replaces older versions of a program.
insert-shell: inserter cosmofs-tool user-programs disks
	@echo Inserting the shell, /bin, /apps, /games and /media...
	@cd $(OUTPUT_FOLDER); ./inserter -f shell 1 $(DISK_NAME).bin > /dev/null
	@cd $(OUTPUT_FOLDER); ./cosmofs-tool -f shell 1 $(COSMO_DISK).bin > /dev/null
	@for d in bin apps games media; do \
		./$(OUTPUT_FOLDER)/inserter -f -r $(STAGE_FOLDER)/$$d 1 $(OUTPUT_FOLDER)/$(DISK_NAME).bin > /dev/null || exit 1; \
		./$(OUTPUT_FOLDER)/cosmofs-tool -f -r $(STAGE_FOLDER)/$$d 1 $(OUTPUT_FOLDER)/$(COSMO_DISK).bin > /dev/null || exit 1; \
	done
	$(insert_samples)

insert-samples: inserter cosmofs-tool disks
	$(insert_samples)

# Host unit/integration tests, run with AddressSanitizer + UBSan
TEST_PROGRAMS = $(OUTPUT_FOLDER)/test-string $(OUTPUT_FOLDER)/test-format $(OUTPUT_FOLDER)/test-heap \
                $(OUTPUT_FOLDER)/test-regex $(OUTPUT_FOLDER)/test-ext2 $(OUTPUT_FOLDER)/test-cosmofs \
                $(OUTPUT_FOLDER)/test-pipe $(OUTPUT_FOLDER)/test-cex

# The kernel's string.c / format.c are compiled with k_ prefixed symbols so the tests can
# compare them against glibc without symbol clashes
STRING_RENAMES = $(foreach f,memset memcpy memcmp memmove strlen strnlen strcmp strncmp strchr strrchr strncpy,-D$(f)=k_$(f))
FORMAT_RENAMES = -Dvsnprintf=k_vsnprintf -Dsnprintf=k_snprintf

$(OUTPUT_FOLDER)/test-string: $(TEST_FOLDER)/test_string.c $(SOURCE_FOLDER)/stdlib/string.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) -fno-builtin $(STRING_RENAMES) -c $(SOURCE_FOLDER)/stdlib/string.c -o $@-impl.o
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $(TEST_FOLDER)/test_string.c $@-impl.o -o $@

$(OUTPUT_FOLDER)/test-format: $(TEST_FOLDER)/test_format.c $(SOURCE_FOLDER)/stdlib/format.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $(FORMAT_RENAMES) -c $(SOURCE_FOLDER)/stdlib/format.c -o $@-impl.o
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) -Wno-format $(TEST_FOLDER)/test_format.c $@-impl.o -o $@

$(OUTPUT_FOLDER)/test-heap: $(TEST_FOLDER)/test_heap.c $(SOURCE_FOLDER)/stdlib/heap.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $^ -o $@

$(OUTPUT_FOLDER)/test-regex: $(TEST_FOLDER)/test_regex.c $(SOURCE_FOLDER)/user/regex.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $^ -o $@

$(OUTPUT_FOLDER)/test-ext2: $(TEST_FOLDER)/test_ext2.c $(TEST_FOLDER)/ramdisk.c $(SOURCE_FOLDER)/filesystem/ext2.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) -Dread=ext2_read -Dwrite=ext2_write -Ddelete=ext2_delete $^ -o $@

$(OUTPUT_FOLDER)/test-cosmofs: $(TEST_FOLDER)/test_cosmofs.c $(SOURCE_FOLDER)/filesystem/cosmofs.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $^ -o $@

$(OUTPUT_FOLDER)/test-pipe: $(TEST_FOLDER)/test_pipe.c $(SOURCE_FOLDER)/process/pipe.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $^ -o $@

$(OUTPUT_FOLDER)/test-cex: $(TEST_FOLDER)/test_cex.c $(SOURCE_FOLDER)/process/exec.c $(OUTPUT_FOLDER)/shell
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $(TEST_FOLDER)/test_cex.c $(SOURCE_FOLDER)/process/exec.c -o $@

test: $(TEST_PROGRAMS)
	@for t in $(TEST_PROGRAMS); do echo "== $$t"; ./$$t || exit 1; done
	@echo "All host tests passed."

FORCE:

-include $(KERNEL_OBJECTS:.o=.d) $(USER_OBJECTS:.o=.d)
