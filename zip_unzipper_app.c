#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <furi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "zip_unzipper.h"

#define ZIP_PATH_SIZE 512

static void show_message(DialogsApp* dialogs, const char* header, const char* text) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, header, 64, 10, AlignCenter, AlignCenter);
    dialog_message_set_text(message, text, 64, 30, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
}

static DialogMessageButton choose_destination(DialogsApp* dialogs) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Extract ZIP", 64, 10, AlignCenter, AlignCenter);
    dialog_message_set_text(message, "Choose destination", 64, 30, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, "Cancel", "Here", "Other");
    DialogMessageButton result = dialog_message_show(dialogs, message);
    dialog_message_free(message);
    return result;
}

static DialogMessageButton choose_extraction_layout(DialogsApp* dialogs) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Extract as", 64, 10, AlignCenter, AlignCenter);
    dialog_message_set_text(message, "Contents or folder?", 64, 30, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, "Cancel", "Contents", "Folder");
    DialogMessageButton result = dialog_message_show(dialogs, message);
    dialog_message_free(message);
    return result;
}

static bool get_parent_directory(const char* path, char* directory, size_t directory_size) {
    const char* filename = strrchr(path, '/');
    if(!filename || filename == path) return false;

    size_t directory_length = (size_t)(filename - path);
    if(directory_length >= directory_size) return false;
    memcpy(directory, path, directory_length);
    directory[directory_length] = '\0';
    return true;
}

static bool copy_directory_path(
    const char* directory, char* output_path, size_t output_size) {
    size_t length = strlen(directory);
    if(length == 0 || length >= output_size) return false;
    memcpy(output_path, directory, length + 1);
    return true;
}

static bool make_output_path(
    const char* archive_path,
    const char* destination_directory,
    char* output_path,
    size_t output_size) {
    const char* filename = strrchr(archive_path, '/');
    filename = filename ? filename + 1 : archive_path;
    const char* extension = strrchr(filename, '.');
    size_t base_length = extension ? (size_t)(extension - filename) : strlen(filename);
    size_t directory_length = strlen(destination_directory);
    bool needs_separator =
        directory_length > 0 && destination_directory[directory_length - 1] != '/';

    if(base_length == 0 || directory_length == 0 ||
       directory_length + needs_separator + base_length + sizeof("_unzipped") > output_size) {
        return false;
    }

    memcpy(output_path, destination_directory, directory_length);
    if(needs_separator) output_path[directory_length++] = '/';
    memcpy(output_path + directory_length, filename, base_length);
    output_path[directory_length + base_length] = '\0';
    strlcat(output_path, "_unzipped", output_size);
    return true;
}

int32_t zip_unzipper_app(void* arg) {
    UNUSED(arg);

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* selected_path = furi_string_alloc_set("/ext");
    DialogsFileBrowserOptions options;
    dialog_file_browser_set_basic_options(&options, ".zip", NULL);
    options.base_path = "/ext";

    if(dialog_file_browser_show(dialogs, selected_path, selected_path, &options)) {
        char output_path[ZIP_PATH_SIZE];
        char archive_path[ZIP_PATH_SIZE];
        size_t archive_path_length =
            strlcpy(archive_path, furi_string_get_cstr(selected_path), sizeof(archive_path));
        char destination_directory[ZIP_PATH_SIZE];
        if(archive_path_length >= sizeof(archive_path) ||
           !get_parent_directory(
               archive_path, destination_directory, sizeof(destination_directory))) {
            show_message(dialogs, "ZIP error", "Archive path is too long");
        } else {
            DialogMessageButton destination_choice = choose_destination(dialogs);
            bool destination_selected = destination_choice == DialogMessageButtonCenter;
            if(destination_choice == DialogMessageButtonRight) {
                show_message(dialogs, "Choose destination", "Select a file in the folder");
                DialogsFileBrowserOptions destination_options;
                dialog_file_browser_set_basic_options(&destination_options, NULL, NULL);
                destination_options.base_path = "/ext";
                if(dialog_file_browser_show(dialogs, selected_path, selected_path, &destination_options)) {
                    if(!get_parent_directory(
                           furi_string_get_cstr(selected_path),
                           destination_directory,
                           sizeof(destination_directory))) {
                        show_message(dialogs, "ZIP error", "Destination path is too long");
                    } else {
                        destination_selected = true;
                    }
                }
            }

            if(destination_selected) {
                DialogMessageButton layout_choice = choose_extraction_layout(dialogs);
                bool direct_to_destination = layout_choice == DialogMessageButtonCenter;
                bool layout_selected = direct_to_destination ||
                                       layout_choice == DialogMessageButtonRight;
                bool output_path_ready = false;
                if(direct_to_destination) {
                    output_path_ready = copy_directory_path(
                        destination_directory, output_path, sizeof(output_path));
                } else if(layout_choice == DialogMessageButtonRight) {
                    output_path_ready = make_output_path(
                        archive_path, destination_directory, output_path, sizeof(output_path));
                    if(output_path_ready && storage_dir_exists(storage, output_path)) {
                        show_message(dialogs, "ZIP error", "Output folder already exists");
                        layout_selected = false;
                    }
                }

                if(layout_selected && !output_path_ready) {
                    show_message(dialogs, "ZIP error", "Output path is too long");
                } else if(layout_selected) {
                    char error[96] = "Extraction failed";
                    uint32_t files_extracted = 0;
                    if(zip_extract(
                           storage, archive_path, output_path, error, sizeof(error), &files_extracted)) {
                        char result[96];
                        snprintf(
                            result,
                            sizeof(result),
                            "%lu file(s) extracted",
                            (unsigned long)files_extracted);
                        show_message(dialogs, "Done", result);
                    } else {
                        show_message(dialogs, "ZIP error", error);
                    }
                }
            }
        }
    }

    furi_string_free(selected_path);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);
    return 0;
}
