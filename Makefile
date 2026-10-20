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
ISO_NAME      = OS2025
DISK_NAME     = storage
COSMO_DISK    = cosmo
DISK_SIZE     = 4M

# Flags
WARNING_CFLAG = -Wall -Wextra -Werror
DEBUG_CFLAG   = -fshort-wchar -g
STRIP_CFLAG   = -nostdlib -fno-stack-protector -nostartfiles -nodefaultlibs -ffreestanding
CFLAGS        = $(DEBUG_CFLAG) $(WARNING_CFLAG) $(STRIP_CFLAG) -m32 -c -I$(SOURCE_FOLDER) -MMD -MP $(KERNEL_DEFINES)
AFLAGS        = -f elf32 -g -F dwarf
LFLAGS        = -T $(SOURCE_FOLDER)/linker.ld -melf_i386 -z noexecstack
HOST_CFLAGS   = -std=gnu11 -g -O1 -Wall -Wextra -Werror -I$(SOURCE_FOLDER)
SANITIZE      = -fsanitize=address,undefined -fno-omit-frame-pointer

# `make run DISPLAY_MODE=text` forces the 80x25 VGA text console (run `make clean` first)
ifeq ($(DISPLAY_MODE),text)
KERNEL_DEFINES += -DCOSMOS_FORCE_TEXT_MODE
endif

# Kernel sources: everything under src/ except host-side programs in src/external/
KERNEL_C_SOURCES   := $(shell find $(SOURCE_FOLDER) -name '*.c' -not -path '$(SOURCE_FOLDER)/external/*' | sort)
KERNEL_ASM_SOURCES := $(shell find $(SOURCE_FOLDER) -name '*.s' | sort)
KERNEL_OBJECTS     := $(patsubst $(SOURCE_FOLDER)/%.c,$(OBJ_FOLDER)/%.o,$(KERNEL_C_SOURCES)) \
                      $(patsubst $(SOURCE_FOLDER)/%.s,$(OBJ_FOLDER)/%.s.o,$(KERNEL_ASM_SOURCES))

# QEMU: storage.bin (EXT2) = primary master, cosmo.bin (CosmoFS) = primary slave
QEMU_DRIVES   = -drive file=$(OUTPUT_FOLDER)/$(DISK_NAME).bin,format=raw,if=ide,index=0,media=disk \
                -drive file=$(OUTPUT_FOLDER)/$(COSMO_DISK).bin,format=raw,if=ide,index=1,media=disk
QEMU_FLAGS    = -vga std $(QEMU_DRIVES) -cdrom $(OUTPUT_FOLDER)/$(ISO_NAME).iso -serial file:$(OUTPUT_FOLDER)/serial.log
QEMU_SOUND    = -audiodev pa,id=snd0 -machine pcspk-audiodev=snd0

.PHONY: all build run run-sound debug clean kernel iso disk disks inserter tools test

all: build
build: iso

run: build disks
	@$(QEMU) $(QEMU_FLAGS)

# Same as run, with the PC speaker routed to PulseAudio (WSLg / Linux desktop)
run-sound: build disks
	@$(QEMU) $(QEMU_FLAGS) $(QEMU_SOUND)

# Wait for gdb on localhost:1234 (what the VS Code "Kernel" launch config does)
debug: build disks
	@$(QEMU) -s -S $(QEMU_FLAGS)

clean:
	rm -rf $(OBJ_FOLDER) $(OUTPUT_FOLDER)/*.o $(OUTPUT_FOLDER)/$(ISO_NAME).iso $(OUTPUT_FOLDER)/kernel $(OUTPUT_FOLDER)/iso
	rm -f $(OUTPUT_FOLDER)/inserter $(OUTPUT_FOLDER)/cosmofs-tool $(OUTPUT_FOLDER)/test-*

kernel: $(OUTPUT_FOLDER)/kernel

$(OUTPUT_FOLDER)/kernel: $(KERNEL_OBJECTS) $(SOURCE_FOLDER)/linker.ld
	@$(LIN) $(LFLAGS) $(KERNEL_OBJECTS) -o $@
	@echo Linking object files and generate elf32...

$(OBJ_FOLDER)/%.o: $(SOURCE_FOLDER)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $< -o $@

$(OBJ_FOLDER)/%.s.o: $(SOURCE_FOLDER)/%.s
	@mkdir -p $(dir $@)
	@$(ASM) $(AFLAGS) $< -o $@

iso: kernel
	@mkdir -p $(OUTPUT_FOLDER)/iso/boot/grub
	@cp $(OUTPUT_FOLDER)/kernel     $(OUTPUT_FOLDER)/iso/boot/
	@cp other/grub1                 $(OUTPUT_FOLDER)/iso/boot/grub/
	@cp $(SOURCE_FOLDER)/menu.lst   $(OUTPUT_FOLDER)/iso/boot/grub/
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

# Disk images are only created when missing, so `make run` never wipes data.
disks: $(OUTPUT_FOLDER)/$(DISK_NAME).bin $(OUTPUT_FOLDER)/$(COSMO_DISK).bin

$(OUTPUT_FOLDER)/%.bin:
	@qemu-img create -f raw $@ $(DISK_SIZE) >/dev/null

# Recreate (wipe) both disk images; the kernel formats them on next boot
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

tools: inserter

# Host unit/integration tests, run with AddressSanitizer + UBSan
TEST_PROGRAMS = $(OUTPUT_FOLDER)/test-string $(OUTPUT_FOLDER)/test-format $(OUTPUT_FOLDER)/test-ext2 $(OUTPUT_FOLDER)/test-cosmofs

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

$(OUTPUT_FOLDER)/test-ext2: $(TEST_FOLDER)/test_ext2.c $(TEST_FOLDER)/ramdisk.c $(SOURCE_FOLDER)/filesystem/ext2.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) -Dread=ext2_read -Dwrite=ext2_write -Ddelete=ext2_delete $^ -o $@

$(OUTPUT_FOLDER)/test-cosmofs: $(TEST_FOLDER)/test_cosmofs.c $(SOURCE_FOLDER)/filesystem/cosmofs.c
	@$(HOSTCC) $(HOST_CFLAGS) $(SANITIZE) $^ -o $@

test: $(TEST_PROGRAMS)
	@for t in $(TEST_PROGRAMS); do echo "== $$t"; ./$$t || exit 1; done
	@echo "All host tests passed."

-include $(KERNEL_OBJECTS:.o=.d)
