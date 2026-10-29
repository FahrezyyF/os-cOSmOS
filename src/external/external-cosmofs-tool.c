// External program: CosmoFS inserter, runs on the host OS (counterpart of external-inserter.c).
// Puts files / folders into a CosmoFS image with the very same cosmofs.c as the kernel; the
// block device is the image file loaded in host memory. A blank image is formatted first,
// exactly like the kernel does on boot.
//
// usage: cosmofs-tool [-f] [-k] [-r] <file or folder> <parent node> <cosmo.bin>
//        cosmofs-tool -l <node> <cosmo.bin>       (list a directory)
//   -f  replace the entry when the name already exists
//   -k  keep entries that already exist (skip them without an error)
//   -r  insert a folder recursively (default: only create the empty folder)
// Only the 4 MiB file system region at the start of the image is touched (the rest is swap).
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include "header/filesystem/cosmofs.h"

#define FS_REGION_BLOCKS 8192u                                   // vfs.h: VFS_FS_REGION_BLOCKS
#define IMAGE_SIZE       (FS_REGION_BLOCKS * COSMOFS_BLOCK_SIZE) // 4 MiB

static uint8_t      *image;
static uint8_t      *file_buffer;
static uint8_t      *read_buffer;
static struct CosmoFS fs;
static bool is_replace   = false;
static bool is_keep      = false;
static bool is_recursive = false;

static bool image_read(void *ctx, uint32_t lba, uint32_t count, void *buf) {
    (void) ctx;
    memcpy(buf, image + (size_t) lba * COSMOFS_BLOCK_SIZE, (size_t) count * COSMOFS_BLOCK_SIZE);
    return true;
}

static bool image_write(void *ctx, uint32_t lba, uint32_t count, const void *buf) {
    (void) ctx;
    memcpy(image + (size_t) lba * COSMOFS_BLOCK_SIZE, buf, (size_t) count * COSMOFS_BLOCK_SIZE);
    return true;
}

static bool image_flush(void *ctx) {
    (void) ctx;
    return true;
}

static const struct CosmoFSDevice device = {
    .context = NULL, .block_count = FS_REGION_BLOCKS, .read = image_read, .write = image_write, .flush = image_flush,
};

static uint32_t host_time(void) {
    return (uint32_t) time(NULL);
}

static const char *base_name(const char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;
    const char *slash = NULL;
    for (size_t i = 0; i < len; i++)
        if (path[i] == '/')
            slash = path + i;
    return slash ? slash + 1 : path;
}

static const char *write_message(int8_t code) {
    switch (code) {
        case 0:  return "Write success";
        case 1:  return "Error: File/folder name already exist";
        case 2:  return "Error: Invalid parent node";
        default: return "Error: Unknown error (invalid name or storage full)";
    }
}

static int insert_path(const char *path, uint32_t parent);

static int insert_file(const char *path, const char *name, uint8_t name_len, uint32_t parent) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cosmofs-tool: cannot open input file '%s'\n", path);
        return 1;
    }
    size_t filesize = fread(file_buffer, 1, IMAGE_SIZE, f);
    fclose(f);
    printf("Filename : %.*s (%zu bytes)\n", name_len, name, filesize);

    struct CosmoFSRequest request = {
        .buf = file_buffer, .name = name, .name_len = name_len, .parent = parent,
        .buffer_size = (uint32_t) filesize, .is_directory = false,
    };
    int8_t ret = cosmofs_write(&fs, &request);
    if (ret == 1 && is_keep && !is_replace) {
        puts("Kept existing entry");
        return 0;
    }
    if (ret == 1 && is_replace) {
        ret = cosmofs_delete(&fs, &request);
        if (ret == 0)
            ret = cosmofs_write(&fs, &request);
    }
    puts(write_message(ret));
    if (ret != 0)
        return 1;

    // Verify the bytes that landed in the image
    struct CosmoFSRequest check = request;
    check.buf = read_buffer;
    check.buffer_size = IMAGE_SIZE;
    if (cosmofs_read(&fs, &check) != 0 || memcmp(read_buffer, file_buffer, filesize) != 0) {
        fprintf(stderr, "cosmofs-tool: read-back verification FAILED for %s\n", path);
        return 1;
    }
    return 0;
}

static int insert_directory(const char *path, const char *name, uint8_t name_len, uint32_t parent) {
    struct CosmoFSRequest request = {.name = name, .name_len = name_len, .parent = parent, .is_directory = true};
    int8_t ret = cosmofs_write(&fs, &request);
    printf("Folder   : %.*s -> %s\n", name_len, name, write_message(ret));
    if (ret != 0 && !(ret == 1 && (is_recursive || is_keep)))
        return 1;
    if (!is_recursive)
        return 0;

    struct CosmoFSStat st;
    if (cosmofs_lookup(&fs, parent, name, name_len, &st) != 0 || st.type != COSMOFS_TYPE_DIR)
        return 1;
    DIR *dir = opendir(path);
    if (dir == NULL)
        return 1;
    int failures = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        failures += insert_path(child, st.node);
    }
    closedir(dir);
    return failures;
}

static int insert_path(const char *path, uint32_t parent) {
    const char *name = base_name(path);
    size_t name_len = strcspn(name, "/");
    if (name_len == 0 || name_len > COSMOFS_NAME_MAX) {
        fprintf(stderr, "cosmofs-tool: invalid name for '%s'\n", path);
        return 1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "cosmofs-tool: cannot stat '%s'\n", path);
        return 1;
    }
    if (S_ISDIR(st.st_mode))
        return insert_directory(path, name, (uint8_t) name_len, parent);
    return insert_file(path, name, (uint8_t) name_len, parent);
}

static void list_directory(uint32_t node) {
    struct CosmoFSRequest request = {
        .buf = read_buffer, .name = ".", .name_len = 1, .parent = node, .buffer_size = IMAGE_SIZE,
    };
    int8_t ret = cosmofs_read_directory(&fs, &request);
    if (ret != 0) {
        printf("read_directory failed: %d\n", ret);
        return;
    }
    struct CosmoFSDirEntry *entries = (struct CosmoFSDirEntry*) read_buffer;
    uint32_t count = request.buffer_size / sizeof(struct CosmoFSDirEntry);
    for (uint32_t i = 0; i < count; i++)
        printf("%6u  %c %9u  %s\n", entries[i].node, entries[i].type == COSMOFS_TYPE_DIR ? 'd' : '-',
               entries[i].size, entries[i].name);
}

int main(int argc, char *argv[]) {
    int argi = 1;
    bool list_mode = false;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (strcmp(argv[argi], "-f") == 0)
            is_replace = true;
        else if (strcmp(argv[argi], "-k") == 0)
            is_keep = true;
        else if (strcmp(argv[argi], "-r") == 0)
            is_recursive = true;
        else if (strcmp(argv[argi], "-l") == 0)
            list_mode = true;
        else
            break;
        argi++;
    }
    if ((!list_mode && argc - argi < 3) || (list_mode && argc - argi < 2)) {
        fprintf(stderr, "Usage: cosmofs-tool [-f] [-k] [-r] <file or folder> <parent_node> <cosmo.bin>\n");
        fprintf(stderr, "       cosmofs-tool -l <directory_node> <cosmo.bin>\n");
        return 1;
    }
    const char *target_path  = list_mode ? NULL : argv[argi];
    const char *parent_str   = argv[argi + (list_mode ? 0 : 1)];
    const char *storage_path = argv[argi + (list_mode ? 1 : 2)];

    image       = calloc(1, IMAGE_SIZE);
    file_buffer = malloc(IMAGE_SIZE);
    read_buffer = malloc(IMAGE_SIZE);
    if (image == NULL || file_buffer == NULL || read_buffer == NULL) {
        fprintf(stderr, "cosmofs-tool: out of host memory\n");
        return 1;
    }
    FILE *img = fopen(storage_path, "rb+");
    if (img == NULL) {
        fprintf(stderr, "cosmofs-tool: failed to open storage '%s' (run `make disk` first)\n", storage_path);
        return 1;
    }
    if (fread(image, IMAGE_SIZE, 1, img) != 1) {
        fprintf(stderr, "cosmofs-tool: failed to read storage (expected at least 4 MiB)\n");
        fclose(img);
        return 1;
    }

    int8_t mount = cosmofs_mount(&fs, &device, host_time);
    if (mount == 1) { // no CosmoFS signature: blank image
        if (!cosmofs_format(&device, "cosmos", host_time) || cosmofs_mount(&fs, &device, host_time) != 0) {
            fprintf(stderr, "cosmofs-tool: format failed\n");
            fclose(img);
            return 1;
        }
        printf("Formatted a blank image as CosmoFS\n");
    } else if (mount != 0) {
        fprintf(stderr, "cosmofs-tool: '%s' is not a valid CosmoFS volume (mount error %d)\n", storage_path, mount);
        fclose(img);
        return 1;
    }

    uint32_t parent = (uint32_t) strtoul(parent_str, NULL, 10);
    int failures = 0;
    if (list_mode)
        list_directory(parent);
    else
        failures = insert_path(target_path, parent);

    struct CosmoFSCheckReport report;
    if (!cosmofs_check(&fs, &report)) {
        fprintf(stderr, "cosmofs-tool: WARNING, cosmofs_check found %u error(s): %s\n", report.errors, report.message);
        failures++;
    }

    fseek(img, 0, SEEK_SET);
    fwrite(image, IMAGE_SIZE, 1, img);
    fclose(img);
    return failures == 0 ? 0 : 1;
}
