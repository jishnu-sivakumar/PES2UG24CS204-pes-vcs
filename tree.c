#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

int object_write(int type, const void *data, size_t len, ObjectID *id_out);

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';

        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; 
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

static int write_tree_recursive(const IndexEntry *entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    for (int i = 0; i < count; ) {
        const char *current_path = entries[i].path + depth;
        const char *slash = strchr(current_path, '/');

        if (slash) {
            size_t dir_name_len = slash - current_path;
            char dir_name[256];
            strncpy(dir_name, current_path, dir_name_len);
            dir_name[dir_name_len] = '\0';

            int sub_count = 0;
            while (i + sub_count < count) {
                const char *p = entries[i + sub_count].path + depth;
                if (strncmp(p, dir_name, dir_name_len) == 0 && p[dir_name_len] == '/') {
                    sub_count++;
                } else {
                    break;
                }
            }

            ObjectID sub_tree_id;
            if (write_tree_recursive(entries + i, sub_count, depth + dir_name_len + 1, &sub_tree_id) != 0) {
                return -1;
            }

            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            strcpy(te->name, dir_name);
            memcpy(te->hash.hash, sub_tree_id.hash, HASH_SIZE);

            i += sub_count;
        } else {
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            strcpy(te->name, current_path);
            memcpy(te->hash.hash, entries[i].hash.hash, HASH_SIZE);
            i++;
        }

        if (tree.count >= MAX_TREE_ENTRIES) return -1;
    }

    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    
    if (object_write(1, data, len, id_out) != 0) {
        free(data);
        return -1;
    }

    free(data);
    return 0;
}

int tree_from_index(ObjectID *id_out) {
    Index idx;
    if (index_load(&idx) != 0) return -1;
    if (idx.count == 0) return -1;
    
    return write_tree_recursive(idx.entries, idx.count, 0, id_out);
}
