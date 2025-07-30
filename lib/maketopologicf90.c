#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <sys/stat.h>
#include <stdbool.h>


#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define INITIAL_FILE_CAPACITY 1024
#define MAX_FILE_CAPACITY 4096
#define INITIAL_USES_CAPACITY 16
#define MAX_USES_CAPACITY 1024
#define MAX_LINE 1024
#define MAX_MODULE_LEN 128
#define HASH_SIZE 16384
#define CHUNK_SIZE 4096

typedef struct ProjectFile {
    char filename[1024];
    char module_name[MAX_MODULE_LEN];  // lowercase, null-terminated
    int *uses;       // store indices of modules used (indices in files[])
    int uses_count;
    int uses_capacity;
} ProjectFile;

static ProjectFile *files = NULL;
static int file_count = 0;
static int file_capacity = 0;

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

void str_tolower(char *s) {
    while (*s) {
        *s = (char)tolower((unsigned char)*s);
        s++;
    }
}

char *trim(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) *end-- = 0;
    return str;
}

void ensure_file_capacity(void) {
    if (file_count >= file_capacity) {
        int new_capacity = file_capacity == 0 ? INITIAL_FILE_CAPACITY : file_capacity * 2;
        assert(new_capacity <= MAX_FILE_CAPACITY && "Exceeded max number of files");
        ProjectFile *new_files = realloc(files, new_capacity * sizeof(ProjectFile));
        if (!new_files) {
            fprintf(stderr, "realloc failed for files\n");
            exit(1);
        }
        // Initialize new entries
        for (int i = file_capacity; i < new_capacity; i++) {
            new_files[i].uses = NULL;
            new_files[i].uses_capacity = 0;
            new_files[i].uses_count = 0;
            new_files[i].module_name[0] = 0;
            new_files[i].filename[0] = 0;
        }
        files = new_files;
        file_capacity = new_capacity;
    }
}

void ensure_uses_capacity(int idx) {
    ProjectFile *f = &files[idx];
    if (f->uses_count >= f->uses_capacity) {
        int new_capacity = f->uses_capacity == 0 ? INITIAL_USES_CAPACITY : f->uses_capacity * 2;
        assert(new_capacity <= MAX_USES_CAPACITY && "Exceeded max uses per file");
        int *new_uses = realloc(f->uses, new_capacity * sizeof(int));
        if (!new_uses) {
            fprintf(stderr, "realloc failed for uses\n");
            exit(1);
        }
        f->uses = new_uses;
        f->uses_capacity = new_capacity;
    }
}

void add_used_module(int file_idx, int used_idx) {
    ProjectFile *f = &files[file_idx];
    for (int i = 0; i < f->uses_count; i++) {
        if (f->uses[i] == used_idx) return;
    }
    ensure_uses_capacity(file_idx);
    f->uses[f->uses_count++] = used_idx;
}

int strcmp_case_insensitive(char const *a, char const *b)
{
    for (;; a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0 || !*a)
            return d;
    }
}

void parse_module_definition(char *line, int idx) {
    char *p = trim(line);
    if (strncasecmp(p, "module ", 7) == 0) {
        char *modname = p + 7;
        modname = trim(modname);
        str_tolower(modname);
        strncpy(files[idx].module_name, modname, MAX_MODULE_LEN - 1);
        files[idx].module_name[MAX_MODULE_LEN - 1] = '\0';
        hash_insert(modname, idx);
    }
}

void parse_use_statement(char *line, int idx) {
    char *p = trim(line);
    if (strncasecmp(p, "use", 3) != 0) return;
    p += 3;
    while (*p == ' ' || *p == ',' || *p == ':') p++;
    if (*p == ':' && *(p + 1) == ':') {
        p += 2;
        while (*p == ' ') p++;
    }
    char modname[MAX_MODULE_LEN];
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && *p != ',' && i < MAX_MODULE_LEN - 1)
        modname[i++] = (char)tolower((unsigned char)*p++);
    modname[i] = '\0';
    int dep_idx = hash_lookup(modname);
    if (dep_idx != -1) add_used_module(idx, dep_idx);
}


void parse_include_statement(char *line, int idx) {
    char header_name[MAX_MODULE_LEN];
    char *p = trim(line);
    if (strncmp(p, "#include", 8) == 0) {
        char *start = strchr(p, '"');
        if (start) {
            char *end = strchr(start + 1, '"');
            if (end && (end - start - 1) < MAX_MODULE_LEN) {
                strncpy(header_name, start + 1, end - start - 1);
                header_name[end - start - 1] = '\0';
                int dep_idx = hash_lookup(header_name);
                if (dep_idx != -1) add_used_module(idx, dep_idx);
            }
        }
    }
}

void parse_line_for_dep(char *line, const char *filename, int file_idx, int mode) {
    // Fortran
    if (strstr(filename, ".f") || strstr(filename, ".F") ) {
        if (mode == 0) parse_module_definition(line, file_idx);
        else parse_use_statement(line, file_idx);
    }

    // C
    if (strstr(filename, ".c")) {
        if (mode == 1) parse_include_statement(line, file_idx);
    }
}

void read_files_in_dir(const char *dir_path, int recursive) {
#ifdef _WIN32
    WIN32_FIND_DATA fd;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", dir_path);

    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Could not open directory: %s\n", dir_path);
        exit(1);
    }
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir_path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursive) {
                read_files_in_dir(path, recursive);
            }
        } else {
            if (strstr(fd.cFileName, ".f") || strstr(fd.cFileName, ".c") || strstr(fd.cFileName, ".F")) {
                ensure_file_capacity();
                strncpy(files[file_count].filename, path, sizeof(files[file_count].filename) - 1);
                files[file_count].filename[sizeof(files[file_count].filename) - 1] = '\0';
                file_count++;
            }
        }
    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);
#else
    DIR *d = opendir(dir_path);
    if (!d) {
        perror(dir_path);
        exit(1);
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(path, &st) == -1) {
            perror(path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (recursive) {
                read_files_in_dir(path, recursive);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (strstr(de->d_name, ".f") || strstr(de->d_name, ".c") || strstr(de->d_name, ".F") ) {
                ensure_file_capacity();
                strncpy(files[file_count].filename, path, sizeof(files[file_count].filename) - 1);
                files[file_count].filename[sizeof(files[file_count].filename) - 1] = '\0';
                file_count++;
            }
        }
    }
    closedir(d);
#endif
}


//Buffered read of the files in chunks before parsing for the depedencies.
void process_modules_in_file(const char *filename, int file_idx, int mode) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) { perror(filename); exit(1); }

    char *buffer = malloc(CHUNK_SIZE * 2);
    if (!buffer) { fprintf(stderr, "malloc failed\n"); exit(1); }

    size_t leftover = 0;
    while (1) {
        size_t n = fread(buffer + leftover, 1, CHUNK_SIZE, fp);
        if (n == 0 && leftover == 0) break;

        size_t total = leftover + n;
        buffer[total] = '\0';

        size_t line_start = 0;
        for (size_t i = 0; i < total; i++) {
            if (buffer[i] == '\n') {
                buffer[i] = '\0';
                parse_line_for_dep(buffer + line_start, filename, file_idx, mode);
                line_start = i + 1;
            }
        }

        leftover = total - line_start;
        if (leftover > 0) memmove(buffer, buffer + line_start, leftover);

        if (n == 0) {
            if (leftover > 0) {
                buffer[leftover] = '\0';
                parse_line_for_dep(buffer, filename, file_idx, mode);
            }
            break;
        }
    }

    free(buffer);
    fclose(fp);
}


void process_directories(){
    // First pass: definitions
    for (int i = 0; i < file_count; i++) {
        process_modules_in_file(files[i].filename, i, 0);
    }

    // Second pass: usages
    for (int i = 0; i < file_count; i++) {
        process_modules_in_file(files[i].filename, i, 1);
    }
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
        ProjectFile *f = &files[i];
        for (int j = 0; j < f->uses_count; j++) {
            int dep = f->uses[j];
            ensure_adj_capacity(dep);
            adj[dep].edges[adj[dep].count++] = i;
            in_degree[i]++;
        }
    }
}

//Topological sort of files based on module dependencies
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


//split_dirs - splits comma separated list of directories into array
//list: string containing comma-separated directory list
//count: pointer to store number of directories parsed
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
        fprintf(stderr, "No files found to process.\n");
        return 1;
    }

    //Process the files
    process_directories();

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
            printf("%s:", files[idx].filename);
            ProjectFile *f = &files[idx];
            for (int u = 0; u < f->uses_count; u++) {
                int dep_idx = f->uses[u];
                printf(" %s", files[dep_idx].filename);
            }
            printf("\n");
        }
    } else {
        // Print build order (filenames only)
        for (int i = 0; i < file_count; i++) {
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