#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNUSED(value) ((void)(value))

typedef struct Storage Storage;
typedef struct File File;

typedef enum {
    FSAM_READ = 1,
    FSAM_WRITE = 2,
} FS_AccessMode;

typedef enum {
    FSOM_OPEN_EXISTING = 1,
    FSOM_CREATE_NEW = 2,
} FS_OpenMode;

typedef enum {
    FSE_OK,
    FSE_INTERNAL,
} FS_Error;

File* storage_file_alloc(Storage* storage);
void storage_file_free(File* file);
bool storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode);
bool storage_file_close(File* file);
size_t storage_file_read(File* file, void* buffer, size_t length);
size_t storage_file_write(File* file, const void* buffer, size_t length);
bool storage_file_seek(File* file, uint32_t offset, bool from_start);
uint64_t storage_file_tell(File* file);
uint64_t storage_file_size(File* file);
bool storage_dir_exists(Storage* storage, const char* path);
FS_Error storage_common_mkdir(Storage* storage, const char* path);
FS_Error storage_common_remove(Storage* storage, const char* path);
size_t strlcpy(char* destination, const char* source, size_t size);
