#define _CRT_SECURE_NO_WARNINGS

#include "zip_unzipper.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define test_mkdir(path) _mkdir(path)
#define test_unlink(path) _unlink(path)
#define test_stat(path, info) _stat(path, info)
#define test_is_directory(mode) (((mode) & _S_IFDIR) != 0)
#else
#include <unistd.h>
#define test_mkdir(path) mkdir(path, 0777)
#define test_unlink(path) unlink(path)
#define test_stat(path, info) stat(path, info)
#define test_is_directory(mode) S_ISDIR(mode)
#endif

struct Storage {
    char root[512];
};

struct File {
    FILE* stream;
};

static Storage test_storage;
static uint32_t tests_run;
static uint32_t tests_failed;

static bool map_path(Storage* storage, const char* path, char* result, size_t result_size) {
    if(strcmp(path, "/ext") == 0) {
        size_t length = strlen(storage->root);
        if(length >= result_size) return false;
        memcpy(result, storage->root, length + 1);
        return true;
    }
    if(strncmp(path, "/ext/", 5) != 0) return false;
    int length = snprintf(result, result_size, "%s/%s", storage->root, path + 5);
    if(length < 0 || (size_t)length >= result_size) return false;
#ifdef _WIN32
    for(char* cursor = result; *cursor; cursor++) {
        if(*cursor == '/') *cursor = '\\';
    }
#endif
    return true;
}

size_t strlcpy(char* destination, const char* source, size_t size) {
    size_t length = strlen(source);
    if(size) {
        size_t copied = length < size - 1 ? length : size - 1;
        memcpy(destination, source, copied);
        destination[copied] = '\0';
    }
    return length;
}

File* storage_file_alloc(Storage* storage) {
    UNUSED(storage);
    return calloc(1, sizeof(File));
}

void storage_file_free(File* file) {
    if(file) {
        if(file->stream) fclose(file->stream);
        free(file);
    }
}

bool storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode) {
    char host_path[1024];
    if(!map_path(&test_storage, path, host_path, sizeof(host_path))) return false;
    if(mode == FSOM_CREATE_NEW) {
        struct stat info;
        if(test_stat(host_path, &info) == 0) return false;
        file->stream = fopen(host_path, "wb");
    } else if(access == FSAM_READ) {
        file->stream = fopen(host_path, "rb");
    }
    return file->stream != NULL;
}

bool storage_file_close(File* file) {
    if(!file || !file->stream) return true;
    FILE* stream = file->stream;
    file->stream = NULL;
    return fclose(stream) == 0;
}

size_t storage_file_read(File* file, void* buffer, size_t length) {
    return fread(buffer, 1, length, file->stream);
}

size_t storage_file_write(File* file, const void* buffer, size_t length) {
    return fwrite(buffer, 1, length, file->stream);
}

bool storage_file_seek(File* file, uint32_t offset, bool from_start) {
    return fseek(file->stream, (long)offset, from_start ? SEEK_SET : SEEK_CUR) == 0;
}

uint64_t storage_file_tell(File* file) {
    return (uint64_t)ftell(file->stream);
}

uint64_t storage_file_size(File* file) {
    long current = ftell(file->stream);
    if(current < 0 || fseek(file->stream, 0, SEEK_END) != 0) return 0;
    long size = ftell(file->stream);
    if(fseek(file->stream, current, SEEK_SET) != 0 || size < 0) return 0;
    return (uint64_t)size;
}

bool storage_dir_exists(Storage* storage, const char* path) {
    char host_path[1024];
    struct stat info;
    return map_path(storage, path, host_path, sizeof(host_path)) &&
           test_stat(host_path, &info) == 0 && test_is_directory(info.st_mode);
}

FS_Error storage_common_mkdir(Storage* storage, const char* path) {
    char host_path[1024];
    if(!map_path(storage, path, host_path, sizeof(host_path))) return FSE_INTERNAL;
    return test_mkdir(host_path) == 0 ? FSE_OK : FSE_INTERNAL;
}

FS_Error storage_common_remove(Storage* storage, const char* path) {
    char host_path[1024];
    if(!map_path(storage, path, host_path, sizeof(host_path))) return FSE_INTERNAL;
    return test_unlink(host_path) == 0 ? FSE_OK : FSE_INTERNAL;
}

static void check(bool passed, const char* name) {
    tests_run++;
    if(!passed) {
        tests_failed++;
        fprintf(stderr, "FAIL: %s\n", name);
    } else {
        printf("PASS: %s\n", name);
    }
}

static bool file_equals(const char* path, const uint8_t* expected, size_t expected_size) {
    char host_path[1024];
    if(!map_path(&test_storage, path, host_path, sizeof(host_path))) return false;
    FILE* file = fopen(host_path, "rb");
    if(!file) return false;
    uint8_t* actual = malloc(expected_size ? expected_size : 1);
    bool result = actual && fread(actual, 1, expected_size, file) == expected_size &&
                  fgetc(file) == EOF && memcmp(actual, expected, expected_size) == 0;
    free(actual);
    fclose(file);
    return result;
}

static bool file_exists(const char* path) {
    char host_path[1024];
    struct stat info;
    return map_path(&test_storage, path, host_path, sizeof(host_path)) &&
           test_stat(host_path, &info) == 0;
}

static bool extract(const char* archive, const char* output, bool expected_success, uint32_t expected_files) {
    char error[96] = {0};
    uint32_t files = 0;
    bool success = zip_extract(
        &test_storage, archive, output, error, sizeof(error), &files);
    if(success != expected_success) {
        fprintf(stderr, "  unexpected extractor result: %s (%s)\n", success ? "success" : "failure", error);
        return false;
    }
    return !success || files == expected_files;
}

int main(int argc, char** argv) {
    if(argc != 2 || strlen(argv[1]) >= sizeof(test_storage.root)) {
        fprintf(stderr, "Usage: test_zip_unzipper <fixture directory>\n");
        return 2;
    }
    strlcpy(test_storage.root, argv[1], sizeof(test_storage.root));
#ifdef _WIN32
    for(char* cursor = test_storage.root; *cursor; cursor++) {
        if(*cursor == '/') *cursor = '\\';
    }
#endif

    static const uint8_t stored_text[] = "stored data";
    check(
        extract("/ext/stored.zip", "/ext/out_stored", true, 2) &&
            file_equals("/ext/out_stored/plain.txt", stored_text, sizeof(stored_text) - 1) &&
            file_equals("/ext/out_stored/empty.bin", (const uint8_t*)"", 0),
        "stored entries and empty file");

    check(
        storage_common_mkdir(&test_storage, "/ext/out_direct") == FSE_OK &&
            extract("/ext/stored.zip", "/ext/out_direct", true, 2) &&
            file_equals("/ext/out_direct/plain.txt", stored_text, sizeof(stored_text) - 1) &&
            file_equals("/ext/out_direct/empty.bin", (const uint8_t*)"", 0),
        "extract contents into existing directory");

    char repeated[96001];
    for(size_t i = 0; i < sizeof(repeated) - 1; i++) {
        repeated[i] = (char)('a' + (i % 23));
    }
    repeated[sizeof(repeated) - 1] = '\0';
    check(
        extract("/ext/deflated.zip", "/ext/out_deflated", true, 1) &&
            file_equals(
                "/ext/out_deflated/repeated.txt",
                (const uint8_t*)repeated,
                sizeof(repeated) - 1),
        "DEFLATE entry with back-references");

    check(
        extract("/ext/empty.zip", "/ext/out_empty", true, 0) &&
            storage_dir_exists(&test_storage, "/ext/out_empty"),
        "empty archive");

    check(
        extract("/ext/traversal.zip", "/ext/out_traversal", false, 0) &&
            !file_exists("/ext/escape.txt"),
        "reject parent-directory traversal");

    check(
        extract("/ext/bad_crc.zip", "/ext/out_bad_crc", false, 0) &&
            !file_exists("/ext/out_bad_crc/plain.txt"),
        "reject CRC mismatch and remove partial file");

    check(
        extract("/ext/truncated.zip", "/ext/out_truncated", false, 0),
        "reject truncated archive");

    check(
        extract("/ext/encrypted.zip", "/ext/out_encrypted", false, 0),
        "reject encrypted entry");

    printf("%lu tests, %lu failed\n", (unsigned long)tests_run, (unsigned long)tests_failed);
    return tests_failed ? 1 : 0;
}
