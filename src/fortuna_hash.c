#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fortuna_hash.h"
#include "fortuna_helper_fn.h"
#include "blake3.h"

#define MAX_LINE 1024
#define HASH_TABLE_SIZE 1024

//Use blake3 for hashing the file
unsigned int hash_file_blake3(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        return 0;
    }

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    unsigned char buffer[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        blake3_hasher_update(&hasher, buffer, bytes_read);
    }

    fclose(file);

    uint8_t hash[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&hasher, hash, BLAKE3_OUT_LEN);

    // Reduce to 32-bit hash for hashtable comparisons
    unsigned int reduced_hash =
        ((unsigned int)hash[0] << 24) |
        ((unsigned int)hash[1] << 16) |
        ((unsigned int)hash[2] << 8)  |
        ((unsigned int)hash[3]);

    return reduced_hash;
}

// Simple hash function for strings (djb2)
unsigned int str_hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash & (HASH_TABLE_SIZE-1); //Assumes hash table size is a power of 2. 
}

// Create new dependent node
DependentNode *new_dependent_node(const char *dependent) {
    DependentNode *node = malloc(sizeof(DependentNode));
    if (!node) {
        print_error("malloc failed in hashmap creating new node");
        exit(EXIT_FAILURE);
    }
    node->dependent = strdup(dependent);
    node->next = NULL;
    return node;
}

// Create new file node with file hash
FileNode *new_file_node(const char *filename) {
    FileNode *node = malloc(sizeof(FileNode));
    if (!node) {
        print_error("malloc failed in hashmap creating new node");
        exit(EXIT_FAILURE);
    }
    node->filename = strdup(filename);
    node->file_hash = hash_file_blake3(filename);
    node->dependents = NULL;
    node->next = NULL;
    return node;
}

// Find file node in hashtable by filename
FileNode *find_file_node(const char *filename, FileNode *hash_table[]) {
    unsigned int index = str_hash(filename);
    FileNode *curr = hash_table[index];
    while (curr) {
        if (strcmp(curr->filename, filename) == 0) {
            return curr;
        }
        curr = curr->next;
    }
    return NULL;
}

// Get or create file node in hashtable
FileNode *get_or_create_file_node(const char *filename, FileNode *hash_table[]) {
    FileNode *node = find_file_node(filename, hash_table);
    if (node) return node;

    node = new_file_node(filename);
    unsigned int index = str_hash(filename);
    node->next = hash_table[index];
    hash_table[index] = node;
    return node;
}

// Add dependent to file's dependents list if not already present
void add_dependent(FileNode *file, const char *dependent) {
    DependentNode *curr = file->dependents;
    while (curr) {
        if (strcmp(curr->dependent, dependent) == 0) {
            return; // already added
        }
        curr = curr->next;
    }
    DependentNode *newnode = new_dependent_node(dependent);
    newnode->next = file->dependents;
    file->dependents = newnode;
}

//Simple check if a node is in the hashmap already. 
int node_is_in_the_hashmap(const char *filename, FileNode *hash_table[]){
    FileNode *node = find_file_node(filename, hash_table);
    return !(node == NULL);
}

//Insert node
void insert_node(const char *filename, FileNode *hash_table[]) {
    FileNode *node = find_file_node(filename, hash_table);
    if (node) return;
    node = new_file_node(filename);
    unsigned int index = str_hash(filename);
    node->next = hash_table[index];
    hash_table[index] = node;
    return;
}

// Parse a dependency line, update hashtable with dependents
void parse_line(char *line, FileNode *hash_table[]) {
    char *colon = strchr(line, ':');
    if (!colon) return;

    *colon = 0;
    char *target = line;
    char *deps = colon + 1;

    // Trim whitespace
    while (*target == ' ' || *target == '\t') target++;
    char *end = target + strlen(target) - 1;
    while (end > target && (*end == ' ' || *end == '\t')) {
        *end = 0;
        end--;
    }

    // Ensure target is in the graph
    FileNode *target_node = get_or_create_file_node(target, hash_table);
    if(target_node == NULL){
        print_error("Unable to insert node into hash table when parsing the hash.dep file");
        exit(1);
    }

    // Parse dependencies and add edges
    char *dep = strtok(deps, " \t");
    while (dep) {
        FileNode *dep_node = get_or_create_file_node(dep, hash_table);
        add_dependent(dep_node, target);  // dep_node -> target
        dep = strtok(NULL, " \t");
    }
}

int parse_dependency_file(const char *filename, FileNode *hash_table[]) {
    // Initialize table to NULLs
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        hash_table[i] = NULL;
    }

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        print_error("Error opening dependency file");
        return 0;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0; // strip newline
        if (strlen(line) == 0) continue;
        parse_line(line, hash_table);
    }

    fclose(fp);
    return 1;
}

void print_hashtable(FileNode *hash_table[]) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileNode *node = hash_table[i];
        while (node) {
            printf("[TABLE] %s -> hash: %u\n", node->filename, node->file_hash);
            DependentNode *d = node->dependents;
            while (d) {
                printf("    depends on -> %s\n", d->dependent);
                d = d->next;
            }
            node = node->next;
        }
    }
}

// Free all allocated memory
void free_all(FileNode *hash_table[]) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileNode *curr = hash_table[i];
        while (curr) {
            free(curr->filename);
            DependentNode *d = curr->dependents;
            while (d) {
                DependentNode *tmp = d;
                free(d->dependent);
                d = d->next;
                free(tmp);
            }
            FileNode *tmp = curr;
            curr = curr->next;
            free(tmp);
        }
        hash_table[i] = NULL;
    }
}

int load_hash_table(const char* dependency_list, FileNode *hash_table[]) {
    FILE *fp = fopen(dependency_list, "r");
    if (!fp) return -1;

    // Initialize
    for (int i = 0; i < HASH_TABLE_SIZE; i++)
        hash_table[i] = NULL;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0) continue;
        parse_line(line, hash_table);
    }
    fclose(fp);
    return 0;
}

int save_hashes(const char *filename, FileNode *hash_table[]) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        print_error("Failed to open file for saving hashes");
        return 0;
    }

    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileNode *curr = hash_table[i];
        while (curr) {
            fprintf(fp, "%s %u\n", curr->filename, curr->file_hash);
            curr = curr->next;
        }
    }

    fclose(fp);
    return 1;
}

void load_prev_hashes(const char *filename, HashEntry *prev_hash_table[]) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        // No cache file yet so we just return.
        return;
    }

    char fname[512];
    unsigned int hash;

    for (int i = 0; i < HASH_TABLE_SIZE; i++) prev_hash_table[i] = NULL;

    while (fscanf(fp, "%511s %u", fname, &hash) == 2) {
        HashEntry *entry = malloc(sizeof(HashEntry));
        entry->filename  = strdup(fname);
        entry->file_hash = hash;

        unsigned int idx = str_hash(fname);
        entry->next = prev_hash_table[idx];
        prev_hash_table[idx] = entry;
    }

    fclose(fp);
}

int file_is_unchanged(const char *filename, unsigned int current_hash, HashEntry *prev_hash_table[]) {
    unsigned int idx = str_hash(filename);
    HashEntry *entry = prev_hash_table[idx];
    while (entry) {
        if (strcmp(entry->filename, filename) == 0) {
            return entry->file_hash == current_hash;
        }
        entry = entry->next;
    }
    return 0;
}

void prune_unchanged_files(FileNode *hash_table[], HashEntry *prev_hash_table[]) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        FileNode **pprev = &hash_table[i];
        FileNode *curr = hash_table[i];

        while (curr) {
            if (file_is_unchanged(curr->filename, curr->file_hash, prev_hash_table)) {
                // Remove the node
                *pprev = curr->next;

                // Free memory
                free(curr->filename);
                DependentNode *d = curr->dependents;
                while (d) {
                    DependentNode *tmp = d;
                    free(d->dependent);
                    d = d->next;
                    free(tmp);
                }

                FileNode *tmp = curr;
                curr = curr->next;
                free(tmp);
            } else {
                pprev = &curr->next;
                curr = curr->next;
            }
        }
    }
}

void prune_obsolete_cached_entries(HashEntry *prev_hash_table[], FileNode *hash_table[]) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        HashEntry **pprev = &prev_hash_table[i];
        HashEntry *curr = prev_hash_table[i];

        while (curr) {
            // Check if this filename is in the current dependency graph
            if (!find_file_node(curr->filename, hash_table)) {
                // Not found → delete from prev_hash_table
                *pprev = curr->next;

                free(curr->filename);
                free(curr);
                curr = *pprev;
            } else {
                pprev = &curr->next;
                curr = curr->next;
            }
        }
    }
}

void free_prev_hash_table(HashEntry *prev_hash_table[]) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        HashEntry *curr = prev_hash_table[i];
        while (curr) {
            HashEntry *tmp = curr;
            free(curr->filename);
            curr = curr->next;
            free(tmp);
        }
        prev_hash_table[i] = NULL;
    }
}

DependentNode* get_dependents_if_changed(const char *filename, FileNode *hash_table[], HashEntry *prev_hash_table[]) {
    FileNode *curr = find_file_node(filename, hash_table);
    if (!curr) return NULL;  // Not in current graph

    unsigned int current_hash = curr->file_hash;

    // Look up in old hash table
    unsigned int idx = str_hash(filename);
    HashEntry *entry = prev_hash_table[idx];
    while (entry) {
        if (strcmp(entry->filename, filename) == 0) {
            if (entry->file_hash != current_hash) {
                return curr->dependents;  // File changed
            } else {
                return NULL;  // File unchanged
            }
        }
        entry = entry->next;
    }

    // If not found in previous table, it's new -> mark as changed
    return curr->dependents;
}


// You can use this to avoid reprocessing files
bool is_in_rebuild_list(const char *filename, FileNode *rebuild_list) {
    FileNode *curr = rebuild_list;
    while (curr) {
        if (strcmp(curr->filename, filename) == 0) return true;
        curr = curr->next;
    }
    return false;
}

// Add file to rebuild list
void append_to_rebuild_list(FileNode **rebuild_list, const char *filename) {
    if (is_in_rebuild_list(filename, *rebuild_list)) return;

    //Otherwise, add to the end. Note, the list is topologically sorted
    //So we want to add them in order. 
    FileNode *node = new_file_node(filename);
    if (*rebuild_list == NULL) {
        *rebuild_list = node;
    } else {
        FileNode *curr = *rebuild_list;
        while (curr->next != NULL) {
            curr = curr->next;
        }
        curr->next = node;
    }
}

// Recursive marking + hash table pruning
void mark_dependents_for_rebuild(const char *filename, FileNode *hash_table[], FileNode **rebuild_list, int *rebuild_cnt) {
    unsigned int idx = str_hash(filename);
    FileNode **pprev = &hash_table[idx];
    FileNode *node   = hash_table[idx];
    while (node) {

        if (strcmp(node->filename, filename) == 0) {
            append_to_rebuild_list(rebuild_list, node->filename);

            //Increment the number of elements in the rebuild list. 
            (*rebuild_cnt)++; 

            // Mark dependents recursively
            DependentNode *d = node->dependents;
            while (d) {
                if (!is_in_rebuild_list(d->dependent, *rebuild_list)) {
                    mark_dependents_for_rebuild(d->dependent, hash_table, rebuild_list, rebuild_cnt);
                }
                d = d->next;
            }

            // Remove node from hash table
            *pprev = node->next;

            // Free memory
            free(node->filename);
            DependentNode *dep = node->dependents;
            while (dep) {
                DependentNode *tmp = dep;
                free(dep->dependent);
                dep = dep->next;
                free(tmp);
            }
            FileNode *tmp = node;
            node = node->next;
            free(tmp);
        } else {
            pprev = &node->next;
            node = node->next;
        }
    }
}