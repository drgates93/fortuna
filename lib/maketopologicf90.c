#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>


//Defines for the data.
#define INITIAL_FILE_CAPACITY 1024
#define MAX_FILE_CAPACITY     100000
#define MAX_FILENAME_LEN      512
#define INITIAL_USES_CAPACITY 16
#define MAX_USES_CAPACITY     16384
#define MAX_LINE              1024
#define MAX_MODULE_LEN        128
#define HASH_SIZE             16384


//Hashtable defintions for the third type.
typedef struct HashEntry {
    char key[MAX_MODULE_LEN];
    int value;
    struct HashEntry *next;
} HashEntry;

static HashEntry *hash_table[HASH_SIZE] = {0};

unsigned int fnv1a_hash(const char *str) {
    const unsigned int FNV_prime = 16777619U;
    unsigned int hash = 2166136261U;
    while (*str) {
        hash ^= (unsigned char)(*str++);
        hash *= FNV_prime;
    }
    return hash % HASH_SIZE;
}

unsigned int hash_func(const char *str) {
    return fnv1a_hash(str);
}

void hash_insert(const char *key, int value) {
    unsigned int h = hash_func(key);
    HashEntry *entry = malloc(sizeof(HashEntry));
    if (!entry) {
        fprintf(stderr, "malloc failed in hash_insert\n");
        exit(1);
    }
    strcpy(entry->key, key);
    entry->value = value;
    entry->next = hash_table[h];
    hash_table[h] = entry;
}

int hash_lookup(const char *key) {
    unsigned int h = hash_func(key);
    for (HashEntry *e = hash_table[h]; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) return e->value;
    }
    return -1;
}

void free_hash_table(void) {
    for (int i = 0; i < HASH_SIZE; i++) {
        HashEntry *e = hash_table[i];
        while (e) {
            HashEntry *next = e->next;
            free(e);
            e = next;
        }
        hash_table[i] = NULL;
    }
}

typedef struct FileEntry {
    char filename[MAX_FILENAME_LEN];
    char **module_names;   // dynamic array of module names (lowercased) for Fortran, header names for C
    int module_count;
    int module_capacity;
    int *uses;             // indices of dependencies
    int uses_count;
    int uses_capacity;
    bool is_fortran;
    bool is_header;  // Need to track the header files, but note that they are not actually rebuilt. 
} FileEntry;

static FileEntry *files = NULL;
static int file_count = 0;
static int file_capacity = 0;

static inline int strncasecmp_prefix(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (tolower(ca) != tolower(cb) || ca == '\0' || cb == '\0')
            return (int)(tolower(ca) - tolower(cb));
    }
    return 0;
}

static inline int is_use_stmt(const char *ptr) {
    return strncasecmp_prefix(ptr, "use", 3) == 0 && isspace((unsigned char)ptr[3]);
}

static inline int is_module_stmt(const char *ptr) {
    return strncasecmp_prefix(ptr, "module", 6) == 0 && isspace((unsigned char)ptr[6]);
}

static inline char *extract_token_lower(char *ptr, char *dst, size_t dst_size) {
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;
    size_t i = 0;
    while (*ptr && !isspace((unsigned char)*ptr) && *ptr != ',' && *ptr != '\n' && i < dst_size - 1) {
        dst[i++] = (char)tolower((unsigned char)*ptr++);
    }
    dst[i] = 0;
    return ptr;
}

//Check if it is a space token
static inline int is_space_token(unsigned char* s){
    return (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v');
}

static inline char *trim(char *str) {
    unsigned char *s = (unsigned char *)str;
    unsigned char *end;

    // Trim leading space
    while (*s && is_space_token(s)) s++;
    if (*s == 0) return (char*)s;

    // Find end of string
    end = s;
    while (*end) end++;
    end--;

    // Trim trailing space
    while (end > s && is_space_token(end)) end--;
    *(end + 1) = '\0';
    return (char*) s;
}

void ensure_file_capacity(void) {
    if (file_count >= file_capacity) {
        int new_cap = file_capacity ? file_capacity * 2 : 128;
        files = realloc(files, new_cap * sizeof(FileEntry));
        assert(files);
        for (int i = file_capacity; i < new_cap; i++) {
            files[i].module_names = NULL;
            files[i].module_capacity = 0;
            files[i].module_count = 0;
            files[i].uses = NULL;
            files[i].uses_capacity = 0;
            files[i].uses_count = 0;
        }
        file_capacity = new_cap;
    }
}

void add_module_name(FileEntry *fe, const char *name) {
    if (fe->module_count >= fe->module_capacity) {
        fe->module_capacity = fe->module_capacity ? fe->module_capacity * 2 : 4;
        fe->module_names = realloc(fe->module_names, fe->module_capacity * sizeof(char *));
        assert(fe->module_names);
    }
    fe->module_names[fe->module_count] = strdup(name);
    assert(fe->module_names[fe->module_count]);
    fe->module_count++;
}

void add_used_file(int file_idx, int dep_idx) {
    FileEntry *f = &files[file_idx];
    for (int i = 0; i < f->uses_count; i++) {
        if (f->uses[i] == dep_idx) return;
    }
    if (f->uses_count >= f->uses_capacity) {
        f->uses_capacity = f->uses_capacity ? f->uses_capacity * 2 : INITIAL_USES_CAPACITY;
        f->uses = realloc(f->uses, f->uses_capacity * sizeof(int));
        assert(f->uses);
    }
    f->uses[f->uses_count++] = dep_idx;
}



static void parse_fortran_dependencies(int file_idx) {
    char line[1024], token[MAX_MODULE_LEN];
    FILE *fp = fopen(files[file_idx].filename, "r");
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        char *ptr = line;
        while (*ptr && isspace((unsigned char)*ptr)) ptr++;

        if (is_use_stmt(ptr)) {
            ptr += 3;
            ptr = extract_token_lower(ptr, token, sizeof(token));
            int idx = hash_lookup(token);
            if (idx != -1) {
                add_used_file(file_idx, idx);
            }
        } else if (is_module_stmt(ptr)) {
            ptr += 6;
            ptr = extract_token_lower(ptr, token, sizeof(token));
            add_module_name(&files[file_idx], token);
            hash_insert(token, file_idx);
        }
    }
    fclose(fp);
}

int add_used_header(int file_idx, const char *name) {
    FileEntry *f = &files[file_idx];
    // Check duplicates
    for (int i = 0; i < f->module_count; i++) {
        if (strcmp(f->module_names[i], name) == 0)
            return 0; // already added
    }

    // Expand module_names
    if (f->module_count >= f->module_capacity) {
        int new_capacity = f->module_capacity ? f->module_capacity * 2 : 8;
        char **new_names = realloc(f->module_names, new_capacity * sizeof(char *));
        if (!new_names) {
            perror("realloc");
            exit(1);
        }
        f->module_names = new_names;
        f->module_capacity = new_capacity;
    }

    f->module_names[f->module_count] = strdup(name);
    if (!f->module_names[f->module_count]) {
        perror("strdup");
        exit(1);
    }
    f->module_count++;
    return 1;
}
static void parse_c_dependencies(int file_idx) {
    char line[1024], token[MAX_FILENAME_LEN];
    FILE *fp = fopen(files[file_idx].filename, "r");
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        char *ptr = line;
        while (*ptr && isspace((unsigned char)*ptr)) ptr++;
        if (strncmp(ptr, "#include", 8) == 0) {
            ptr += 8;
            while (*ptr && isspace((unsigned char)*ptr)) ptr++;
            if (*ptr == '"') {
                ptr++;
                size_t j = 0;
                while (*ptr && *ptr != '"' && j < sizeof(token) - 1) {
                    token[j++] = *ptr++;
                }
                token[j] = '\0';

                // Look for matching tracked header files only.
                for (int k = 0; k < file_count; k++) {
                    const char *fname = strrchr(files[k].filename, '/');
                    fname = fname ? fname + 1 : files[k].filename;
                    if (strcmp(fname, token) == 0) {
                        add_used_file(file_idx, k);
                        break;
                    }
                }
            }
        }
    }
    fclose(fp);
}
    

void parse_file_dependencies(void) {
    for (int i = 0; i < file_count; i++) {
        if (files[i].is_fortran) {
            parse_fortran_dependencies(i);
        } else {
            parse_c_dependencies(i);
        }
    }
}

void initialize_file_entry(FileEntry *entry, const char *filepath, 
                          bool is_fortran_flag, bool is_header_flag) {
    strncpy(entry->filename, filepath, sizeof(entry->filename) - 1);
    entry->filename[sizeof(entry->filename) - 1] = '\0';
    entry->is_fortran      = is_fortran_flag;
    entry->is_header       = is_header_flag;
    entry->module_names    = NULL;
    entry->module_count    = 0;
    entry->module_capacity = 0;
    entry->uses            = NULL;
    entry->uses_count      = 0;
    entry->uses_capacity   = 0;
    return;
}

void read_files_in_dir(const char *dir_path, int recursive) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        perror(dir_path);
        exit(1);
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' && (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) continue;

        char full_path[MAX_FILENAME_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == -1) continue;

        if (S_ISDIR(st.st_mode)) {
            if (recursive) read_files_in_dir(full_path, recursive);
        } else if (S_ISREG(st.st_mode)) {
            size_t name_len = strlen(entry->d_name);
            bool is_fortran = (name_len >= 4 && (strcasecmp(entry->d_name + name_len - 4, ".f90") == 0 
                                             ||  strcasecmp(entry->d_name + name_len - 4, ".for") == 0
                                             ||  strcasecmp(entry->d_name + name_len - 4, ".f")   == 0));
            bool is_c       = (name_len >= 2 && (strcasecmp(entry->d_name + name_len - 2, ".c") == 0));
            bool is_header  = (name_len >= 2 && (strcasecmp(entry->d_name + name_len - 2, ".h") == 0));

            if (is_fortran || is_c || is_header) {
                ensure_file_capacity();
                FileEntry *file_entry = &files[file_count];
                initialize_file_entry(file_entry, full_path, is_fortran, is_header);
                file_count++;
            }
        }
    }
    closedir(dir);
}

typedef struct {
    int *edges;
    int count;
    int capacity;
} AdjList;

static AdjList *adj = NULL;
static int *in_degree = NULL;

void ensure_adj_capacity(int u) {
    if (adj[u].count >= adj[u].capacity) {
        int new_capacity = adj[u].capacity == 0 ? 4 : adj[u].capacity * 2;
        int *new_edges = realloc(adj[u].edges, new_capacity * sizeof(int));
        if (!new_edges) {
            fprintf(stderr, "realloc failed for adjacency list\n");
            exit(1);
        }
        adj[u].edges = new_edges;
        adj[u].capacity = new_capacity;
    }
}

void build_graph(void) {
    adj = calloc(file_count, sizeof(AdjList));
    in_degree = calloc(file_count, sizeof(int));
    if (!adj || !in_degree) {
        fprintf(stderr, "calloc failed for graph\n");
        exit(1);
    }
    for (int i = 0; i < file_count; i++) {
        adj[i].edges = NULL;
        adj[i].capacity = 0;
        adj[i].count = 0;
        in_degree[i] = 0;
    }
    for (int i = 0; i < file_count; i++) {
        FileEntry *f = &files[i];
        for (int j = 0; j < f->uses_count; j++) {
            int dep = f->uses[j];
            ensure_adj_capacity(dep);
            adj[dep].edges[adj[dep].count++] = i;
            in_degree[i]++;
        }
    }
}

//Topological sort of files based on module dependencies
//Using Khadane's algorithm.
//  Returns 1 if we could sort
//  Returns 0 if we detected a cycle. 
int topologic_sort(int *sorted, int *sorted_len) {
    int *queue = malloc(file_count * sizeof(int));
    if (!queue) {
        fprintf(stderr, "malloc failed for topo queue\n");
        exit(1);
    }
    int front = 0, back = 0;
    for (int i = 0; i < file_count; i++) {
        if (in_degree[i] == 0) queue[back++] = i;
    }
    int count = 0;
    while (front < back) {
        int u = queue[front++];
        sorted[count++] = u;
        for (int i = 0; i < adj[u].count; i++) {
            int v = adj[u].edges[i];
            in_degree[v]--;
            if (in_degree[v] == 0) queue[back++] = v;
        }
    }
    free(queue);
    if (count != file_count) {
        return 0;  // cycle detected
    }
    *sorted_len = count;
    return 1;
}

/**
 * split_dirs - splits comma separated list of directories into array
 * list: string containing comma-separated directory list
 * count: pointer to store number of directories parsed
 *
 * Returns dynamically allocated array of strings (each dynamically allocated)
 * Caller must free strings and array.
 */
char **split_dirs(char *list, int *count) {
    int capacity = 8;
    char **dirs = malloc(capacity * sizeof(char *));
    if (!dirs) {
        fprintf(stderr, "malloc failed in split_dirs\n");
        exit(1);
    }
    *count = 0;

    char *token = strtok(list, ",");
    while (token) {
        char *dir = trim(token);
        if (*dir != 0) {
            if (*count >= capacity) {
                capacity *= 2;
                char **new_dirs = realloc(dirs, capacity * sizeof(char *));
                if (!new_dirs) {
                    fprintf(stderr, "realloc failed in split_dirs\n");
                    exit(1);
                }
                dirs = new_dirs;
            }
            dirs[*count] = strdup(dir);
            if (!dirs[*count]) {
                fprintf(stderr, "strdup failed in split_dirs\n");
                exit(1);
            }
            (*count)++;
        }
        token = strtok(NULL, ",");
    }
    return dirs;
}

/**
 * free_dirs - free array of directory strings returned by split_dirs
 */
void free_dirs(char **dirs, int count) {
    if (!dirs) return;
    for (int i = 0; i < count; i++) free(dirs[i]);
    free(dirs);
}

/**
 * print_help - prints usage information
 */
void print_help(const char *progname) {
    printf(
        "Usage: %s [-d dirs] [-D dirs] [-m] [-h]\n"
        "\n"
        "Scans Fortran .f90 source files to determine module dependencies,\n"
        "then outputs the topologic build order of modules.\n"
        "\n"
        "Flags:\n"
        "  -d DIRS    Comma-separated list of directories to scan non-recursively.\n"
        "             Only one -d flag allowed.\n"
        "  -D DIRS    Comma-separated list of directories to scan recursively.\n"
        "             Only one -D flag allowed.\n"
        "  -m         Print a Makefile dependency list instead of build order.\n"
        "  -h         Show this help message.\n"
        "\n"
        "If neither -d nor -D is specified, defaults to scanning 'src' non-recursively.\n"
    , progname);
}

int main(int argc, char **argv) {
    /*
    Program: Fortran module dependency analyzer and topologic build order printer.

    Usage:
      -d DIRS   Comma-separated list of directories to scan non-recursively.
                Only one -d flag allowed.
      -D DIRS   Comma-separated list of directories to scan recursively.
                Only one -D flag allowed.
      -m        Print a Makefile dependency list instead of build order.
      -h        Show this help message.

    Description:
      Scans .f90 files in given directories to detect Fortran modules and their
      'use' dependencies, then computes a topologic order for building modules.
      Can output either the ordered list of files to build or a Makefile
      dependency list suitable for build systems.

    Notes:
      - Repeated use of -d or -D flags is an error.
      - Directories in the flags are comma-separated and will be scanned in the order
        given.
      - If neither -d nor -D is specified, defaults to scanning 'src' non-recursively.

    Author:
        Drake Gates
    */

    //Allocate the direcotry pointers.
    char *d_dirs_str = NULL;
    char *D_dirs_str = NULL;
    int print_make_deps = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            if (d_dirs_str != NULL) {
                fprintf(stderr, "Error: -d flag specified more than once\n");
                return 1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -d flag requires an argument\n");
                return 1;
            }
            d_dirs_str = argv[++i];
        } else if (strcmp(argv[i], "-D") == 0) {
            if (D_dirs_str != NULL) {
                fprintf(stderr, "Error: -D flag specified more than once\n");
                return 1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -D flag requires an argument\n");
                return 1;
            }
            D_dirs_str = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0) {
            print_make_deps = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    int d_count = 0, D_count = 0;
    char **d_dirs = NULL, **D_dirs = NULL;

    if (d_dirs_str) {
        d_dirs = split_dirs(d_dirs_str, &d_count);
        if (d_count == 0) {
            fprintf(stderr, "Error: -d flag requires at least one directory\n");
            return 1;
        }
    }

    if (D_dirs_str) {
        D_dirs = split_dirs(D_dirs_str, &D_count);
        if (D_count == 0) {
            fprintf(stderr, "Error: -D flag requires at least one directory\n");
            return 1;
        }
    }

    if (!d_dirs && !D_dirs) {
        // Default to "src" non-recursive
        d_count = 1;
        d_dirs = malloc(sizeof(char *));
        if (!d_dirs) {
            fprintf(stderr, "malloc failed for default dir\n");
            return 1;
        }
        d_dirs[0] = strdup("src");
        if (!d_dirs[0]) {
            fprintf(stderr, "strdup failed for default dir\n");
            return 1;
        }
    }

    // Read all files in all directories
    for (int i = 0; i < d_count; i++) {
        read_files_in_dir(d_dirs[i], 0);
    }
    for (int i = 0; i < D_count; i++) {
        read_files_in_dir(D_dirs[i], 1);
    }

    //Free the memory
    free_dirs(d_dirs, d_count);
    free_dirs(D_dirs, D_count);

    if (file_count == 0) {
        fprintf(stderr, "No .f90 files found to process.\n");
        return 1;
    }

    //Find all the modules prefaced by module module_name
    //Allows for multiple modules in the file and stores them in 
    //the hash table as (filename, index in filelist)

    //Find all the actual used module name / #include statements and then lookup if it's
    //in the hashtable. If it is, we store it in the used file list. 
    //That is, we store the index of the file 
    // [1,0,0,0,0,0] => Single module
    // [1,1,0,0,0,0] => Second file depends on the first if we have an index in element 0. 
    parse_file_dependencies();



    //Build the adjacency graph by the files the module name appears in.
    //This is the entire graph of dependencies when that list is topologically sorted. 
    build_graph();

    int *sorted = malloc(file_count * sizeof(int));
    if (!sorted) {
        fprintf(stderr, "malloc failed for sorted\n");
        return 1;
    }

    //Topolgocially sort the graph by the adjacency graph.
    if (!topologic_sort(sorted, &file_count)) {
        fprintf(stderr, "Error: cyclic dependency detected, no valid build order\n");
        free(sorted);
        free_hash_table();
        for (int i = 0; i < file_count; i++) free(files[i].uses);
        free(files);
        free(adj);
        free(in_degree);
        return 1;
    }

    if (print_make_deps) {
        // Print Makefile dependency list: filename: dependencies filenames...
        for (int i = 0; i < file_count; i++) {
            int idx = sorted[i];
            if (files[idx].is_header) continue;
            printf("%s:", files[idx].filename);
            FileEntry *f = &files[idx];
            for (int u = 0; u < f->uses_count; u++) {
                int dep_idx = f->uses[u];
                printf(" %s", files[dep_idx].filename);
            }

            // Print module names and headers (strings) that are dependencies but not tracked as files
            for (int m = 0; m < f->module_count; m++) {
                printf(" %s", f->module_names[m]);
            }
            printf("\n");
        }
    } else {
        // Print build order (filenames only)
        for (int i = 0; i < file_count; i++) {
            if (files[sorted[i]].is_header) continue;
            printf("%s\n", files[sorted[i]].filename);
        }
    }

    free(sorted);
    free_hash_table();
    for (int i = 0; i < file_count; i++) free(files[i].uses);
    free(files);
    free(adj);
    free(in_degree);

    return 0;
}
