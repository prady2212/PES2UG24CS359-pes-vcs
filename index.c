// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// PROVIDED functions: index_find, index_remove, index_status
// IMPLEMENTED:        index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
// Returns pointer to the entry, or NULL if not found.
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index (unstage it).
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory compared to the index.
// Shows staged, unstaged (modified/deleted), and untracked files.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            // File was deleted from working directory
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: compare mtime and size instead of re-hashing
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip special entries and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            // Check if this file is already in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load the index from .pes/index into memory.
// If the file does not exist (no files staged yet), initializes an empty
// index — this is NOT an error, it just means nothing is staged yet.
// Returns 0 on success, -1 on parse error.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // File doesn't exist yet — empty index, that's fine
        return 0;
    }

    char hex[HASH_HEX_SIZE + 1];
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
        // Parse one line: "<mode> <hex> <mtime> <size> <path>"
        int ret = fscanf(f, "%o %64s %" SCNu64 " %u %511s\n",
                         &e->mode,
                         hex,
                         &e->mtime_sec,
                         &e->size,
                         e->path);
        if (ret != 5) break; // end of file or malformed line
        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }
        index->count++;
    }

    fclose(f);
    return 0;
}

// Helper for qsort — sort index entries by path alphabetically.
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

// Save the index to .pes/index atomically.
// Uses the temp-file-then-rename pattern so a crash mid-write never
// leaves a corrupted index file.
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // Work on a sorted copy — index file is always in alphabetical order
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_index_entries);

    // Write to a temporary file first
    char tmp_path[] = INDEX_FILE ".tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted->count; i++) {
        const IndexEntry *e = &sorted->entries[i];
        hash_to_hex(&e->hash, hex);
        // Format: "<mode-octal> <hex> <mtime> <size> <path>"
        fprintf(f, "%o %s %" PRIu64 " %u %s\n",
                e->mode, hex, e->mtime_sec, e->size, e->path);
    }

    // Flush userspace buffers → sync to disk → atomically replace old index
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp_path, INDEX_FILE);
}

// Stage a file for the next commit.
// Reads the file, stores its contents as a blob object, then updates
// (or creates) its entry in the index with the new hash and metadata.
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // 1. Read the entire file into memory
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    // Allocate at least 1 byte so malloc(0) is never called
    void *contents = malloc(file_size > 0 ? file_size : 1);
    if (!contents) { fclose(f); return -1; }

    size_t bytes_read = fread(contents, 1, file_size, f);
    fclose(f);

    // 2. Write the file contents as a blob to the object store
    //    object_write returns the SHA-256 hash of the stored object
    ObjectID hash;
    if (object_write(OBJ_BLOB, contents, bytes_read, &hash) != 0) {
        free(contents);
        return -1;
    }
    free(contents);

    // 3. Stat the file to capture metadata for fast change detection later
    struct stat st;
    if (lstat(path, &st) != 0) {
        perror(path);
        return -1;
    }

    // 4. Find the existing index entry for this path, or create a new one
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        entry = &index->entries[index->count++];
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
    }

    // 5. Update the entry with the new hash and current file metadata
    entry->hash      = hash;
    entry->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size      = (uint32_t)st.st_size;

    // 6. Persist the updated index atomically
    return index_save(index);
}
