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

//This allows for nested src files in any number of directories
//to be parsed into just the filename and thus we can put them in the
//object directory. We only rebuild if the src changes, not the obj. 
char *get_last_path_segment(const char *path) {
    const char *end = path + strlen(path);
    const char *p = end;
    while (p > path && *(p - 1) != '/' && *(p - 1) != '\\') p--;
    return strdup(p); 
}

// Worker thread: runs system() on a single command string
void compile_system_worker(void *arg) {
    char *cmd = (char *)arg;
    int ret = system(cmd);
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

// Free a list of strings
static void free_string_list(char **list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
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
    int ret = system(ar_cmd);
    if (ret != 0) {
        print_error("Linking failed.");
        return -1;
    }
    return 0;
}

int fortuna_build_project_incremental(const int parallel_build, const int incremental_build_override, 
                                     const int lib_only, const int run_flag) {

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

    // Combine unique flags once into a single string
    char **unique_flags = NULL;
    int unique_count = 0;

    for (int i = 0; flags_array[i]; i++) {
        if (add_unique_flag(&unique_flags, &unique_count, flags_array[i]) != 0) {
            print_error("Memory error adding flags to list");
            goto cleanup_arrays;
        }
    }

    // Build single string of all flags (space separated)
    size_t flags_len = 0;
    for (int i = 0; i < unique_count; i++) {
        flags_len += strlen(unique_flags[i]) + 1; // +1 for space or null terminator
    }

    char *flags_str = malloc(flags_len + 1);
    if (!flags_str) {
        print_error("Memory allocation error for flags string.");
        goto cleanup_arrays;
    }
    flags_str[0] = '\0';

    for (int i = 0; i < unique_count; i++) {
        strcat(flags_str, unique_flags[i]);
        if (i < unique_count - 1) strcat(flags_str, " ");
    }

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
        goto cleanup_arrays;
    }
    if (mod_dir && !dir_exists(mod_dir)) {
        print_error("Module directory does not exist.");
        goto cleanup_arrays;
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

    //Build the command for the maketopologicf90 call. 
    //This will be a loop when we support multiple binaries. 
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
    //Still need the list of source files to link against.
    char *topo_src= run_command_capture(maketop_cmd);

    //Allocate the hashmaps.
    FileNode*  cur_map[HASH_TABLE_SIZE]  = {NULL};
    HashEntry* prev_map[HASH_TABLE_SIZE] = {NULL};
    
    //Need the list of everything for linking. 
    if (!topo_src) {
        print_error("Failed to get topologically sorted sources.");
        goto cleanup_search_arrays;
    }

    
    //Now we get the exclusion list (if it exists)
    FileNode*  exclusion_map[HASH_TABLE_SIZE]  = {NULL};
    char **exclude_files = fortuna_toml_get_array(&cfg, "exclude.files");
    if(exclude_files){
        for(int i = 0; exclude_files[i]; i++){
            insert_node(exclude_files[i],exclusion_map);
        }
    }


    //From the list of topologically sorted files, we need to parse them
    //properly as they are a single string. 
    char *line     = strtok(topo_src, "\n");
    char **sources = NULL;
    int src_count  = 0;
    char **tmp     = NULL;
    while (line) {
        tmp = realloc(sources, sizeof(char *)*(src_count + 1));
        if (!tmp) {
            print_error("Memory allocation error.");
            free(topo_src);
            goto cleanup_sources;
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


    //Define the rebuild count
    int rebuild_cnt = 0;

    // Compile each source in parallel if asked.
    thread_t *threads;

    //The most threads we can have is all of them so this is a safe allocation. 
    if(parallel_build) threads = (thread_t*)malloc(src_count*sizeof(thread_t));

    //Allocate the characetr buffers
    char compile_cmd[2048];
    char obj_file[1024];

    //For the incremental build, we parse the dependency files and rebuild. 
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
            return -1;
        }
            
        //If hash file exists, load it and compare
        if (file_exists(hash_cache_file)) {
            load_prev_hashes(hash_cache_file,prev_map);
            save_hashes(hash_cache_file,cur_map);
            prune_obsolete_cached_entries(prev_map,cur_map);
        }else{
            print_error("Cannot do an incremental build with no history!");
            print_error("Check that the .cache/hash.dep file exists.\n");
            return -1;
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
                node = node->next;
            }
        }

        //Rebuild required if the rebuild list is not empty.
        //Otherwise, we jump to our memory cleanup.
        if(rebuild_list == NULL && lib_only == 0) {
            if(!run_flag) print_info("Nothing to build");
            goto cleanup_sources;
        }

        //Compile each source only if it changed and needs to be rebuilt. 
        FileNode *curr = rebuild_list;
        while(curr){
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
                int ret = system(compile_cmd);
                if (ret != 0) {
                    print_error("Compilation failed.");
                    free(topo_make);
                    free(rebuild_list); 
                    goto cleanup_sources;
                }
            }else{
                //Copy the command to a per thread buffer to avoid conflicts. 
                char *compile_cmd_ts = strdup(compile_cmd);
                if (thread_create(&threads[rebuild_cnt++], compile_system_worker, compile_cmd_ts) != 0) {
                    print_error("Failed to create thread");
                    free(topo_make);
                    free(rebuild_list); 
                    goto cleanup_sources;
                }
            }

            //Next node
            curr = curr->next;
        }

        //Free the topo_make here and then free the rebuild list. 
        free(topo_make);
        free(rebuild_list);
    }else{
        for (int i = 0; i < src_count; i++) {
            const char *src      = sources[i];
            const char *rel_path = get_last_path_segment(src);
            if(truncate_file_name_at_file_extension(rel_path)) continue;
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
                int ret = system(compile_cmd);
                if (ret != 0) {
                    print_error("Compilation failed.");
                    goto cleanup_sources;
                }
            }else{
                //Copy the command to a per thread buffer to avoid conflicts. 
                char *compile_cmd_ts = strdup(compile_cmd);
                if (thread_create(&threads[i], compile_system_worker, compile_cmd_ts) != 0) {
                    print_error("Failed to create thread");
                    goto cleanup_sources;
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
    const char* lib = fortuna_toml_get_string(&cfg, "lib.target");
    if(lib != NULL && lib_only == 0) {
        if(build_library(sources,src_count,obj_dir,lib) == -1){
            print_error("Failed to link library. Check if ar is installed and if the paths are correct.");
            return -1;
        }
    }else if(lib != NULL && lib_only == 1){
        //If lib only, we skip linking the executable.
        goto skip_linking;
    }else if(lib == NULL && lib_only == 1){
        //If lib == NULL but we requested a lib only run, error out. 
        print_error("No target lib found in Fortuna.toml");
        return -1;
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
            return -1;
        }

        //Add to the command string.
        link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, " %s", obj_path);
    }

    //Link with the libraries (if they exist).
    char **source_libs = fortuna_toml_get_array(&cfg, "library.source-libs");
    if (!source_libs) source_libs = NULL;
    if (source_libs ) {
        for (int i = 0; source_libs[i]; i++) {
            link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, " %s", source_libs[i]);
        }
    }

    //Build the final link command
    link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, " -o %s", target);

    //Execute the link command
    print_info(link_cmd);
    int ret = system(link_cmd);
    if (ret != 0) {
        print_error("Linking failed.");
        goto cleanup_sources;
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


    //GOTO's for freeing the memory. Basically defer, but obviously C doesn't have a real defer. 
cleanup_sources:
    if (sources) {
        for (int i = 0; i < src_count; i++) free(sources[i]);
        free(sources);
    }
    free(topo_src);

cleanup_search_arrays:
    if (deep_dirs) {
        for (int i = 0; deep_dirs[i]; i++) free(deep_dirs[i]);
        free(deep_dirs);
    }
    if (shallow_dirs) {
        for (int i = 0; shallow_dirs[i]; i++) free(shallow_dirs[i]);
        free(shallow_dirs);
    }

cleanup_arrays:
    for (int i = 0; flags_array[i]; i++) free(flags_array[i]);
    free(flags_array);

    free_string_list(unique_flags, unique_count);
    fortuna_toml_free(&cfg);

    //Free the hashmaps.
    free_all(cur_map);
    free_prev_hash_table(prev_map);
    free_all(exclusion_map);

    return 0;
}





// Deprecated Code for single increment every time. Saved here because it's useful for debugging. 
// int fortuna_build_project(const char *project_dir) {
//     if (!project_dir) {
//         print_error("Project directory is NULL.");
//         return -1;
//     }

//     char build_dir[512];
//     snprintf(build_dir, sizeof(build_dir), "%s/build", project_dir);

//     if (!dir_exists(build_dir)) {
//         snprintf(build_dir, sizeof(build_dir), "build");
//         if (!dir_exists(build_dir)) {
//             print_error("Build directory does not exist.");
//             return -1;
//         }
//     }

    
//     char cache_path[512];
//     snprintf(cache_path, sizeof(cache_path), "%s/incremental_cache.txt",build_dir);
//     if(file_exists(cache_path)){
//         int res = fortuna_build_project_incremental(project_dir);
//         return res;
//     }

//     char toml_path[512];
//     snprintf(toml_path, sizeof(toml_path), "%s/project.toml", build_dir);

//     fortuna_toml_t cfg = {0};
//     if (fortuna_toml_load(toml_path, &cfg) != 0) {
//         print_error("Failed to load project.toml.");
//         return -1;
//     }

//     const char *target = fortuna_toml_get_string(&cfg, "build.target");
//     if (!target) {
//         print_error("Missing 'build.target' in config.");
//         fortuna_toml_free(&cfg);
//         return -1;
//     }

//     char *compiler = (char *)fortuna_toml_get_string(&cfg, "build.compiler");
//     if (!compiler) compiler = "gfortran";

//     char **flags_array = fortuna_toml_get_array(&cfg, "build.flags");
//     if (!flags_array) {
//         print_error("Missing or empty 'build.flags' in config.");
//         fortuna_toml_free(&cfg);
//         return -1;
//     }

//     // Combine unique flags once into a single string
//     char **unique_flags = NULL;
//     int unique_count = 0;

//     for (int i = 0; flags_array[i]; i++) {
//         if (add_unique_flag(&unique_flags, &unique_count, flags_array[i]) != 0) {
//             print_error("Memory error adding flag.");
//             goto cleanup_arrays;
//         }
//     }

//     // Build single string of all flags (space separated)
//     size_t flags_len = 0;
//     for (int i = 0; i < unique_count; i++) {
//         flags_len += strlen(unique_flags[i]) + 1; // +1 for space or null terminator
//     }

//     char *flags_str = malloc(flags_len + 1);
//     if (!flags_str) {
//         print_error("Memory allocation error for flags string.");
//         goto cleanup_arrays;
//     }
//     flags_str[0] = '\0';

//     for (int i = 0; i < unique_count; i++) {
//         strcat(flags_str, unique_flags[i]);
//         if (i < unique_count - 1) strcat(flags_str, " ");
//     }

//     char old_dir[PATH_MAX];
//     getcwd(old_dir, sizeof(old_dir));
//     chdir(project_dir);

//     const char *obj_dir = fortuna_toml_get_string(&cfg, "build.obj_dir");
//     const char *mod_dir = fortuna_toml_get_string(&cfg, "build.mod_dir");

//     if (!obj_dir || !mod_dir) {
//         print_error("Missing directory settings in config.");
//         goto cleanup_flags_str;
//     }

//     if (!dir_exists(obj_dir)) {
//         print_error("Object directory does not exist.");
//         goto cleanup_flags_str;
//     }
//     if (!dir_exists(mod_dir)) {
//         print_error("Module directory does not exist.");
//         goto cleanup_flags_str;
//     }

//     char **deep_dirs = fortuna_toml_get_array(&cfg, "search.deep");
//     char **shallow_dirs = fortuna_toml_get_array(&cfg, "search.shallow");

//     if (!deep_dirs) deep_dirs = NULL;
//     if (!shallow_dirs) shallow_dirs = NULL;

//     char maketop_cmd[1024] = {0};

// #ifdef _WIN32
//     strcat(maketop_cmd, "build\\maketopologicf90.exe");
// #else
//     strcat(maketop_cmd, "./build/maketopologicf90.exe");
// #endif
//     if (deep_dirs) {
//         strcat(maketop_cmd, " -D ");
//         for (int i = 0; deep_dirs[i]; i++) {
//             strcat(maketop_cmd, deep_dirs[i]);
//             if (deep_dirs[i + 1]) strcat(maketop_cmd, ",");
//         }
//     }
//     if (shallow_dirs) {
//         strcat(maketop_cmd, " -d ");
//         for (int i = 0; shallow_dirs[i]; i++) {
//             strcat(maketop_cmd, shallow_dirs[i]);
//             if (shallow_dirs[i + 1]) strcat(maketop_cmd, ",");
//         }
//     }

//     //Get the file list. 
//     char *topo_src= run_command_capture(maketop_cmd);
//     if (!topo_src) {
//         print_error("Failed to get topologically sorted sources.");
//         goto cleanup_search_arrays;
//     }

//     char *line = strtok(topo_src, "\n");
//     char **sources = NULL;
//     int src_count = 0;

//     while (line) {
//         char **tmp = realloc(sources, sizeof(char *) * (src_count + 1));
//         if (!tmp) {
//             print_error("Memory allocation error.");
//             free(topo_src);
//             goto cleanup_sources;
//         }
//         sources = tmp;
//         sources[src_count] = strdup(line);
//         src_count++;
//         line = strtok(NULL, "\n");
//     }

//     // Compile each source
//     for (int i = 0; i < src_count; i++) {
//         const char *src = sources[i];
//         const char *rel_path = src + strlen("src") + 1;

//         char rel_path_no_ext[512];
//         strncpy(rel_path_no_ext, rel_path, sizeof(rel_path_no_ext));
//         rel_path_no_ext[sizeof(rel_path_no_ext) - 1] = '\0';

//         char *ext = strrchr(rel_path_no_ext, '.');
//         if (ext && (strcmp(ext, ".f90") == 0 || strcmp(ext, ".for") == 0)) {
//             *ext = '\0';
//         }

//         char obj_file[1024];
//         snprintf(obj_file, sizeof(obj_file), "%s/%s.o", obj_dir, rel_path_no_ext);

//         char compile_cmd[2048];

//         snprintf(compile_cmd, sizeof(compile_cmd), "%s %s -J%s -c %s -o %s",
//             compiler, flags_str, mod_dir, src, obj_file);

//         print_info(compile_cmd);
//         int ret = system(compile_cmd);
//         if (ret != 0) {
//             print_error("Compilation failed.");
//             goto cleanup_sources;
//         }
//     }

//     // Link
//     char link_cmd[4096] = {0};
//     size_t link_pos = 0;

//     link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, "%s %s", compiler, flags_str);

//     for (int i = 0; i < src_count; i++) {
//         const char *src = sources[i];
//         const char *rel_path = src + strlen("src") + 1;

//         char rel_path_no_ext[512];
//         strncpy(rel_path_no_ext, rel_path, sizeof(rel_path_no_ext));
//         rel_path_no_ext[sizeof(rel_path_no_ext) - 1] = '\0';

//         char *ext = strrchr(rel_path_no_ext, '.');
//         if (ext && (strcmp(ext, ".f90") == 0 || strcmp(ext, ".for") == 0)) {
//             *ext = '\0';
//         }

//         char obj_path[512];
//         snprintf(obj_path, sizeof(obj_path), "%s/%s.o", obj_dir, rel_path_no_ext);

//         link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, " %s", obj_path);
//     }

//     link_pos += snprintf(link_cmd + link_pos, sizeof(link_cmd) - link_pos, " -o %s", target);

//     print_info(link_cmd);
//     int ret = system(link_cmd);
//     if (ret != 0) {
//         print_error("Linking failed.");
//         goto cleanup_sources;
//     }

//     print_ok("Built Successfully");

//     //For the incremental build, we parse the files!
//     strcat(maketop_cmd," -m");
//     char *topo_make = run_command_capture(maketop_cmd);

//     //Always rebuild the hash table when we are done. 
//     const char* deps_file = "build/topo.txt";
//     FILE* depedency_chain = fopen(deps_file ,"w+");
//     fprintf(depedency_chain,"%s",topo_make);
//     fclose(depedency_chain);

//     //Allocate the hashmaps.
//     FileNode*  cur_map[HASH_TABLE_SIZE]  = {NULL};

//     //Parse the dependency file first (we always need it)
//     parse_dependency_file(deps_file,cur_map);

//     //If hash file exists, load it and compare
//     const char* hash_cache_file = "build/incremental_cache.txt";

//     //Save updated hash list for future runs
//     save_hashes(hash_cache_file,cur_map);

// cleanup_sources:
//     if (sources) {
//         for (int i = 0; i < src_count; i++) free(sources[i]);
//         free(sources);
//     }
//     free(topo_src);

// cleanup_search_arrays:
//     if (deep_dirs) {
//         for (int i = 0; deep_dirs[i]; i++) free(deep_dirs[i]);
//         free(deep_dirs);
//     }
//     if (shallow_dirs) {
//         for (int i = 0; shallow_dirs[i]; i++) free(shallow_dirs[i]);
//         free(shallow_dirs);
//     }

// cleanup_flags_str:
//     free(flags_str);

// cleanup_arrays:
//     for (int i = 0; flags_array[i]; i++) free(flags_array[i]);
//     free(flags_array);

//     free_string_list(unique_flags, unique_count);

//     fortuna_toml_free(&cfg);
//     chdir(old_dir);

//     free_all(cur_map);
    
//     return 0;
// }


