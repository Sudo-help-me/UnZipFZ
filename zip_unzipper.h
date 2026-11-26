#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <storage/storage.h>

bool zip_extract(
    Storage* storage,
    const char* archive_path,
    const char* output_path,
    char* error,
    size_t error_size,
    uint32_t* files_extracted);
