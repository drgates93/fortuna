#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

//Fortuna files
#include "fortuna_levenshtein.h"
#include "fortuna_build.h"
#include "fortuna_cli_args.h"
#include "fortuna_helper_fn.h"
#include "fortuna_toml.h"

#ifdef _WIN32
    #define MKDIR(path) _mkdir(path)
    #define PATH_SEP '\\'
    #include <direct.h>
    #include <windows.h>
#else
    #define MKDIR(path) mkdir(path, 0755)
    #define PATH_SEP '/'
    #include <unistd.h>
    #include <libgen.h>
#endif

#define FORTUNA_NAME "Fortuna.toml"

const char *get_executable_dir(void) {
    static char buffer[4096];
    #if defined(_WIN32)
        DWORD len = GetModuleFileNameA(NULL, buffer, sizeof(buffer));
        if (len == 0 || len == sizeof(buffer)) return NULL;
        for (int i = len - 1; i >= 0; --i) {
            if (buffer[i] == '\\' || buffer[i] == '/') {
                buffer[i] = '\0';
                break;
            }
        }
        return buffer;
    #elif defined(__linux__)
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len == -1 || len >= sizeof(buffer)) return NULL;
        buffer[len] = '\0';
        return dirname(buffer);

    #else
        return NULL;
    #endif
}

// Directory list
const char *dirs[] = { "src", "mod", "obj", "data", "lib", "bin"};
const int num_dirs = sizeof(dirs) / sizeof(dirs[0]);

const char *hidden_dirs[] = {".cache"};
const int num_hidden_dirs = 1;

// Create directory helper
int create_dir(const char *path) {
    int result = MKDIR(path);
    if (result == 0) return 0;
    if (errno == EEXIST) return 0; // already exists is fine
    return -1;
}

int create_hidden_dir(const char* dir_name) {
#ifdef _WIN32
    // Create the directory
    if (!CreateDirectoryA(dir_name, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            print_error("Failed to create directory. Error code: %lu\n");
            return -1;
        }
    }

    // Set the directory as hidden
    if (!SetFileAttributesA(dir_name, FILE_ATTRIBUTE_HIDDEN)) {
        print_error("Failed to set hidden attribute.\n");
        return -1;
    }
#else
    // Just create the directory (it's hidden due to the dot prefix)
    if (MKDIR(dir_name) != 0 && errno != EEXIST) {
        print_error("Failed to create directory");
        return -1;
    }
#endif
    return 0;
}

// Create directories inside a base path
void create_directories(const char *base_path) {
    for (int i = 0; i < num_dirs; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s%c%s", base_path, PATH_SEP, dirs[i]);
        int result = create_dir(path);
        if (result == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Created directory: %s", path);
            print_ok(msg);
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to create directory: %s (errno %d)", path, errno);
            print_error(msg);
        }
    }

    for(int i = 0; i < num_hidden_dirs; i++){
        char path[512];
        snprintf(path, sizeof(path), "%s%c%s", base_path, PATH_SEP, hidden_dirs[i]);
        int result = create_hidden_dir(path);
        if (result != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to create directory: %s (errno %d)", path, errno);
            print_error(msg);
        }
    }
}

int remove_folder_recursive(const char *path) {
#ifdef _WIN32
    WIN32_FIND_DATA ffd;
    HANDLE hFind = INVALID_HANDLE_VALUE;
    char search_path[MAX_PATH];
    snprintf(search_path, MAX_PATH, "%s\\*", path);

    hFind = FindFirstFile(search_path, &ffd);
    if (INVALID_HANDLE_VALUE == hFind) {
        fprintf(stderr, "FindFirstFile failed (%lu)\n", GetLastError());
        return -1;
    }

    do {
        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
            continue;

        char full_path[MAX_PATH];
        snprintf(full_path, MAX_PATH, "%s\\%s", path, ffd.cFileName);

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (remove_folder_recursive(full_path) != 0) {
                FindClose(hFind);
                return -1;
            }
        } else {
            if (!DeleteFile(full_path)) {
                fprintf(stderr, "Failed to delete file %s (%lu)\n", full_path, GetLastError());
                FindClose(hFind);
                return -1;
            }
        }
    } while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);

    if (!RemoveDirectory(path)) {
        fprintf(stderr, "Failed to remove directory %s (%lu)\n", path, GetLastError());
        return -1;
    }
    return 0;

#else
    DIR *d = opendir(path);
    if (!d) {
        perror("opendir failed");
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        char full_path[PATH_MAX];
        snprintf(full_path, PATH_MAX, "%s/%s", path, entry->d_name);

        struct stat statbuf;
        if (lstat(full_path, &statbuf) != 0) {
            perror("stat failed");
            closedir(d);
            return -1;
        }

        if (S_ISDIR(statbuf.st_mode)) {
            if (remove_folder_recursive(full_path) != 0) {
                closedir(d);
                return -1;
            }
        } else {
            if (unlink(full_path) != 0) {
                perror("unlink failed");
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    if (rmdir(path) != 0) {
        perror("rmdir failed");
        return -1;
    }
    return 0;
#endif
}


// Modified copy_file to support destination path with base dir
int copy_file(const char *src, const char *dest) {
    FILE *f_src = fopen(src, "rb");
    if (!f_src) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot open source file: %s", src);
        print_error(msg);
        return -1;
    }

    FILE *f_dest = fopen(dest, "wb");
    if (!f_dest) {
        fclose(f_src);
        char msg[256];
        snprintf(msg, sizeof(msg), "Cannot open destination file: %s", dest);
        print_error(msg);
        return -1;
    }

    char buffer[4096];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), f_src)) > 0) {
        fwrite(buffer, 1, bytes, f_dest);
    }

    fclose(f_src);
    fclose(f_dest);
    return 0;
}

void copy_template_files(const char *base_path) {
    //char dest_makefile[512];
    char dest_exe[512];
    //snprintf(dest_makefile, sizeof(dest_makefile), "%s%cmakefile", base_path, PATH_SEP);
    snprintf(dest_exe, sizeof(dest_exe), "%s%cbin%cmaketopologicf90.exe", base_path, PATH_SEP, PATH_SEP);


    //Need the absolute install path
    const char *install_dir = get_executable_dir();
    //char src_makefile[1024];
    char src_exe[1024];
    //snprintf(src_makefile, sizeof(src_makefile), "%s%c%s%c%s", install_dir, PATH_SEP, "data",PATH_SEP,"template.mak");
    snprintf(src_exe,sizeof(src_exe), "%s%c%s%c%s", install_dir, PATH_SEP, "bin",PATH_SEP,"maketopologicf90.exe");

    //Now we can copy the files.
    //copy_file(src_makefile, dest_makefile);
    copy_file(src_exe, dest_exe);
}

int directory_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

int file_exists_generic(char *filename) {
  struct stat buffer;   
  return (stat(filename, &buffer) == 0);
}

int generate_project_toml(const char *project_name) {

    // Path to the project.toml file
    char toml_path[512];
    snprintf(toml_path, sizeof(toml_path), "%s%c%s", project_name,PATH_SEP,FORTUNA_NAME);

    FILE *f = fopen(toml_path, "w");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to create TOML file: %s (%s)", toml_path, strerror(errno));
        print_error(msg);
        return -1;
    }

    fprintf(f,
        "[build]\n"
        "target = \"%s\"\n"
        "compiler = \"gfortran\"\n\n"
        "flags = [\n"
        "  \"-cpp\", \"-fno-align-commons\", \"-O3\",\n"
        "  \"-ffpe-trap=zero,invalid,underflow,overflow\",\n"
        "  \"-std=legacy\", \"-ffixed-line-length-none\", \"-fall-intrinsics\",\n"
        "  \"-Wno-unused-variable\", \"-Wno-unused-function\",\n"
        "  \"-Wno-conversion\", \"-fopenmp\", \"-Imod\"\n"
        "]\n\n"
        "obj_dir = \"obj\"\n"
        "mod_dir = \"mod\"\n\n"
        "[search]\n"
        "deep = [\"src\"]\n"
        "#shallow = [\"lib\", \"include\"]\n\n"
        "[library]\n"
        "#source-libs = [\"lib/test.lib\"]\n\n"
        "[exclude]\n"
        "#Requires the relative path from the Fortuna.toml file.\n"
        "#files = [\"src/some_file.f90\"] \n\n"
        "[lib]\n"
        "#Placed in the lib folder and only supports static linking with ar\n"
        "#target = \"%s.lib\"\n\n"
        "[args]\n"
        "#cmds = [\"cmd_line_argument\"] \n\n",
        project_name,project_name
    );

    fclose(f);
    print_ok("Generated Fortuna.toml file successfully.");
    return 0;
}

// int replace_program_name_in_makefile(const char *new_program_name) {
//     int n = strlen(new_program_name);
//     char* makefile_path = (char*)calloc((n+1024),sizeof(char));
//     strcpy(makefile_path,new_program_name);
//     strcat(makefile_path,"/makefile");
//     FILE *f = fopen(makefile_path, "r");
//     if (!f) {
//         char msg[256];
//         snprintf(msg, sizeof(msg), "Error opening file %s: %s", makefile_path, strerror(errno));
//         print_error(msg);
//         return -1;
//     }

//     // Read all lines into memory (assuming small file)
//     char lines[1024][512];
//     int line_count = 0;
//     while (fgets(lines[line_count], sizeof(lines[0]), f)) {
//         line_count++;
//         if (line_count >= 1024) break;
//     }
//     fclose(f);

//     // Find PROGRAM line and replace
//     int replaced = 0;
//     for (int i = 0; i < line_count; i++) {
//         if (strncmp(lines[i], "PROGRAM =", 8) == 0) {
//             snprintf(lines[i], sizeof(lines[0]), "PROGRAM = %s\n", new_program_name);
//             replaced = 1;
//             break;
//         }
//     }

//     if (!replaced) {
//         print_error("PROGRAM variable not found in Makefile.");
//         return -2;
//     }

//     // Write back the modified content
//     f = fopen(makefile_path, "w");
//     if (!f) {
//         char msg[256];
//         snprintf(msg, sizeof(msg), "Error opening file %s for writing: %s", makefile_path, strerror(errno));
//         print_error(msg);
//         return -1;
//     }

//     for (int i = 0; i < line_count; i++) {
//         fputs(lines[i], f);
//     }

//     fclose(f);
//     return 0;
// }

int create_main_f90(const char *project_dir) {
    char src_dir[512];
    snprintf(src_dir, sizeof(src_dir), "%s/src", project_dir);

    // Make sure the src directory exists
    struct stat st = {0};
    if (stat(src_dir, &st) == -1) {
        if (mkdir(src_dir) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to create src directory: %s", strerror(errno));
            print_error(msg);
            return -1;
        }
    }

    // Path to main.f90
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/main.f90", src_dir);

    FILE *f = fopen(filepath, "w");
    if (!f) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to create %s: %s", filepath, strerror(errno));
        print_error(msg);
        return -1;
    }

    fprintf(f,
        "program main\n"
        "    print*, \"Hello World\"\n"
        "end program main\n"
    );
    fclose(f);
    return 0;
}

// int run_make_in_build(const char *project_name) {
//     char build_dir[512];
//     snprintf(build_dir, sizeof(build_dir), "%s", project_name);

//     // Save the current (top-level) working directory
//     char original_dir[512];
//     if (!getcwd(original_dir, sizeof(original_dir))) {
//         print_error("Failed to get current working directory");
//         return -1;
//     }

//     // Change to build directory
//     if (chdir(build_dir) != 0) {

//         //Check if we are in the directory
//         if(!strcmp(original_dir,build_dir)){
//             char msg[256];
//             snprintf(msg, sizeof(msg), "Failed to change directory to %s: %s", build_dir, strerror(errno));
//             print_error(msg);
//             return -1;
//         }

//     }

//     int result = system("make");
//     if (result != 0) {
//         print_error("Build failed");
//         return -1;
//     }

//     print_ok("Build completed successfully");
//     return 0;
// }

int main(int argc, char *argv[]) {

    //parse the cli arguments into the table.
    cli_args_t args;
    cli_args_init(&args);

    if (cli_args_parse(&args, argc, argv) != 0) {
        print_error("Failed to cli parse arguments\n");
        return 0;
    }

    if(argc < 2){
        print_error("Not enough cli arguments detected\n");
        return 0;
    }

    //Parallel build flag.
    int parallel_build = 0;

    //Incremental build flag
    int incremental_build = 1;

    //Build only a library flag
    int lib_only = 0;

    //Check if this is a run or not
    int run_flag = 0;

    //Project dir
    const char *project_dir;

    //New Command
    if (hashmap_contains_key_and_index(&args.args_map, "new", 1)) {

        int new_index = return_index_for_key(&args.args_map, "new");
        project_dir   = return_key_for_index(&args.args_map, new_index+1);
        if(project_dir == NULL){
            print_error("No valid project directory chosen with the new flag.");
            print_error("Syntax is \"fortuna new project\"");
            return 1;
        }
        printf("Initializing new project in '%s'...\n", project_dir);

         // Create the main project directory first
        if (create_dir(project_dir) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to create project directory: %s", project_dir);
            print_error(msg);
            return 1;
        } else {
            print_ok("Created project root directory");
        }

        // Create subdirectories inside project_dir
        create_directories(project_dir);

        // Copy template files into project_dir/build
        copy_template_files(project_dir);

        //Replace the name
        //replace_program_name_in_makefile(project_dir);
        
        //Build a program
        create_main_f90(project_dir);

        //Make the generic toml file
        generate_project_toml(project_dir);

        //Safely exit
        return 0;
    }

    //Build command
    if (hashmap_contains_key_and_index(&args.args_map, "build", 1)) {

        //Check if we are doing a parallel build.
        if(hashmap_contains(&args.args_map, "-j")) parallel_build = 1;

        //Check if we are allowing an incremental build.
        if(hashmap_contains(&args.args_map, "-r") || hashmap_contains(&args.args_map, "--rebuild") ){
            incremental_build = 0;
        }

        //Check if we are building a lib only
        if(hashmap_contains(&args.args_map, "--lib")) lib_only = 1;


        //Run the build
        fortuna_build_project_incremental(parallel_build,incremental_build,lib_only,run_flag);

        //Safely exit
        return 0;
    }

    //Run command
    if (hashmap_contains_key_and_index(&args.args_map, "run", 1)) {

        //Set the run flag
        run_flag = 1;

        //Load the toml file.
        const char* toml_path = FORTUNA_NAME;
        fortuna_toml_t cfg = {0};
        if (fortuna_toml_load(toml_path, &cfg) != 0) {
            print_error("Failed to load project.toml.");
            return -1;
        }

        const char *target = fortuna_toml_get_string(&cfg, "build.target");
        if (!target) {
            print_error("Missing 'build.target' in config.");
            fortuna_toml_free(&cfg);
            return -1;
        }

        //Check if we are doing a parallel build.
        if(hashmap_contains(&args.args_map, "-j")) parallel_build = 1;

        //Check if we are allowing an incremental build or forcing a full rebuild.
        if(hashmap_contains(&args.args_map, "-r") || hashmap_contains(&args.args_map, "--rebuild") ){
            incremental_build = 0;
        }

        if(!hashmap_contains(&args.args_map, "--bin")){

            //Then we may need a rebuild so we have to check. 
            if(fortuna_build_project_incremental(parallel_build,incremental_build,lib_only,run_flag) < 0){
                //print_error("Build Error");
                return -1;
            }
        }else{

            //Target a specific binary name in the top level directory of the project. 
            //Might be the bin folder later on. 
            int bin_index         = return_index_for_key(&args.args_map, "--bin");
            const char* exe_name  = return_key_for_index(&args.args_map, bin_index+1);
            if(exe_name != NULL) target = exe_name;
        }

        char exe[512];
        #ifdef _WIN32
            snprintf(exe,sizeof(exe),"%s.exe",target);
        #else
            snprintf(exe,sizeof(exe),"%s",target);
        #endif


        //Check for arguments
        const char *cmd = fortuna_toml_get_string(&cfg, "args.cmd");

        //Check if file exists first
        if(file_exists_generic(exe)) {
            launch_process(exe,cmd);
        }else{

            //Rebuild the project from scratch.
            run_flag          = 0;
            incremental_build = 0;
            parallel_build    = 1;
            fortuna_build_project_incremental(parallel_build,incremental_build,lib_only,run_flag);

            //Then check if the executable exists. If it does not, then print an error message. 
            if(file_exists_generic(exe)){
                launch_process(exe,cmd);
            }else{
                char msg[256];
                snprintf(msg,sizeof(msg),"Executable named %s not found",exe);
                print_error(msg);
                return -1;
            }
        }
    }


    //Clean 
    if (hashmap_contains_key_and_index(&args.args_map, "clean", 1)){
       
        //Load the toml file.
        const char* toml_path = FORTUNA_NAME;
        fortuna_toml_t cfg = {0};
        if (fortuna_toml_load(toml_path, &cfg) != 0) {
            print_error("Failed to load project.toml.");
            return -1;
        }

        //Remove the directories.
        const char *obj_dir = fortuna_toml_get_string(&cfg, "build.obj_dir");
        if (obj_dir) remove_folder_recursive(obj_dir);

        const char *mod_dir = fortuna_toml_get_string(&cfg, "build.mod_dir");
        if (mod_dir) remove_folder_recursive(mod_dir);

        //Make new directories
        if (obj_dir) create_dir(obj_dir);
        if (mod_dir) create_dir(mod_dir);

    }
    return 0;
}
