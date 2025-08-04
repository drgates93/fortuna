#include "fortuna_helper_fn.h"

void print_ok(const char *msg) {
    printf("%s[OK]%s     %s\n", COLOR_GREEN, COLOR_RESET, msg);
}

void print_info(const char *msg) {
    printf("%s[INFO]%s   %s\n", COLOR_YELLOW, COLOR_RESET, msg);
}

void print_error(const char *msg) {
    fprintf(stderr, "%s[ERROR]%s  %s\n", COLOR_RED, COLOR_RESET, msg);
}

void print_test(const char *msg) {
    printf("%s[TEST]%s  %s\n", COLOR_BLUE, COLOR_RESET, msg);
}

#if defined(_WIN32)
#include <windows.h>

// Windows version using CreateProcessA
int launch_process(const char *exe, const char *args) {
    // Combine exe + args into a single command line string
    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "%s %s", exe, args ? args : "");

    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);

    BOOL success = CreateProcessA(
        NULL,             // Application name (NULL = use cmdline)
        cmdline,          // Command line
        NULL, NULL,       // Security attributes
        FALSE,            // Inherit handles
        0,                // Creation flags
        NULL,             // Environment (inherit)
        NULL,             // Current directory (inherit)
        &si, &pi
    );

    if (!success) {
        char msg[512];
        snprintf(msg,sizeof(msg),"CreateProcess failed (error %lu)\n", GetLastError());
        print_error(msg);
        return -1;
    }

    // Wait for process to finish
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

#else
#include <unistd.h>
#include <sys/wait.h>

// POSIX version using fork + execve
int launch_process(const char *exe, const char *args) {
    // Tokenize the args string into argv[] array
    char *argv[64];
    int argc = 0;

    argv[argc++] = strdup(exe); // argv[0] is the program name

    if (args && *args) {
        char *argstr = strdup(args);
        char *token = strtok(argstr, " ");
        while (token && argc < 63) {
            argv[argc++] = token;
            token = strtok(NULL, " ");
        }
        argv[argc] = NULL;
    } else {
        argv[argc] = NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        print_error("fork failed");
        return -1;
    } else if (pid == 0) {
        execve(exe, argv, environ);
        print_error("execve failed");
    } else {
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
}
#endif