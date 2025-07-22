#ifndef FORTUNA_TOML_H
#define FORTUNA_TOML_H

#include "toml.h"

typedef struct {
    toml_table_t *table;
    char *data;  // Owned buffer of TOML file content
} fortuna_toml_t;

int fortuna_toml_load(const char *path, fortuna_toml_t *cfg);
void fortuna_toml_free(fortuna_toml_t *cfg);

//Returns a NULL-terminated array of strings (caller must free all)
char **fortuna_toml_get_array(fortuna_toml_t *cfg, const char *key_path);

//Get a string from key_path, or NULL if not found (do NOT free)
const char *fortuna_toml_get_string(fortuna_toml_t *cfg, const char *key_path);

//Get a matrix of strings from a toml file
char ***extract_string_matrix(toml_table_t* cfg, const char* key, int* rows, int* cols);

//Get keys of an "unknown" subfield
char **fortuna_toml_get_table_keys_list(fortuna_toml_t *cfg, const char *table_path);
char *fortuna_toml_resolve_target_name(fortuna_toml_t *cfg, const char *table_path, const char *key);

#endif // FORTUNA_TOML_H
