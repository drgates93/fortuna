#ifndef CLI_ARGS_H
#define CLI_ARGS_H

#include <stddef.h>

#define MAX_ARG_LEN 256
#define HASHMAP_SIZE 32

typedef struct kvpair {
    char *key;
    int   idx;
    struct kvpair *next;
} kvpair_t;

typedef struct {
    kvpair_t *buckets[HASHMAP_SIZE];
} hashmap_t;

typedef struct {
    hashmap_t args_map;
} cli_args_t;

// Initialize the hashmap
void hashmap_init(hashmap_t *map);

// Insert a key (string) into the hashmap; returns 0 on success, -1 on failure
int hashmap_put(hashmap_t *map, const char *key, const int idx);

// Check if key exists; returns 1 if found, 0 otherwise
int hashmap_contains(hashmap_t *map, const char *key);

//Check if key exists with some index
int hashmap_contains_key_and_index(hashmap_t *map, const char *key, const int idx);

//Return key for a given index in the hashmap
const char* return_key_for_index(hashmap_t *map, const int idx);

//Return index for a given key in the hashmap
int return_index_for_key(hashmap_t *map, const char* query);

// Free all memory used by hashmap
void hashmap_free(hashmap_t *map);

// Initialize CLI args parser
void cli_args_init(cli_args_t *args);

// Free CLI args parser resources
void cli_args_free(cli_args_t *args);

// Parse argv into the hashmap; returns 0 on success, nonzero on error
int cli_args_parse(cli_args_t *args, int argc, char **argv);

#endif // CLI_ARGS_H
