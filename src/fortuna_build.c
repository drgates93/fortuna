#include "fortuna_build.h"
#include "fortuna_toml.h"
#include "fortuna_hash.h"
#include "fortuna_threads.h"
#include "fortuna_helper_fn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#include <limits.h>
#endif

static int dir_exists(const char *path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
#endif
}

//Figure out the path options.
#ifdef _WIN32
    #define PATH_SEP '\\'
    //Generate the hash file cache. 
    const char* hash_cache_file = ".cache\\hash.dep";

    //Depenency list 
    const char* deps_file = ".cache\\topo.dep";
#else
    #define PATH_SEP '/'
    //Generate the hash file cache. 
    const char* hash_cache_file = ".cache/hash.dep";

    //Depenency list 
    const char* deps_file = ".cache/topo.dep";
#endif

int make_dir(const char *path) {
    int result = mkdir(path);
    if (result == 0) return 0;
    if (errno == EEXIST) return 0; // already exists is fine
    return -1;
}

int count_files_in_directory(const char *path) {
    int count = 0;

#ifdef _WIN32
    WIN32_FIND_DATA fd;
    char search_path[MAX_PATH];
    snprintf(search_path, MAX_PATH, "%s\\*", path);

    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return -1;  // error
    }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            count++;
        }
    } while (FindNextFile(hFind, &fd));

    FindClose(hFind);

#else
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    char fullpath[1024];

    dir = opendir(path);
    if (dir == NULL) {
        return -1;  // error
    }

    while ((entry = readdir(dir)) != NULL) {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
            count++;
        }
    }

    closedir(dir);
#endif

    return count;
}

#define CHUNK_SIZE 4096

// Trim leading/trailing spaces
char *trim(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) *end-- = 0;
    return str;
}

// Extract module name if it's a module definition (not module procedure)
char *parse_module_definition(char *line) {
    char *p = trim(line);

    if (*p == '\0' || *p == '!') return NULL;

    if (strncasecmp(p, "module procedure", 16) == 0)
        return NULL;

    if (strncasecmp(p, "module ", 7) == 0) {
        char *modname = p + 7;

        // Trim trailing comments or extra tokens
        while (*modname && isspace((unsigned char)*modname)) modname++;

        // Copy name until space or comment
        char namebuf[256];
        int i = 0;
        while (*modname && !isspace((unsigned char)*modname) && *modname != '!' && i < 255) {
            namebuf[i++] = tolower(*modname++);
        }
        namebuf[i] = '\0';

        if (i == 0) return NULL;

        // Allocate and return "modulename.mod"
        char *result = malloc(strlen(namebuf) + 5);
        if (!result) return NULL;
        sprintf(result, "%s.mod", namebuf);
        return result;
    }

    return NULL;
}

char *parse_line_for_dep(char *line, const char *filename) {
    if (strstr(filename, ".f") || strstr(filename, ".F")) {
        return parse_module_definition(line);
    }
    return NULL;
}

// Buffered file reader that returns module .mod name
char *get_module_filename(const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) { perror(filename); exit(1); }

    char *buffer = malloc(CHUNK_SIZE + 1);
    char *linebuf = malloc(CHUNK_SIZE * 2);
    if (!buffer || !linebuf) {
        fprintf(stderr, "malloc failed\n");
        exit(1);
    }

    size_t line_len = 0;
    char *modname = NULL;

    while (!feof(fp)) {
        size_t n = fread(buffer, 1, CHUNK_SIZE, fp);
        buffer[n] = '\0';

        size_t i = 0;
        while (i < n) {
            char *newline = memchr(buffer + i, '\n', n - i);
            size_t chunk_len = newline ? (newline - (buffer + i) + 1) : (n - i);

            memcpy(linebuf + line_len, buffer + i, chunk_len);
            line_len += chunk_len;

            if (newline) {
                linebuf[line_len] = '\0';
                modname = parse_line_for_dep(linebuf, filename);
                if (modname) goto done;
                line_len = 0;
                i += chunk_len;
            } else {
                i += chunk_len;
            }
        }
    }

    // Final incomplete line
    if (line_len > 0) {
        linebuf[line_len] = '\0';
        modname = parse_line_for_dep(linebuf, filename);
    }

done:
    free(buffer);
    free(linebuf);
    fclose(fp);
    return modname;  // NULL if none found
}

//This allows for nested src files in any number of directories
//to be parsed into just the filename and thus we can put them in the
//object directory. We only rebuild if the src changes, not the obj. 
char *get_last_path_segment(const char *path) {
    const char *end = path + strlen(path);
    const char *p = end;
    while (p > path && *(p - 1) != '/' && *(p - 1) != '\\') p--;
    return strdup(p); 
}

// Worker thread: launches a process per thread
void compile_system_worker(void *arg) {
    char *cmd = (char *)arg;
    int ret = launch_process(cmd,NULL);
    if (ret != 0) {
        print_error("Compilation failed: %s\n");
        return;
    }
    return;
}

int file_exists(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file) {
        fclose(file);
        return 1;  // File exists
    }
    return 0;  // File does not exist
}

static char *run_command_capture(const char *cmd) {
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        print_error("Failed to run command.");
        return NULL;
    }
    char *buffer = NULL;
    size_t size = 0;
    char chunk[512];

    while (fgets(chunk, sizeof(chunk), pipe)) {
        size_t len = strlen(chunk);
        char *newbuf = realloc(buffer, size + len + 1);
        if (!newbuf) {
            free(buffer);
            pclose(pipe);
            print_error("Memory allocation error.");
            return NULL;
        }
        buffer = newbuf;
        memcpy(buffer + size, chunk, len);
        size += len;
        buffer[size] = '\0';
    }
    pclose(pipe);
    return buffer;
}

// Add flag to unique list if not already there
int add_unique_flag(char ***list, int *count, const char *flag) {
    for (int i = 0; i < *count; i++) {
        if (strcmp((*list)[i], flag) == 0) {
            return 0; // already present
        }
    }
    char **newlist = realloc(*list, sizeof(char*) * (*count + 1));
    if (!newlist) return -1;
    *list = newlist;
    (*list)[*count] = strdup(flag);
    if (!(*list)[*count]) return -1;
    (*count)++;
    return 0;
}

// Case-insensitive string compare for extension match
int strcmp_case_insensitive(const char *ext, const char *target) {
    while (*ext && *target) {
        if (tolower((unsigned char)*ext) != tolower((unsigned char)*target)) {
            return 1; // not equal
        }
        ext++;
        target++;
    }
    return (*ext == '\0' && *target == '\0') ? 0 : 1;
}

//Truncate the string at the file extension for fortran and C
int truncate_file_name_at_file_extension(const char* rel_file_path){
    char *ext = strrchr(rel_file_path, '.');

    //Skip header files
    if(strcmp_case_insensitive(ext, ".h") == 0) return 1;

    //Check that the extension is valid.
    if (ext && (strcmp_case_insensitive(ext, ".f90") == 0 
            ||  strcmp_case_insensitive(ext, ".for") == 0
            ||  strcmp_case_insensitive(ext, ".f")   == 0
            ||  strcmp_case_insensitive(ext, ".f77") == 0
            ||  strcmp_case_insensitive(ext, ".c")   == 0)){
        *ext = '\0';
    }
    return 0;
}

char *join_flags_array(char **flags_array) {
    if (!flags_array) return NULL;

    size_t total_len = 0;
    int count = 0;

    for (int i = 0; flags_array[i]; i++) {
        total_len += strlen(flags_array[i]) + 1; // +1 for space or null terminator
        count++;
    }

    if (count == 0) return NULL;

    char *joined = malloc(total_len);
    if (!joined) return NULL;

    joined[0] = '\0';
    for (int i = 0; i < count; i++) {
        strcat(joined, flags_array[i]);
        if (i < count - 1) strcat(joined, " ");
    }

    return joined;
}


//Libary build
int build_library(char** sources, int src_count, const char* obj_dir, const char* lib_name){
    char ar_cmd[4096] = {0};
    size_t ar_pos = 0;

    ar_pos += snprintf(ar_cmd + ar_pos, sizeof(ar_cmd) - ar_pos, "ar rcs %s%c%s ","lib",PATH_SEP,lib_name);

    for (int i = 0; i < src_count; i++) {
        const char *src      = sources[i];
        const char *rel_path = get_last_path_segment(src);
        if(truncate_file_name_at_file_extension(rel_path)) continue;

        //Write the "object" to the obj directory. For simplified building, 
        //we eliminate the relative path to the src in the obj dir and link against
        //just a list of all .o files we need in one place. This is much cleaner. 
        char obj_path[512];
        snprintf(obj_path, sizeof(obj_path), "%s%c%s.o", obj_dir, PATH_SEP, rel_path);
        ar_pos += snprintf(ar_cmd + ar_pos, sizeof(ar_cmd) - ar_pos, " %s", obj_path);
    }
    print_info(ar_cmd);
    int ret = launch_process(ar_cmd,NULL);
    if (ret != 0) {
        print_error("Linking failed.");
        return -1;
    }
    return 0;
}


int build_target_incremental_core(fortuna_toml_t *cfg,
                                   char *maketop_cmd,
                                   const char *compiler,
                                   const char *flags_str,
                                   const char *obj_dir,
                                   const char *mod_dir,
                                   const char *target_name,
                                   char **exclude_files,
                                   int parallel_build,
                                   int incremental_build,
                                   const int lib_only,
                                   const int run_flag,
                                   const int is_c) {


    //Set the return code
    int return_code = 0;

    //Still need the list of source files to link against.
    char *topo_src= run_command_capture(maketop_cmd);

    //Allocate the hashmaps.
    FileNode*  cur_map[HASH_TABLE_SIZE]  = {NULL};
    HashEntry* prev_map[HASH_TABLE_SIZE] = {NULL};
    
    //Now we get the exclusion list (if it exists)
    FileNode*  exclusion_map[HASH_TABLE_SIZE]  = {NULL};
    if(exclude_files){
        for(int i = 0; exclude_files[i]; i++){
            insert_node(exclude_files[i],exclusion_map);
        }
    }

    //Count the object files
    int obj_cnt = count_files_in_directory(obj_dir);

    //From the list of topologically sorted files, we need to parse them
    //properly as they are a single string. 
    char *line           = strtok(topo_src, "\n");
    char **sources       = NULL;
    int src_count        = 0;
    char **tmp           = NULL;
    while (line) {
        tmp = realloc(sources, sizeof(char *)*(src_count + 1));
        if (!tmp) {
            print_error("Memory allocation error in parsing sources");
            free(topo_src);
            return_code = -1;
            goto defer_hashmaps;
        }
                
        //Skip if this file is in the exclusion list
        if(node_is_in_the_hashmap(line,exclusion_map)){
            line = strtok(NULL, "\n");
            continue;
        }

        //Otherwise, add to the list of sources!
        sources = tmp;
        sources[src_count] = strdup(line);
        src_count++;
        line = strtok(NULL, "\n");

    }

    //Trigger a full rebuild because we don't have a match for the number of
    //object files and the number of src files. We can't just rebuild some obj
    //files and not others because this case would be weird enough that maybe a file
    //was renamed to a different one and now we have a problem. 
    if(src_count != obj_cnt){
        incremental_build = 0;
        parallel_build    = 0;
    }

    //Define the rebuild count
    int rebuild_cnt = 0;

    // Compile each source in parallel if asked.
    thread_t *threads;

    //The most threads we can have is all of them so this is a safe allocation. 
    if(parallel_build) threads = (thread_t*)malloc(src_count*sizeof(thread_t));

    //Allocate the characetr buffers
    char compile_cmd[2048];
    char obj_file[1024];
    char mod_file[1024];

    //For the incremental build, we parse the dependency chain and rebuild. 
    if(incremental_build){
        strcat(maketop_cmd," -m");
        char *topo_make = run_command_capture(maketop_cmd);

        //Write the new list to a file and then reload it. 
        FILE* depedency_chain = fopen(deps_file ,"w+");
        fprintf(depedency_chain,"%s",topo_make);
        fclose(depedency_chain);

        //Parse the dependency file first
        int res = parse_dependency_file(deps_file,cur_map);
        if(!res){
            print_error("Failed to make hash table of dependency graph\n");
            return_code = -1;
            goto defer_core;
        }
            
        //If hash file exists, load it and compare
        if (file_exists(hash_cache_file)) {
            load_prev_hashes(hash_cache_file,prev_map);
            save_hashes(hash_cache_file,cur_map);
            prune_obsolete_cached_entries(prev_map,cur_map);
        }else{
            print_error("Cannot do an incremental build with no history!");
            print_error("Check that the .cache/hash.dep file exists.\n");
            return_code = -1;
            goto defer_core;
        }

        //Check the hash table for what we need to build.
        FileNode *rebuild_list = NULL;
        for (int i = 0; i < HASH_TABLE_SIZE; i++) {
            FileNode *node = cur_map[i];
            while (node) {
                // Check if this node has changed
                if (!file_is_unchanged(node->filename, node->file_hash, prev_map)) {
                    // It changed — mark its dependents
                    mark_dependents_for_rebuild(node->filename, cur_map, &rebuild_list, &rebuild_cnt);
                }

                //Check the whether we built the mod file successfully on a previous run. 
                const char* module_name = get_module_filename(node->filename);
                if(module_name) {

                    //If the mod files does not exist, we need to rebuild it.
                    //This is because either the previous compilation failed
                    //or the files were deleted/moved. Either way, we need it! 
                    snprintf(mod_file, sizeof(mod_file), "%s%c%s", mod_dir, PATH_SEP, module_name);
                    if(!file_exists(mod_file)) {
                        append_to_rebuild_list(&rebuild_list, node->filename);
                        rebuild_cnt++;
                    }
                }
                node = node->next;
            }
        }

        //Rebuild required if the rebuild list is not empty.
        //Otherwise, we jump to our memory cleanup.
        if(rebuild_list == NULL && lib_only == 0) {
            if(!run_flag) print_info("Nothing to build");
            return_code = 0;
            goto defer_core;
        }

        //Compile each source only if it changed and needs to be rebuilt. 
        int node_cnt   = 0;
        FileNode *curr = rebuild_list;
        while(curr && node_cnt != rebuild_cnt){
            const char *src = curr->filename;

            //Check the exclusion list here. This can break a build,
            //but that is the correct behavior if asked. 
            if(node_is_in_the_hashmap(src,exclusion_map)) {
                curr = curr->next;
                continue;
            }

            //Otherwise continue on 
            const char *rel_path = get_last_path_segment(src);
            if(truncate_file_name_at_file_extension(rel_path)) continue;

            //Write the object file name to a string
            snprintf(obj_file, sizeof(obj_file), "%s%c%s.o", obj_dir, PATH_SEP, rel_path);

            //Generate the compile command
            if(!is_c){
                snprintf(compile_cmd, sizeof(compile_cmd), "%s %s -J%s -c %s -o %s",
                         compiler, flags_str, mod_dir, src, obj_file);
            }else{
                snprintf(compile_cmd, sizeof(compile_cmd), "%s %s -c %s -o %s",
                         compiler, flags_str, src, obj_file);
            }

            //Print the info
            print_info(compile_cmd);

            //Parallel build logic.
            if(!parallel_build){
                int ret = launch_process(compile_cmd,NULL);
                if (ret != 0) {
                    print_error("Compilation failed.");
                    free(topo_make);
                    free(rebuild_list); 
                    return_code = -1;
                    goto defer_core;
                }
            }else{
                //Copy the command to a per thread buffer to avoid conflicts. 
                char *compile_cmd_ts = strdup(compile_cmd);
                if (thread_create(&threads[rebuild_cnt++], compile_system_worker, compile_cmd_ts) != 0) {
                    print_error("Failed to create thread");
                    free(topo_make);
                    free(rebuild_list); 
                    return_code = -1;
                    goto defer_core;
                }
            }

            //Next node
            curr = curr->next;
            node_cnt++;
        }

        //Free the topo_make here and then free the rebuild list. 
        free(topo_make);
        free(rebuild_list);
    }else{
        for (int i = 0; i < src_count; i++) {

            const char *src      = sources[i];
            const char *rel_path = get_last_path_segment(src);
            if(truncate_file_name_at_file_extension(rel_path)) continue;

            //Skip files if requested. 
            if(node_is_in_the_hashmap(src,exclusion_map)) {
                continue;
            }

            snprintf(obj_file, sizeof(obj_file), "%s%c%s.o", obj_dir, PATH_SEP, rel_path);

            if(!is_c){
                snprintf(compile_cmd, sizeof(compile_cmd), "%s %s -J%s -c %s -o %s",
                         compiler, flags_str, mod_dir, src, obj_file);
            }else{
                snprintf(compile_cmd, sizeof(compile_cmd), "%s %s -c %s -o %s",
                         compiler, flags_str, src, obj_file);
            }

            //Print the info
            print_info(compile_cmd);

            //Check if we issue a sys call or spawn a thread to do it.
            if(!parallel_build){
                int ret = launch_process(compile_cmd,NULL);
                if (ret != 0) {
                    print_error("Compilation failed.");
                    return_code = -1;
                    goto defer_core;
                }
            }else{
                //Copy the command to a per thread buffer to avoid conflicts. 
                char *compile_cmd_ts = strdup(compile_cmd);
                if (thread_create(&threads[i], compile_system_worker, compile_cmd_ts) != 0) {
                    print_error("Failed to create thread");
                    return_code = -1;
                    goto defer_core;
                }
            }
        }
    }

    
    //If waiting on a parallel build, we finish up here by joining all the threads.
    if(parallel_build){
        int num_threads_spawned = (incremental_build) ? rebuild_cnt : src_count;
        for (int i = 0; i < num_threads_spawned; i++) {
            thread_join(threads[i]);
        }
    }

    //Check if we are building a library or not.
    const char* lib = fortuna_toml_get_string(cfg, "lib.target");
    if(lib != NULL && lib_only == 0) {
        if(build_library(sources,src_count,obj_dir,lib) == -1){
            print_error("Failed to link library. Check if ar is installed and if the paths are correct.");
            return_code = -1;
            goto defer_core;
        }
    }else if(lib != NULL && lib_only == 1){
        //If lib only, we skip linking the executable.
        goto skip_linking;
    }else if(lib == NULL && lib_only == 1){
        //If lib == NULL but we requested a lib only run, error out. 
        print_error("No target lib found in Fortuna.toml");
        return_code = -1;
        goto defer_core;
    }

    // Link
    char link_cmd[4096] = {0};
    size_t link_pos = 0;
    link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, "%s %s", compiler, flags_str);

    //Link all the objects files. We check if the file exists to prevent issues.
    char obj_path[512];
    for (int i = 0; i < src_count; i++) {
        const char *src      = sources[i];
        const char *rel_path = get_last_path_segment(src);
        if(truncate_file_name_at_file_extension(rel_path)) continue;

        //Write the "object" to the obj directory. For simplified building, 
        //we eliminate the relative path to the src in the obj dir and link against
        //just the object in the specified folder.
        snprintf(obj_path, sizeof(obj_path), "%s%c%s.o", obj_dir, PATH_SEP, rel_path);

        //Check if the obj file actually built and/or still exists.
        if(!file_exists(obj_path)){
            char msg[256];
            snprintf(msg, sizeof(msg), "Object file %s does not exist.", obj_path);
            print_error(msg);
            return_code = -1;
            goto defer_core;
        }

        //Add to the command string.
        link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, " %s", obj_path);
    }

    //Link with the libraries (if they exist).
    char **source_libs = fortuna_toml_get_array(cfg, "library.source-libs");
    if (!source_libs) source_libs = NULL;
    if (source_libs ) {
        for (int i = 0; source_libs[i]; i++) {
            link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, " %s", source_libs[i]);
        }
    }

    //Build the final link command
    link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, " -o %s", target_name);

    //Execute the link command
    print_info(link_cmd);
    int ret = launch_process(link_cmd,NULL);
    if (ret != 0) {
        print_error("Linking failed.");
        return_code = -1;
        goto defer_core;
    }


    //Skip linking jump if we don't want to link.
    skip_linking:
    print_ok("Built Successfully");


    //If we aren't on an incremental build, we need to dump everything
    //to the .cache files here for the first run. 
    if(!incremental_build){
         //For the incremental build, we parse the files!
        strcat(maketop_cmd," -m");
        char *topo_make = run_command_capture(maketop_cmd);

        //Build the dependency table and save to file.
        FILE* depedency_chain = fopen(deps_file ,"w+");
        fprintf(depedency_chain,"%s",topo_make);
        fclose(depedency_chain);

        //Load it into memory or the hashmap is empty on save. 
        parse_dependency_file(deps_file,cur_map);

        //Save hashes for the current state of the project. 
        save_hashes(hash_cache_file,cur_map);
    }

defer_core:
    if(parallel_build) free(threads);

defer_hashmaps:
    free_prev_hash_table(prev_map);
    free_all(cur_map);
    free_all(exclusion_map);

    return return_code;
}



int fortuna_build_project_incremental(const int parallel_build, 
                                      const int incremental_build_override, 
                                      const int lib_only, 
                                      const int run_flag) {


    //Set the return code
    int ret_code = 0;

    //Check if we can do an incremental build.
    int incremental_build = file_exists(hash_cache_file);

    //If we allow the override, then we want to rebuild all, so incremental build is disabled.
    if(incremental_build_override == 0) incremental_build = 0;

    //Load the toml file.
    const char* toml_path = "Fortuna.toml";
    fortuna_toml_t cfg = {0};
    if (fortuna_toml_load(toml_path, &cfg) != 0) {
        print_error("Failed to load Fortuna.toml.");
        return -1;
    }

    const char *target = fortuna_toml_get_string(&cfg, "build.target");
    if (!target) {
        print_error("Missing 'build.target' in config.");
        fortuna_toml_free(&cfg);
        return -1;
    }

    char *compiler = (char *)fortuna_toml_get_string(&cfg, "build.compiler");
    if (!compiler) {
        print_error("Invalid compiler selected");
        return -1;
    }

    char **flags_array = fortuna_toml_get_array(&cfg, "build.flags");
    if (!flags_array) {
        print_error("Missing or empty 'build.flags' in config.");
        fortuna_toml_free(&cfg);
        return -1;
    }

    //Join the flags into a single string.
    char *flags_str = join_flags_array(flags_array);

    //Load the location to place the obj and mod files. 
    const char *obj_dir = fortuna_toml_get_string(&cfg, "build.obj_dir");
    const char *mod_dir = fortuna_toml_get_string(&cfg, "build.mod_dir");

    int is_c = 0;
    if(strcasecmp(compiler,"clang") == 0 || 
       strcasecmp(compiler,"gcc")   == 0){
        is_c = 1;
    }

    //Check if the dirs exist if requested.
    if (obj_dir && !dir_exists(obj_dir)) {
        print_error("Object directory does not exist.");
        goto defer_build;
    }
    if (mod_dir && !dir_exists(mod_dir)) {
        print_error("Module directory does not exist.");
        goto defer_build;
    }

    if(!obj_dir) {
        obj_dir = "obj";
        if(make_dir("obj") == -1){
            print_error("Unable to make obj directory. Check folder permissions.");
            return -1;
        }
    }
        
    if(!mod_dir && !is_c) {
        mod_dir = "mod";
        if(make_dir("mod") == -1){
            print_error("Unable to make mod directory. Check folder permissions.");
            return -1;
        }
    }else if(mod_dir && is_c){
        mod_dir = "";
    }

    //Check the directories.
    char **deep_dirs    = fortuna_toml_get_array(&cfg, "search.deep");
    char **shallow_dirs = fortuna_toml_get_array(&cfg, "search.shallow");

    // // Check for multiple targets
    // char **keys = fortuna_toml_get_table_keys_list(&cfg, "bin");
    // if (keys) {
    //     for (int i = 0; keys[i]; i++) {
    //         char *sub_target_name  = fortuna_toml_resolve_target_name(&cfg, "bin", keys[i]);
    //         if (!sub_target_name ) continue;
    //         char key_buf[256];

    //         snprintf(key_buf, sizeof(key_buf), "bin.%s.flags", sub_target_name);
    //         char **sub_flags_array = fortuna_toml_get_array(&cfg, key_buf);
    //         char *sub_flags_str = join_flags_array(sub_flags_array);

    //         snprintf(key_buf, sizeof(key_buf), "bin.%s.obj_dir", sub_target_name);
    //         const char *sub_obj_dir = fortuna_toml_get_string(&cfg, key_buf);
    //         if (!sub_obj_dir) sub_obj_dir = obj_dir;

    //         snprintf(key_buf, sizeof(key_buf), "bin.%s.mod_dir", sub_target_name);
    //         const char *sub_mod_dir = fortuna_toml_get_string(&cfg, key_buf);
    //         if (!sub_mod_dir) sub_mod_dir = mod_dir;

    //         snprintf(key_buf, sizeof(key_buf), "bin.%s.search.deep", sub_target_name);
    //         char **sub_deep_dirs = fortuna_toml_get_array(&cfg, key_buf);

    //         snprintf(key_buf, sizeof(key_buf), "bin.%s.search.shallow", sub_target_name);
    //         char **sub_shallow_dirs = fortuna_toml_get_array(&cfg, key_buf);

    //         snprintf(key_buf, sizeof(key_buf), "bin.%s.exclude", sub_target_name);
    //         char **sub_exclude_files = fortuna_toml_get_array(&cfg, key_buf);

    //             // Build maketopologicf90 command
    //             char maketop_cmd[1024] = {0};
    //         #ifdef _WIN32
    //             strcat(maketop_cmd, "bin\\maketopologicf90.exe");
    //         #else
    //             strcat(maketop_cmd, "./bin/maketopologicf90.exe");
    //         #endif
    //         if (sub_deep_dirs) {
    //             strcat(maketop_cmd, " -D ");
    //             for (int j = 0; sub_deep_dirs[j]; j++) {
    //                 strcat(maketop_cmd, sub_deep_dirs[j]);
    //                 if (sub_deep_dirs[j + 1]) strcat(maketop_cmd, ",");
    //             }
    //         }
    //         if (sub_shallow_dirs) {
    //             strcat(maketop_cmd, " -d ");
    //             for (int j = 0; sub_shallow_dirs[j]; j++) {
    //                 strcat(maketop_cmd, sub_shallow_dirs[j]);
    //                 if (sub_shallow_dirs[j + 1]) strcat(maketop_cmd, ",");
    //             }
    //         }

    //         ret_code = build_target_incremental_core(&cfg,
    //                                                  maketop_cmd,
    //                                                  compiler,
    //                                                  sub_flags_str,
    //                                                  sub_obj_dir,
    //                                                  sub_mod_dir,
    //                                                  sub_target_name,
    //                                                  sub_exclude_files,
    //                                                  parallel_build,
    //                                                  incremental_build,
    //                                                  lib_only,
    //                                                  run_flag,
    //                                                  is_c);

    //         //Free the allocations for the arrays
    //         if (sub_deep_dirs) {
    //             for (int k = 0; sub_deep_dirs[k]; k++) free(sub_deep_dirs[k]);
    //             free(sub_deep_dirs);
    //         }
    //         if (sub_shallow_dirs) {
    //             for (int k = 0; sub_shallow_dirs[k]; k++) free(sub_shallow_dirs[k]);
    //             free(sub_shallow_dirs);
    //         }
    //         if(sub_exclude_files){
    //             for (int k = 0; sub_exclude_files[k]; k++) free(sub_exclude_files[k]);
    //             free(sub_exclude_files);
    //         }

    //         if(ret_code < 0) return -1;
    //     }
    // }

    // Now build the top-level main target using the same logic:
    char maketop_cmd[1024] = {0};
    #ifdef _WIN32
        strcat(maketop_cmd, "bin\\maketopologicf90.exe");
    #else
        strcat(maketop_cmd, "./bin/maketopologicf90.exe");
    #endif
    if (deep_dirs) {
        strcat(maketop_cmd, " -D ");
        for (int i = 0; deep_dirs[i]; i++) {
            strcat(maketop_cmd, deep_dirs[i]);
            if (deep_dirs[i + 1]) strcat(maketop_cmd, ",");
        }
    }
    if (shallow_dirs) {
        strcat(maketop_cmd, " -d ");
        for (int i = 0; shallow_dirs[i]; i++) {
            strcat(maketop_cmd, shallow_dirs[i]);
            if (shallow_dirs[i + 1]) strcat(maketop_cmd, ",");
        }
    }

    char **exclude_files = fortuna_toml_get_array(&cfg, "exclude.files");
    ret_code = build_target_incremental_core(&cfg,
                                             maketop_cmd,
                                             compiler,
                                             flags_str,
                                             obj_dir,
                                             mod_dir,
                                             target,
                                             exclude_files,
                                             parallel_build,
                                             incremental_build,
                                             lib_only,
                                             run_flag,
                                             is_c);


    if (deep_dirs) {
        for (int i = 0; deep_dirs[i]; i++) free(deep_dirs[i]);
        free(deep_dirs);
    }
    if (shallow_dirs) {
        for (int i = 0; shallow_dirs[i]; i++) free(shallow_dirs[i]);
        free(shallow_dirs);
    }
    if (exclude_files) {
        for (int i = 0; exclude_files[i]; i++) free(exclude_files[i]);
        free(exclude_files);
    }

defer_build:
    if (flags_array) {
        for (int i = 0; flags_array[i]; i++) free(flags_array[i]);
        free(flags_array);
    }
    if (flags_str) free(flags_str);
    fortuna_toml_free(&cfg);

    //Check if we passed or failed the build
    return (ret_code < 0) ? -1 : 0;
}

