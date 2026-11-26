#include "zip_unzipper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_NAME_MAX 512
#define ZIP_CHUNK_SIZE 512
#define ZIP_MAX_FILE_SIZE (64U * 1024U * 1024U)
#define ZIP_MAX_TOTAL_SIZE (128U * 1024U * 1024U)

typedef struct {
    File* file;
    uint32_t remaining;
    uint8_t bytes[ZIP_CHUNK_SIZE];
    size_t position;
    size_t length;
    uint32_t bits;
    uint8_t bit_count;
    uint64_t bits_consumed;
    bool failed;
} BitReader;

typedef struct {
    uint16_t count[16];
    uint16_t first_code[16];
    uint16_t first_symbol[16];
    uint16_t symbols[288];
    uint8_t max_bits;
} Huffman;

typedef struct {
    File* file;
    uint8_t window[32768];
    uint8_t output[ZIP_CHUNK_SIZE];
    size_t output_length;
    uint32_t window_position;
    uint32_t crc;
    uint32_t size;
    uint32_t expected_size;
} Output;

static uint32_t crc_table[256];
static bool crc_table_initialized;

static void crc_table_init(void) {
    if(crc_table_initialized) return;
    for(uint16_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for(uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        crc_table[i] = crc;
    }
    crc_table_initialized = true;
}

static uint16_t read_u16(const uint8_t* bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static bool read_exact(Storage* storage, File* file, void* buffer, size_t length) {
    UNUSED(storage);
    return storage_file_read(file, buffer, length) == length;
}

static bool seek_file(Storage* storage, File* file, uint32_t offset) {
    UNUSED(storage);
    return storage_file_seek(file, offset, true);
}

static bool set_error(char* error, size_t error_size, const char* text) {
    if(error_size > 0) strlcpy(error, text, error_size);
    return false;
}

static bool bit_read(BitReader* reader, uint8_t count, uint32_t* value) {
    while(reader->bit_count < count) {
        if(reader->position == reader->length) {
            if(reader->remaining == 0) {
                reader->failed = true;
                return false;
            }
            size_t amount = reader->remaining < sizeof(reader->bytes) ?
                                reader->remaining :
                                sizeof(reader->bytes);
            size_t read = storage_file_read(reader->file, reader->bytes, amount);
            if(read != amount) {
                reader->failed = true;
                return false;
            }
            reader->remaining -= amount;
            reader->position = 0;
            reader->length = amount;
        }
        reader->bits |= (uint32_t)reader->bytes[reader->position++] << reader->bit_count;
        reader->bit_count += 8;
    }
    *value = reader->bits & ((1U << count) - 1U);
    reader->bits >>= count;
    reader->bit_count -= count;
    reader->bits_consumed += count;
    return true;
}

static bool huffman_build(Huffman* table, const uint8_t* lengths, uint16_t symbols) {
    memset(table, 0, sizeof(*table));
    for(uint16_t i = 0; i < symbols; i++) {
        if(lengths[i] > 15) return false;
        if(lengths[i]) {
            table->count[lengths[i]]++;
            if(lengths[i] > table->max_bits) table->max_bits = lengths[i];
        }
    }

    uint16_t code = 0;
    uint16_t symbol_offset = 0;
    for(uint8_t bits = 1; bits <= 15; bits++) {
        code = (code + table->count[bits - 1]) << 1;
        table->first_code[bits] = code;
        table->first_symbol[bits] = symbol_offset;
        symbol_offset += table->count[bits];
        if(code + table->count[bits] > (1U << bits)) return false;
    }

    uint16_t offsets[16];
    memcpy(offsets, table->first_symbol, sizeof(offsets));
    for(uint16_t symbol = 0; symbol < symbols; symbol++) {
        if(lengths[symbol]) table->symbols[offsets[lengths[symbol]]++] = symbol;
    }
    return table->max_bits != 0;
}

static bool huffman_decode(BitReader* reader, const Huffman* table, uint16_t* symbol) {
    uint32_t bit;
    uint16_t code = 0;
    for(uint8_t length = 1; length <= table->max_bits; length++) {
        if(!bit_read(reader, 1, &bit)) return false;
        code = (code << 1) | bit;
        uint16_t first = table->first_code[length];
        if(code >= first && code - first < table->count[length]) {
            *symbol = table->symbols[table->first_symbol[length] + code - first];
            return true;
        }
    }
    return false;
}

static bool build_fixed_tables(Huffman* literal, Huffman* distance) {
    uint8_t literal_lengths[288];
    uint8_t distance_lengths[32];
    for(uint16_t i = 0; i <= 143; i++) literal_lengths[i] = 8;
    for(uint16_t i = 144; i <= 255; i++) literal_lengths[i] = 9;
    for(uint16_t i = 256; i <= 279; i++) literal_lengths[i] = 7;
    for(uint16_t i = 280; i <= 287; i++) literal_lengths[i] = 8;
    memset(distance_lengths, 5, sizeof(distance_lengths));
    return huffman_build(literal, literal_lengths, 288) &&
           huffman_build(distance, distance_lengths, 32);
}

static bool build_dynamic_tables(BitReader* reader, Huffman* literal, Huffman* distance) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint32_t value;
    if(!bit_read(reader, 5, &value)) return false;
    uint16_t literal_count = value + 257;
    if(literal_count > 286) return false;
    if(!bit_read(reader, 5, &value)) return false;
    uint16_t distance_count = value + 1;
    if(!bit_read(reader, 4, &value)) return false;
    uint16_t code_count = value + 4;

    uint8_t code_lengths[19] = {0};
    for(uint16_t i = 0; i < code_count; i++) {
        if(!bit_read(reader, 3, &value)) return false;
        code_lengths[order[i]] = value;
    }

    Huffman code_table;
    if(!huffman_build(&code_table, code_lengths, 19)) return false;
    uint8_t lengths[286 + 32];
    uint16_t total = literal_count + distance_count;
    uint16_t position = 0;
    while(position < total) {
        uint16_t symbol;
        if(!huffman_decode(reader, &code_table, &symbol)) return false;
        if(symbol <= 15) {
            lengths[position++] = symbol;
        } else {
            uint8_t repeat_bits;
            uint8_t repeat_base;
            uint8_t repeated;
            if(symbol == 16) {
                if(position == 0) return false;
                repeat_bits = 2;
                repeat_base = 3;
                repeated = lengths[position - 1];
            } else if(symbol == 17) {
                repeat_bits = 3;
                repeat_base = 3;
                repeated = 0;
            } else if(symbol == 18) {
                repeat_bits = 7;
                repeat_base = 11;
                repeated = 0;
            } else {
                return false;
            }
            if(!bit_read(reader, repeat_bits, &value)) return false;
            uint16_t repeat = repeat_base + value;
            if(repeat > total - position) return false;
            memset(lengths + position, repeated, repeat);
            position += repeat;
        }
    }

    return lengths[256] != 0 && huffman_build(literal, lengths, literal_count) &&
           huffman_build(distance, lengths + literal_count, distance_count);
}

static bool output_flush(Output* output) {
    if(output->output_length == 0) return true;
    if(storage_file_write(output->file, output->output, output->output_length) !=
       output->output_length) {
        return false;
    }
    output->output_length = 0;
    return true;
}

static bool output_byte(Output* output, uint8_t byte) {
    if(output->size >= output->expected_size) return false;
    output->window[output->window_position] = byte;
    output->window_position = (output->window_position + 1) & 32767;
    output->output[output->output_length++] = byte;
    output->crc = (output->crc >> 8) ^ crc_table[(output->crc ^ byte) & 0xffU];
    output->size++;
    return output->output_length < sizeof(output->output) || output_flush(output);
}

static bool inflate_codes(
    BitReader* reader,
    Output* output,
    const Huffman* literal,
    const Huffman* distance) {
    static const uint16_t length_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t length_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t distance_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577};
    static const uint8_t distance_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    uint32_t value;
    for(;;) {
        uint16_t symbol;
        if(!huffman_decode(reader, literal, &symbol)) return false;
        if(symbol < 256) {
            if(!output_byte(output, symbol)) return false;
        } else if(symbol == 256) {
            return true;
        } else if(symbol >= 257 && symbol <= 285) {
            uint16_t length_index = symbol - 257;
            uint32_t length = length_base[length_index];
            if(!bit_read(reader, length_extra[length_index], &value)) return false;
            length += value;
            if(!huffman_decode(reader, distance, &symbol) || symbol >= 30) return false;
            uint32_t back = distance_base[symbol];
            if(!bit_read(reader, distance_extra[symbol], &value)) return false;
            back += value;
            if(back > output->size || back > sizeof(output->window)) return false;
            while(length--) {
                uint8_t byte =
                    output->window[(output->window_position - back) & 32767];
                if(!output_byte(output, byte)) return false;
            }
        } else {
            return false;
        }
    }
}

static bool inflate_raw(
    Storage* storage,
    File* archive,
    uint32_t compressed_size,
    Output* output) {
    UNUSED(storage);
    BitReader reader = {
        .file = archive,
        .remaining = compressed_size,
    };
    bool final_block = false;
    while(!final_block) {
        uint32_t value;
        if(!bit_read(&reader, 1, &value)) return false;
        final_block = value != 0;
        if(!bit_read(&reader, 2, &value)) return false;
        if(value == 0) {
            uint32_t discard = reader.bit_count & 7U;
            reader.bits >>= discard;
            reader.bit_count -= discard;
            reader.bits_consumed += discard;
            uint32_t length;
            uint32_t inverse;
            if(!bit_read(&reader, 16, &length) || !bit_read(&reader, 16, &inverse) ||
               (length ^ 0xffffU) != inverse) {
                return false;
            }
            for(uint32_t i = 0; i < length; i++) {
                if(!bit_read(&reader, 8, &value) || !output_byte(output, value)) return false;
            }
        } else if(value == 1 || value == 2) {
            Huffman literal;
            Huffman distance;
            bool built = value == 1 ? build_fixed_tables(&literal, &distance) :
                                      build_dynamic_tables(&reader, &literal, &distance);
            if(!built || !inflate_codes(&reader, output, &literal, &distance)) return false;
        } else {
            return false;
        }
    }
    return !reader.failed && (reader.bits_consumed + 7U) / 8U == compressed_size &&
           output_flush(output);
}

static bool crc_matches(Output* output, uint32_t expected_crc, uint32_t expected_size) {
    return output_flush(output) && output->size == expected_size &&
           (output->crc ^ 0xffffffffU) == expected_crc;
}

static bool normalize_entry_name(char* name, size_t length, bool* is_directory) {
    if(length == 0 || length >= ZIP_NAME_MAX || name[0] == '/' || name[0] == '\\') return false;
    for(size_t i = 0; i < length; i++) {
        if(name[i] == '\0') return false;
    }
    if(name[length - 1] == '/' || name[length - 1] == '\\') {
        *is_directory = true;
        name[--length] = '\0';
    } else {
        name[length] = '\0';
    }

    size_t segment_start = 0;
    for(size_t i = 0; i <= length; i++) {
        if(name[i] == '\\') name[i] = '/';
        if(name[i] == ':') return false;
        if(name[i] == '/' || i == length) {
            size_t segment_length = i - segment_start;
            if(segment_length == 0 && i != length) return false;
            if((segment_length == 1 && name[segment_start] == '.') ||
               (segment_length == 2 && name[segment_start] == '.' &&
                name[segment_start + 1] == '.')) {
                return false;
            }
            segment_start = i + 1;
        }
    }
    return true;
}

static bool ensure_directory(Storage* storage, const char* path) {
    char current[ZIP_NAME_MAX];
    size_t length = strlen(path);
    if(length == 0 || length >= sizeof(current)) return false;
    memcpy(current, path, length + 1);
    for(char* cursor = current + 1; *cursor; cursor++) {
        if(*cursor == '/') {
            *cursor = '\0';
            if(!storage_dir_exists(storage, current) &&
               storage_common_mkdir(storage, current) != FSE_OK) {
                return false;
            }
            *cursor = '/';
        }
    }
    return storage_dir_exists(storage, current) ||
           storage_common_mkdir(storage, current) == FSE_OK;
}

static bool find_end_record(Storage* storage, File* archive, uint32_t size, uint32_t* eocd_offset) {
    uint32_t scan_size = size < 65557U ? size : 65557U;
    uint8_t* tail = malloc(scan_size);
    if(!tail) return false;
    bool found = false;
    if(seek_file(storage, archive, size - scan_size) &&
       read_exact(storage, archive, tail, scan_size)) {
        for(int32_t i = (int32_t)scan_size - 22; i >= 0; i--) {
            if(read_u32(tail + i) == 0x06054b50U) {
                uint16_t comment_length = read_u16(tail + i + 20);
                if((uint32_t)i + 22U + comment_length == scan_size) {
                    *eocd_offset = size - scan_size + (uint32_t)i;
                    found = true;
                    break;
                }
            }
        }
    }
    free(tail);
    return found;
}

static bool extract_entry(
    Storage* storage,
    File* archive,
    const char* root,
    uint32_t central_offset,
    uint16_t flags,
    uint16_t method,
    uint32_t crc,
    uint32_t compressed_size,
    uint32_t uncompressed_size,
    uint32_t local_offset,
    char* name,
    size_t name_length,
    uint32_t* total_size,
    uint32_t* files_extracted) {
    bool is_directory = false;
    if(!normalize_entry_name(name, name_length, &is_directory)) return false;
    char output_path[ZIP_NAME_MAX];
    int root_length = snprintf(output_path, sizeof(output_path), "%s/%s", root, name);
    if(root_length < 0 || (size_t)root_length >= sizeof(output_path)) return false;

    if(is_directory) return ensure_directory(storage, output_path);
    if(uncompressed_size > ZIP_MAX_FILE_SIZE ||
       *total_size > ZIP_MAX_TOTAL_SIZE - uncompressed_size) {
        return false;
    }

    char parent[ZIP_NAME_MAX];
    strlcpy(parent, output_path, sizeof(parent));
    char* last_slash = strrchr(parent, '/');
    if(!last_slash) return false;
    *last_slash = '\0';
    if(!ensure_directory(storage, parent)) return false;

    uint8_t local_header[30];
    if(!seek_file(storage, archive, local_offset) ||
       !read_exact(storage, archive, local_header, sizeof(local_header)) ||
       read_u32(local_header) != 0x04034b50U) {
        return false;
    }
    uint16_t local_name_length = read_u16(local_header + 26);
    uint16_t local_extra_length = read_u16(local_header + 28);
    if(read_u16(local_header + 6) != flags || read_u16(local_header + 8) != method) {
        return false;
    }
    uint32_t data_offset = local_offset + sizeof(local_header) + local_name_length +
                           local_extra_length;
    if(data_offset < local_offset || data_offset > central_offset ||
       compressed_size > central_offset - data_offset ||
       !seek_file(storage, archive, data_offset)) {
        return false;
    }

    File* output_file = storage_file_alloc(storage);
    if(!output_file) return false;
    if(!storage_file_open(output_file, output_path, FSAM_WRITE, FSOM_CREATE_NEW)) {
        storage_file_close(output_file);
        storage_file_free(output_file);
        return false;
    }
    bool success = true;

    Output* output = malloc(sizeof(Output));
    if(!output) {
        storage_file_close(output_file);
        storage_file_free(output_file);
        storage_common_remove(storage, output_path);
        return false;
    }
    memset(output, 0, sizeof(Output));
    output->file = output_file;
    output->crc = 0xffffffffU;
    output->expected_size = uncompressed_size;
    crc_table_init();

    if(method == 0 && compressed_size == uncompressed_size) {
        uint8_t buffer[ZIP_CHUNK_SIZE];
        uint32_t remaining = compressed_size;
        success = true;
        while(remaining > 0 && success) {
            size_t amount = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
            success = read_exact(storage, archive, buffer, amount);
            for(size_t i = 0; i < amount && success; i++) success = output_byte(output, buffer[i]);
            remaining -= amount;
        }
    } else if(method == 8) {
        success = inflate_raw(storage, archive, compressed_size, output);
    } else {
        success = false;
    }

    success = success && crc_matches(output, crc, uncompressed_size);
    free(output);
    bool closed = storage_file_close(output_file);
    storage_file_free(output_file);
    if(!success || !closed) {
        storage_common_remove(storage, output_path);
        return false;
    }
    *total_size += uncompressed_size;
    (*files_extracted)++;
    UNUSED(flags);
    return true;
}

bool zip_extract(
    Storage* storage,
    const char* archive_path,
    const char* output_path,
    char* error,
    size_t error_size,
    uint32_t* files_extracted) {
    File* archive = storage_file_alloc(storage);
    if(!archive) return set_error(error, error_size, "Cannot open archive");
    if(!storage_file_open(archive, archive_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(archive);
        storage_file_free(archive);
        return set_error(error, error_size, "Cannot open archive");
    }

    bool success = false;
    uint32_t total_size = 0;
    *files_extracted = 0;
    uint64_t file_size_64 = storage_file_size(archive);
    if(file_size_64 > 0xffffffffU || file_size_64 < 22) {
        set_error(error, error_size, "Invalid ZIP size");
        goto cleanup;
    }
    uint32_t file_size = file_size_64;
    uint32_t eocd_offset;
    if(!find_end_record(storage, archive, file_size, &eocd_offset)) {
        set_error(error, error_size, "ZIP end record not found");
        goto cleanup;
    }

    uint8_t eocd[22];
    if(!seek_file(storage, archive, eocd_offset) || !read_exact(storage, archive, eocd, sizeof(eocd)) ||
       read_u32(eocd) != 0x06054b50U || read_u16(eocd + 4) != 0 || read_u16(eocd + 6) != 0 ||
       read_u16(eocd + 8) != read_u16(eocd + 10) || read_u16(eocd + 10) == 0xffffU ||
       read_u32(eocd + 12) == 0xffffffffU || read_u32(eocd + 16) == 0xffffffffU) {
        set_error(error, error_size, "Unsupported or invalid ZIP");
        goto cleanup;
    }

    uint16_t entry_count = read_u16(eocd + 10);
    uint32_t central_size = read_u32(eocd + 12);
    uint32_t central_offset = read_u32(eocd + 16);
    if(central_offset > eocd_offset || central_size > eocd_offset - central_offset ||
       !ensure_directory(storage, output_path)) {
        set_error(error, error_size, "Cannot create output folder");
        goto cleanup;
    }
    uint32_t central_end = central_offset + central_size;

    if(!seek_file(storage, archive, central_offset)) {
        set_error(error, error_size, "Invalid central directory");
        goto cleanup;
    }

    for(uint16_t entry = 0; entry < entry_count; entry++) {
        uint64_t current_offset = storage_file_tell(archive);
        if(current_offset > central_end || central_end - current_offset < 46) {
            set_error(error, error_size, "Invalid central directory");
            goto cleanup;
        }
        uint8_t header[46];
        if(!read_exact(storage, archive, header, sizeof(header)) ||
           read_u32(header) != 0x02014b50U) {
            set_error(error, error_size, "Invalid ZIP entry");
            goto cleanup;
        }

        uint16_t flags = read_u16(header + 8);
        uint16_t method = read_u16(header + 10);
        uint32_t crc = read_u32(header + 16);
        uint32_t compressed_size = read_u32(header + 20);
        uint32_t uncompressed_size = read_u32(header + 24);
        uint16_t name_length = read_u16(header + 28);
        uint16_t extra_length = read_u16(header + 30);
        uint16_t comment_length = read_u16(header + 32);
        uint32_t local_offset = read_u32(header + 42);
        uint64_t next_entry_offset_64 = storage_file_tell(archive) + name_length + extra_length +
                                        comment_length;
        if(name_length == 0 || name_length >= ZIP_NAME_MAX || compressed_size == 0xffffffffU ||
           uncompressed_size == 0xffffffffU || local_offset == 0xffffffffU ||
           (method != 0 && method != 8) ||
           (method == 0 && compressed_size != uncompressed_size) ||
           (flags & (1U | 0x20U | 0x40U | 0x2000U)) != 0 || (flags & ~0x080eU) != 0) {
            set_error(error, error_size, "Unsupported ZIP feature");
            goto cleanup;
        }

        char name[ZIP_NAME_MAX];
        if(!read_exact(storage, archive, name, name_length)) {
            set_error(error, error_size, "Invalid ZIP filename");
            goto cleanup;
        }
        if(!extract_entry(
               storage,
               archive,
               output_path,
               central_offset,
               flags,
               method,
               crc,
               compressed_size,
               uncompressed_size,
               local_offset,
               name,
               name_length,
               &total_size,
               files_extracted)) {
            set_error(error, error_size, "Extraction failed or unsafe entry");
            goto cleanup;
        }
        if(next_entry_offset_64 > central_end ||
           !seek_file(storage, archive, (uint32_t)next_entry_offset_64)) {
            set_error(error, error_size, "Invalid central directory");
            goto cleanup;
        }
    }

    success = true;

cleanup:
    storage_file_close(archive);
    storage_file_free(archive);
    return success;
}
