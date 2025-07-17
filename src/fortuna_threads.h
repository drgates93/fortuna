// cross_thread_single.c
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE thread_t;
#else
#include <pthread.h>
typedef pthread_t thread_t;
#endif

typedef void (*thread_func_t)(void *);

// Cross-platform thread start wrapper declaration
static int thread_create(thread_t *thread, thread_func_t func, void *arg);
static int thread_join(thread_t thread);

#ifdef _WIN32

typedef struct {
    thread_func_t func;
    void *arg;
} thread_start_t;

static DWORD WINAPI thread_start(LPVOID param) {
    thread_start_t *start = (thread_start_t *)param;
    start->func(start->arg);
    free(start);
    return 0;
}

static int thread_create(thread_t *thread, thread_func_t func, void *arg) {
    thread_start_t *start = (thread_start_t*)malloc(sizeof(thread_start_t));
    if (!start) {
        fprintf(stderr, "Failed to allocate memory for thread start data\n");
        return -1;
    }

    start->func = func;
    start->arg = arg;

    *thread = CreateThread(NULL, 0, thread_start, start, 0, NULL);
    if (*thread == NULL) {
        fprintf(stderr, "Failed to create thread\n");
        free(start);
        return -1;
    }
    return 0;
}

static int thread_join(thread_t thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}

#else // POSIX pthreads

static void *thread_start(void *arg) {
    void **data = (void **)arg;
    thread_func_t func = (thread_func_t)data[0];
    void *func_arg = data[1];
    free(data);
    func(func_arg);
    return NULL;
}

static int thread_create(thread_t *thread, thread_func_t func, void *arg) {
    void **data = malloc(2 * sizeof(void *));
    if (!data) {
        fprintf(stderr, "Failed to allocate memory for thread start data\n");
        return -1;
    }

    data[0] = (void *)func;
    data[1] = arg;

    int res = pthread_create(thread, NULL, thread_start, data);
    if (res != 0) {
        fprintf(stderr, "Failed to create thread (pthread_create returned %d)\n", res);
        free(data);
        return res;
    }
    return 0;
}

static int thread_join(thread_t thread) {
    return pthread_join(thread, NULL);
}

#endif
