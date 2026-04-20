// commit.c — Commit creation and history traversal
//
// Commit object text format:
//
//   tree <64-char-hex-hash>
//   parent <64-char-hex-hash>        <- omitted for the first commit
//   author <name> <unix-timestamp>
//   committer <name> <unix-timestamp>
//
//   <commit message>
//
// There is a blank line between the headers and the message.
//
// PROVIDED functions: commit_parse, commit_serialize, commit_walk,
//                     head_read, head_update
// IMPLEMENTED:        commit_create

#include "commit.h"
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

// Forward declarations from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Parse raw commit object data into a Commit struct.
// Returns 0 on success, -1 on parse error.
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    (void)len;
    const char *p = (const char *)data;
    char hex[HASH_HEX_SIZE + 1];

    // Parse "tree <hex>\n"
    if (sscanf(p, "tree %64s\n", hex) != 1) return -1;
    if (hex_to_hash(hex, &commit_out->tree) != 0) return -1;
    p = strchr(p, '\n') + 1;

    // Optionally parse "parent <hex>\n" (absent on first commit)
    if (strncmp(p, "parent ", 7) == 0) {
        if (sscanf(p, "parent %64s\n", hex) != 1) return -1;
        if (hex_to_hash(hex, &commit_out->parent) != 0) return -1;
        commit_out->has_parent = 1;
        p = strchr(p, '\n') + 1;
    } else {
        commit_out->has_parent = 0;
    }

    // Parse "author <name> <timestamp>\n"
    char author_buf[256];
    uint64_t ts;
    if (sscanf(p, "author %255[^\n]\n", author_buf) != 1) return -1;
    // The timestamp is the last space-separated token in the author line
    char *last_space = strrchr(author_buf, ' ');
    if (!last_space) return -1;
    ts = (uint64_t)strtoull(last_space + 1, NULL, 10);
    *last_space = '\0'; // trim timestamp off author string
    snprintf(commit_out->author, sizeof(commit_out->author), "%s", author_buf);
    commit_out->timestamp = ts;
    p = strchr(p, '\n') + 1; // skip author line
    p = strchr(p, '\n') + 1; // skip committer line
    p = strchr(p, '\n') + 1; // skip blank line

    // Everything remaining is the commit message
    snprintf(commit_out->message, sizeof(commit_out->message), "%s", p);
    return 0;
}

// Serialize a Commit struct into the text format for object_write.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int commit_serialize(const Commit *commit, void **data_out, size_t *len_out) {
    char tree_hex[HASH_HEX_SIZE + 1];
    char parent_hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit->tree, tree_hex);

    char buf[8192];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "tree %s\n", tree_hex);
    if (commit->has_parent) {
        hash_to_hex(&commit->parent, parent_hex);
        n += snprintf(buf + n, sizeof(buf) - n, "parent %s\n", parent_hex);
    }
    n += snprintf(buf + n, sizeof(buf) - n,
                  "author %s %" PRIu64 "\n"
                  "committer %s %" PRIu64 "\n"
                  "\n"
                  "%s",
                  commit->author, commit->timestamp,
                  commit->author, commit->timestamp,
                  commit->message);

    *data_out = malloc(n + 1);
    if (!*data_out) return -1;
    memcpy(*data_out, buf, n + 1);
    *len_out = (size_t)n;
    return 0;
}

// Walk commit history from HEAD to the root commit, calling `callback`
// for each commit in newest-to-oldest order.
int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID id;
    if (head_read(&id) != 0) return -1;

    while (1) {
        ObjectType type;
        void *raw;
        size_t raw_len;
        if (object_read(&id, &type, &raw, &raw_len) != 0) return -1;

        Commit c;
        int rc = commit_parse(raw, raw_len, &c);
        free(raw);
        if (rc != 0) return -1;

        callback(&id, &c, ctx);

        if (!c.has_parent) break; // reached root commit
        id = c.parent;
    }
    return 0;
}

// Read the commit hash that HEAD currently points to.
// Follows symbolic refs: HEAD → refs/heads/main → commit hash.
// Returns 0 on success, -1 if the repo has no commits yet.
int head_read(ObjectID *id_out) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0'; // strip trailing newline

    char ref_path[512];
    if (strncmp(line, "ref: ", 5) == 0) {
        // Symbolic ref — follow it to the actual branch file
        snprintf(ref_path, sizeof(ref_path), "%s/%s", PES_DIR, line + 5);
        f = fopen(ref_path, "r");
        if (!f) return -1; // branch file missing = no commits yet
        if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
        fclose(f);
        line[strcspn(line, "\r\n")] = '\0';
    }
    return hex_to_hash(line, id_out);
}

// Update HEAD (or the branch it points to) to a new commit hash.
// Uses atomic temp-file + rename to avoid partial writes.
int head_update(const ObjectID *new_commit) {
    FILE *f = fopen(HEAD_FILE, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
    fclose(f);
    line[strcspn(line, "\r\n")] = '\0';

    // Determine which file to update: branch file or HEAD directly
    char target_path[520];
    if (strncmp(line, "ref: ", 5) == 0) {
        snprintf(target_path, sizeof(target_path), "%s/%s", PES_DIR, line + 5);
    } else {
        // Detached HEAD — update HEAD itself
        snprintf(target_path, sizeof(target_path), "%s", HEAD_FILE);
    }

    char tmp_path[528];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target_path);

    f = fopen(tmp_path, "w");
    if (!f) return -1;

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(new_commit, hex);
    fprintf(f, "%s\n", hex);

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp_path, target_path);
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Create a new commit from the current staging area (index).
//
// Steps:
//   1. Build a tree object from the index (snapshot of staged files)
//   2. Read current HEAD as parent (empty on first commit)
//   3. Fill in author, timestamp, message
//   4. Serialize and write the commit object
//   5. Update HEAD to point to the new commit
//
// Returns 0 on success, -1 on error.
int commit_create(const char *message, ObjectID *commit_id_out) {
    Commit c;
    memset(&c, 0, sizeof(c));

    // Step 1: Build the tree from staged index entries
    // tree_from_index writes all blob/tree objects and returns the root hash
    if (tree_from_index(&c.tree) != 0) {
        fprintf(stderr, "error: nothing staged to commit\n");
        return -1;
    }

    // Step 2: Read current HEAD to use as the parent commit
    // head_read returns -1 if there are no commits yet (first commit)
    if (head_read(&c.parent) == 0) {
        c.has_parent = 1; // normal commit — has a parent
    } else {
        c.has_parent = 0; // first commit — no parent
    }

    // Step 3: Fill in commit metadata
    snprintf(c.author, sizeof(c.author), "%s", pes_author());
    c.timestamp = (uint64_t)time(NULL);
    snprintf(c.message, sizeof(c.message), "%s", message);

    // Step 4: Serialize the Commit struct to text and write to object store
    void *data;
    size_t len;
    if (commit_serialize(&c, &data, &len) != 0) return -1;

    ObjectID commit_id;
    int rc = object_write(OBJ_COMMIT, data, len, &commit_id);
    free(data);
    if (rc != 0) return -1;

    // Step 5: Move the branch pointer (HEAD) to our new commit
    if (head_update(&commit_id) != 0) return -1;

    // Pass the new commit ID back to the caller if they want it
    if (commit_id_out) *commit_id_out = commit_id;

    // Print a summary line like Git does: "[branch shortHash] message"
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&commit_id, hex);
    printf("[main %.7s] %s\n", hex, message);
    return 0;
}
