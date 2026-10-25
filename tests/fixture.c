#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void *slots[16];
static size_t sizes[16];
static void **bulk;
static size_t bulk_count;
static _Atomic int stop;
static pthread_t threads[4];
static int running;
static void ignored(int sig) { (void)sig; }
static void preinit(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv;
    for (; *envp; ++envp)
        if (!strncmp(*envp, "TEST_SIGNAL_CONFLICT=", 21)) signal(SIGUSR2, ignored);
}
__attribute__((section(".preinit_array"), used))
static void (*preinit_entry)(int, char **, char **) = preinit;

__attribute__((noinline)) void *allocation_site(size_t size)
{
    void *p = malloc(size);
    if (p) memset(p, 0x5a, size);
    return p;
}
static void *release_other_thread(void *p) { free(p); return NULL; }
static void *stress(void *unused)
{
    (void)unused;
    while (!atomic_load(&stop)) {
        void *p[64];
        for (unsigned i = 0; i < 64; ++i) {
            p[i] = allocation_site(64 + i);
            if (!p[i]) abort();
        }
        for (unsigned i = 0; i < 64; ++i) {
            void *q = realloc(p[i], 192 + i);
            if (!q) abort();
            p[i] = q;
        }
        for (unsigned i = 0; i < 64; ++i) free(p[63 - i]);
    }
    return NULL;
}
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);
    printf("READY %ld\n", (long)getpid());
    char line[256], op[32];
    unsigned slot;
    size_t n, arg;
    while (fgets(line, sizeof(line), stdin)) {
        slot = 0; n = arg = 0;
        if (sscanf(line, "%31s %u %zu %zu", op, &slot, &n, &arg) < 1) continue;
        if (slot >= 16) abort();
        if (!strcmp(op, "malloc")) {
            slots[slot] = allocation_site(n); sizes[slot] = n;
        } else if (!strcmp(op, "calloc")) {
            errno = 0;
            slots[slot] = calloc(n, arg); sizes[slot] = slots[slot] ? n * arg : 0;
            if (slots[slot]) for (size_t i = 0; i < sizes[slot]; ++i)
                if (((unsigned char *)slots[slot])[i]) abort();
        } else if (!strcmp(op, "realloc")) {
            errno = 0;
            void *p = realloc(slots[slot], n);
            int saved = errno;
            if (p) {
                size_t common = n < sizes[slot] ? n : sizes[slot];
                for (size_t i = 0; i < common; ++i)
                    if (((unsigned char *)p)[i] != 0x5a) abort();
                memset(p, 0x5a, n);
                slots[slot] = p; sizes[slot] = n;
            } else if (!n) { slots[slot] = NULL; sizes[slot] = 0; }
            printf("REALLOC result=%p errno=%d\n", p, saved);
        } else if (!strcmp(op, "free")) {
            errno = EDOM; free(slots[slot]);
            if (errno != EDOM) abort();
            slots[slot] = NULL; sizes[slot] = 0;
        } else if (!strcmp(op, "crossfree")) {
            pthread_t t;
            if (pthread_create(&t, NULL, release_other_thread, slots[slot])) abort();
            pthread_join(t, NULL); slots[slot] = NULL; sizes[slot] = 0;
        } else if (!strcmp(op, "aligned")) {
            errno = EDOM;
            int rc = posix_memalign(&slots[slot], arg, n);
            if (errno != EDOM) abort();
            printf("ALIGNED rc=%d\n", rc);
            if (!rc) {
                if ((uintptr_t)slots[slot] % arg) abort();
                sizes[slot] = n;
            }
        } else if (!strcmp(op, "alignalloc")) {
            slots[slot] = aligned_alloc(arg, n); sizes[slot] = n;
            if (slots[slot] && (uintptr_t)slots[slot] % arg) abort();
        } else if (!strcmp(op, "memalign")) {
            slots[slot] = memalign(arg, n); sizes[slot] = n;
            if (slots[slot] && (uintptr_t)slots[slot] % arg) abort();
        } else if (!strcmp(op, "valloc") || !strcmp(op, "pvalloc")) {
            slots[slot] = !strcmp(op, "valloc") ? valloc(n) : pvalloc(n);
            sizes[slot] = n;
            if (slots[slot] && (uintptr_t)slots[slot] % (size_t)sysconf(_SC_PAGESIZE)) abort();
        } else if (!strcmp(op, "usable")) {
            if (malloc_usable_size(slots[slot]) < sizes[slot]) abort();
        } else if (!strcmp(op, "sizedfree")) {
            void (*fn)(void *, size_t) = dlsym(RTLD_DEFAULT, "free_sized");
            if (!fn) abort();
            fn(slots[slot], sizes[slot]); slots[slot] = NULL;
        } else if (!strcmp(op, "stress")) {
            atomic_store(&stop, 0);
            for (unsigned i = 0; i < 4; ++i)
                if (pthread_create(&threads[i], NULL, stress, NULL)) abort();
            running = 1;
        } else if (!strcmp(op, "stop")) {
            atomic_store(&stop, 1);
            for (unsigned i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
            running = 0;
        } else if (!strcmp(op, "bulk")) {
            bulk_count = n;
            bulk = calloc(n, sizeof(void *));
            if (!bulk) abort();
            for (size_t i = 0; i < n; ++i) {
                bulk[i] = allocation_site(17);
                if (!bulk[i]) abort();
            }
        } else if (!strcmp(op, "bulkfree")) {
            for (size_t i = 0; i < bulk_count; ++i) free(bulk[i]);
            free(bulk); bulk = NULL; bulk_count = 0;
        } else if (!strcmp(op, "fork")) {
            pid_t child = fork();
            if (child < 0) abort();
            if (!child) {
                void *p = allocation_site(73); free(p);
                raise(SIGUSR2);
                _exit(0);
            }
            int status;
            waitpid(child, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status)) abort();
        } else if (!strcmp(op, "outputfail")) {
            signal(SIGXFSZ, SIG_IGN);
            struct rlimit limit = { .rlim_cur = 1, .rlim_max = 1 };
            if (setrlimit(RLIMIT_FSIZE, &limit)) abort();
        } else if (!strcmp(op, "quit")) {
            if (running) abort();
            return 0;
        } else abort();
        printf("OK ptr=%p size=%zu\n", slots[slot], sizes[slot]);
    }
    return 0;
}
