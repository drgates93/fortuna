#ifndef FORTUNA_HELPER_FN
#define FORTUNA_HELPER_FN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLOR_GREEN  "\033[0;32m"
#define COLOR_RED    "\033[0;31m"
#define COLOR_YELLOW "\033[0;33m"
#define COLOR_BLUE   "\033[0;34m"
#define COLOR_RESET  "\033[0m"

void print_ok(const char *msg);
void print_info(const char *msg);
void print_error(const char *msg);
void print_test(const char *msg);

#endif
