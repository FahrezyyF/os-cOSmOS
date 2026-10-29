// External program: Inserter (book 3.3.1), runs on the host OS.
// Inserts files / folders into an EXT2 - IF2130 Edition image using the very same ext2.c as the
// kernel; only read_blocks() / write_blocks() are re-implemented on top of host memory.
//
// usage: inserter [-f] [-k] [-r] <file or folder> <parent inode> <storage.bin>
//        inserter -l <inode> <storage.bin>          (list a directory)
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
#include "header/filesystem/ext2.h"

#define IMAGE_SIZE (4 * 1024 * 1024)

// Global variable
uint8_t *image_storage;
uint8_t *file_buffer;
uint8_t *read_buffer;
bool is_replace   = false;
bool is_keep      = false;
bool is_recursive = false;

void read_blocks(void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    for (int i = 0; i < block_count; i++) {
        memcpy(
            (uint8_t*) ptr + BLOCK_SIZE*i,
            image_storage + BLOCK_SIZE*(logical_block_address+i),
            BLOCK_SIZE
        );
    }
}

void write_blocks(const void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    for (int i = 0; i < block_count; i++) {
        memcpy(
            image_storage + BLOCK_SIZE*(logical_block_address+i),
            (uint8_t*) ptr + BLOCK_SIZE*i,
            BLOCK_SIZE
        );
    }
}

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
        case 2:  return "Error: Invalid parent node index";
        default: return "Error: Unknown error (invalid name or storage full)";
    }
}

static int insert_path(const char *path, uint32_t parent_inode);

static int insert_file(const char *path, const char *name, uint8_t name_len, uint32_t parent_inode) {
    FILE *fptr_target = fopen(path, "rb");
    if (fptr_target == NULL) {
        fprintf(stderr, "inserter: cannot open input file '%s'\n", path);
        return 1;
    }
    size_t filesize = fread(file_buffer, 1, IMAGE_SIZE, fptr_target);
    fclose(fptr_target);

    printf("Filename : %.*s\n", name_len, name);
    printf("Filesize : %zu bytes\n", filesize);

    struct EXT2DriverRequest request = {
        .buf          = file_buffer,
        .name         = (char*) name,
        .name_len     = name_len,
        .parent_inode = parent_inode,
        .buffer_size  = (uint32_t) filesize,
        .is_directory = false,
    };

    struct EXT2DriverRequest reqread = request;
    reqread.buf         = read_buffer;
    reqread.buffer_size = IMAGE_SIZE;
    if (read(reqread) == 0) {
        bool same = memcmp(read_buffer, file_buffer, filesize) == 0;
        printf("Existing file is %s\n", same ? "identical" : "different");
    }

    int8_t retcode = write(&request);
    if (retcode == 1 && is_keep && !is_replace) {
        puts("Kept existing entry");
        return 0;
    }
    if (retcode == 1 && is_replace) {
        retcode = delete(request);
        if (retcode == 0)
            retcode = write(&request);
    }
    puts(write_message(retcode));
    if (retcode != 0)
        return 1;

    // Verify the bytes that landed in the image
    if (read(reqread) != 0 || memcmp(read_buffer, file_buffer, filesize) != 0) {
        fprintf(stderr, "inserter: read-back verification FAILED for %s\n", path);
        return 1;
    }
    return 0;
}

static int insert_directory(const char *path, const char *name, uint8_t name_len, uint32_t parent_inode) {
    struct EXT2DriverRequest request = {
        .name = (char*) name, .name_len = name_len, .parent_inode = parent_inode, .is_directory = true,
    };
    int8_t retcode = write(&request);
    printf("Folder   : %.*s -> %s\n", name_len, name, write_message(retcode));
    if (retcode != 0 && !(retcode == 1 && (is_recursive || is_keep)))
        return 1;
    if (!is_recursive)
        return 0;

    uint32_t dir_inode;
    if (ext2_lookup(parent_inode, name, name_len, &dir_inode, NULL) != 0)
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
        failures += insert_path(child, dir_inode);
    }
    closedir(dir);
    return failures;
}

static int insert_path(const char *path, uint32_t parent_inode) {
    const char *name = base_name(path);
    size_t name_len = strcspn(name, "/");
    if (name_len == 0 || name_len > EXT2_NAME_MAX) {
        fprintf(stderr, "inserter: invalid name for '%s'\n", path);
        return 1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "inserter: cannot stat '%s'\n", path);
        return 1;
    }
    if (S_ISDIR(st.st_mode))
        return insert_directory(path, name, (uint8_t) name_len, parent_inode);
    return insert_file(path, name, (uint8_t) name_len, parent_inode);
}

static void list_directory(uint32_t inode) {
    struct EXT2DriverRequest request = {
        .buf = read_buffer, .name = ".", .name_len = 1, .parent_inode = inode, .buffer_size = IMAGE_SIZE,
    };
    int8_t ret = read_directory(&request);
    if (ret != 0) {
        printf("read_directory failed: %d\n", ret);
        return;
    }
    for (uint32_t block = 0; block < IMAGE_SIZE / BLOCK_SIZE; block++) {
        uint8_t *b = read_buffer + block * BLOCK_SIZE;
        if (get_directory_entry(b, 0)->rec_len == 0)
            break;
        for (uint32_t off = 0; off < BLOCK_SIZE;) {
            struct EXT2DirectoryEntry *e = get_directory_entry(b, off);
            if (e->rec_len == 0)
                break;
            if (e->inode != 0) {
                struct EXT2Inode node;
                ext2_get_inode(e->inode, &node);
                printf("%6u  %c %9u  %.*s\n", e->inode, e->file_type == EXT2_FT_DIR ? 'd' : '-',
                       node.i_size, e->name_len, get_entry_name(e));
            }
            off += e->rec_len;
        }
    }
}

int main(int argc, char *argv[]) {
    int argi = 1;
    bool list_mode = false;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (strcmp(argv[argi], "-f") == 0)
            is_replace = true;
        else if (strcmp(argv[argi], "-r") == 0)
            is_recursive = true;
        else if (strcmp(argv[argi], "-k") == 0)
            is_keep = true;
        else if (strcmp(argv[argi], "-l") == 0)
            list_mode = true;
        else
            break;
        argi++;
    }

    if ((!list_mode && argc - argi < 3) || (list_mode && argc - argi < 2)) {
        fprintf(stderr, "Usage: inserter [-f] [-k] [-r] <file or folder> <parent_inode> <storage.bin>\n");
        fprintf(stderr, "       inserter -l <directory_inode> <storage.bin>\n");
        fprintf(stderr, "  -f : replace if the entry already exists\n");
        fprintf(stderr, "  -k : keep (skip) entries that already exist\n");
        fprintf(stderr, "  -r : insert folders recursively\n");
        exit(1);
    }

    const char *target_path  = list_mode ? NULL : argv[argi];
    const char *parent_str   = argv[argi + (list_mode ? 0 : 1)];
    const char *storage_path = argv[argi + (list_mode ? 1 : 2)];

    // Read storage into memory, requiring 4 MB memory
    image_storage = calloc(1, IMAGE_SIZE);
    file_buffer   = malloc(IMAGE_SIZE);
    read_buffer   = malloc(IMAGE_SIZE);
    if (image_storage == NULL || file_buffer == NULL || read_buffer == NULL) {
        fprintf(stderr, "inserter: out of host memory\n");
        exit(1);
    }

    FILE *fptr = fopen(storage_path, "rb+");
    if (fptr == NULL) {
        fprintf(stderr, "inserter: failed to open storage '%s' (run `make disk` first)\n", storage_path);
        exit(1);
    }
    if (fread(image_storage, IMAGE_SIZE, 1, fptr) != 1) {
        fprintf(stderr, "inserter: failed to read storage (expected 4MiB)\n");
        fclose(fptr);
        exit(1);
    }

    // EXT2 operations
    ext2_set_time_source(host_time);
    initialize_filesystem_ext2();
    uint32_t parent_inode = (uint32_t) strtoul(parent_str, NULL, 10);

    int failures = 0;
    if (list_mode)
        list_directory(parent_inode);
    else
        failures = insert_path(target_path, parent_inode);

    struct EXT2CheckReport report;
    if (!ext2_check(&report)) {
        fprintf(stderr, "inserter: WARNING, ext2_check found %u error(s): %s\n", report.errors, report.message);
        failures++;
    }

    // Write image in memory into original, overwrite them
    fseek(fptr, 0, SEEK_SET);
    fwrite(image_storage, IMAGE_SIZE, 1, fptr);
    fclose(fptr);

    return failures == 0 ? 0 : 1;
}
