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