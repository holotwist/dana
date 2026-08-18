#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "file_list.h"
#include "state.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>

static int file_cmp(const void *a, const void *b) {
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;
    if (fa->is_dir != fb->is_dir) return fb->is_dir - fa->is_dir;
    return strcasecmp(fa->name, fb->name);
}

void load_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    num_files = 0;
    strcpy(files[num_files].name, "..");
    files[num_files].is_dir = 1;
    num_files++;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && num_files < MAX_FILES) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            int is_dir = S_ISDIR(st.st_mode);
            char *ext = strrchr(entry->d_name, '.');
            if (is_dir || (ext && (strcasecmp(ext, ".dana") == 0 || strcasecmp(ext, ".dahl") == 0))) {
                strncpy(files[num_files].name, entry->d_name, 255);
                files[num_files].is_dir = is_dir;
                num_files++;
            }
        }
    }
    closedir(dir);
    if (num_files > 1) qsort(files + 1, (size_t)(num_files - 1), sizeof(FileEntry), file_cmp);

    selected_file_idx = 0;
    scroll_offset = 0;
}