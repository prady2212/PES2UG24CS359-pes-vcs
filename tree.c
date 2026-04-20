// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// IMPLEMENTED:        tree_from_index

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ──────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── Local index types (mirrors index.h exactly) ─────────────────────────────
// Defined here so tree.c compiles and links without index.o.
// test_tree links only tree.o + object.o, so we cannot call index_load().
// Instead tree_from_index reads .pes/index directly using fscanf.

#define LOCAL_MAX_INDEX 10000

typedef struct {
    uint32_t  mode;
    ObjectID  hash;
    uint64_t  mtime_sec;
    uint32_t  size;
    char      path[512];
} LocalEntry;

// Read .pes/index into a flat array of LocalEntry.
// Returns entry count, or -1 on error.
static int load_index_local(LocalEntry *entries, int max_entries) {
    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0; // no index yet = 0 entries, not an error

    int count = 0;
    char hex[65];
    while (count < max_entries) {
        LocalEntry *e = &entries[count];
        int ret = fscanf(f, "%o %64s %" SCNu64 " %u %511s\n",
                         &e->mode, hex, &e->mtime_sec, &e->size, e->path);
        if (ret != 5) break;
        // Convert hex string to binary hash
        for (int i = 0; i < HASH_SIZE; i++) {
            unsigned int byte;
            sscanf(hex + i * 2, "%02x", &byte);
            e->hash.hash[i] = (uint8_t)byte;
        }
        count++;
    }
    fclose(f);
    return count;
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

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

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Recursive helper: builds one level of the tree hierarchy.
// entries: slice of index entries sharing the same directory prefix up to `depth`
// depth:   how many path components to skip to reach the current level
static int write_tree_level(LocalEntry *entries, int count, int depth, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        // Skip `depth` path components to get to current level
        // e.g. depth=1, path="src/main.c" -> p points at "main.c"
        const char *p = entries[i].path;
        for (int d = 0; d < depth; d++) {
            p = strchr(p, '/');
            if (!p) return -1;
            p++; // skip '/'
        }

        const char *slash = strchr(p, '/');

        if (!slash) {
            // No slash -> plain file at this level
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            size_t name_len = strlen(p);
            if (name_len >= sizeof(te->name)) name_len = sizeof(te->name) - 1;
            memcpy(te->name, p, name_len);
            te->name[name_len] = '\0';
            i++;
        } else {
            // Slash found -> subdirectory at this level
            size_t dir_len = slash - p;
            char dir_name[256];
            if (dir_len >= sizeof(dir_name)) dir_len = sizeof(dir_name) - 1;
            memcpy(dir_name, p, dir_len);
            dir_name[dir_len] = '\0';

            // Group all entries that belong to this same subdirectory
            int j = i;
            while (j < count) {
                const char *q = entries[j].path;
                for (int d = 0; d < depth; d++) {
                    q = strchr(q, '/');
                    if (!q) break;
                    q++;
                }
                if (q && strncmp(q, dir_name, dir_len) == 0 && q[dir_len] == '/')
                    j++;
                else
                    break;
            }

            // Recurse one level deeper for this subdirectory
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, depth + 1, &sub_id) != 0)
                return -1;

            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = 0040000;
            te->hash = sub_id;
            memcpy(te->name, dir_name, dir_len);
            te->name[dir_len] = '\0';

            i = j;
        }
    }

    // Serialize and write this tree level to the object store
    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store. Returns the root tree hash in *id_out.
int tree_from_index(ObjectID *id_out) {
    // Allocate on heap - LOCAL_MAX_INDEX entries is too large for the stack
    LocalEntry *entries = malloc(LOCAL_MAX_INDEX * sizeof(LocalEntry));
    if (!entries) return -1;

    int count = load_index_local(entries, LOCAL_MAX_INDEX);
    if (count <= 0) {
        free(entries);
        return -1;
    }

    int rc = write_tree_level(entries, count, 0, id_out);
    free(entries);
    return rc;
}
