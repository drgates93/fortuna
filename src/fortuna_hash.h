#ifndef FORTUNA_HASH_H
#define FORTUNA_HASH_H

#include <stdbool.h>

#define HASH_TABLE_SIZE 1024

typedef struct DependentNode {
    char *dependent;
    struct DependentNode *next;
} DependentNode;

typedef struct FileNode {
    char *filename;
    unsigned int file_hash;
    DependentNode *dependents;
    struct FileNode *next;
} FileNode;

typedef struct HashEntry {
    char *filename;
    unsigned int file_hash;
    struct HashEntry *next;
} HashEntry;

// Hash functions
unsigned int hash_file_fnv1a(const char *filename);
unsigned int str_hash(const char *str);

// Node creation
DependentNode *new_dependent_node(const char *dependent);
FileNode *new_file_node(const char *filename);

// Hashtable access
FileNode *find_file_node(const char *filename, FileNode *hash_table[]);
FileNode *get_or_create_file_node(const char *filename, FileNode *hash_table[]);

// Dependency management
void add_dependent(FileNode *file, const char *dependent);
void parse_line(char *line, FileNode *hash_table[]);
int parse_dependency_file(const char *filename, FileNode *hash_table[]);

// Hashtable operations
void print_hashtable(FileNode *hash_table[]);
void free_all(FileNode *hash_table[]);

// Loading and saving hashes
int load_hash_table(const char* dependency_list, FileNode *hash_table[]);
int save_hashes(const char *filename, FileNode *hash_table[]);

// Previous hash table management
void load_prev_hashes(const char *filename, HashEntry *prev_hash_table[]);
int file_is_unchanged(const char *filename, unsigned int current_hash, HashEntry *prev_hash_table[]);
void prune_unchanged_files(FileNode *hash_table[], HashEntry *prev_hash_table[]);
void prune_obsolete_cached_entries(HashEntry *prev_hash_table[], FileNode *hash_table[]);
void free_prev_hash_table(HashEntry *prev_hash_table[]);

// Dependency checking
DependentNode* get_dependents_if_changed(const char *filename, FileNode *hash_table[], HashEntry *prev_hash_table[]);

//Rebuild check
// Check if a file is already marked for rebuild
bool is_in_rebuild_list(const char *filename, FileNode *rebuild_list);

// Add file to the rebuild list if not already present
void append_to_rebuild_list(FileNode **rebuild_list, const char *filename);

// Recursively mark a file and its dependents for rebuild,
// removing visited nodes from the hash_table
void mark_dependents_for_rebuild(const char *filename, FileNode *hash_table[], FileNode **rebuild_list, int *rebuild_cnt);

//Generic function to insert node
void insert_node(const char *filename, FileNode *hash_table[]);

//Generic function to check node. 
int node_is_in_the_hashmap(const char *filename, FileNode *hash_table[]);

#endif // FORTUNA_HASH_H