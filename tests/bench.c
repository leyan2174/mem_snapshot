#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static size_t iterations, live;
static pthread_barrier_t start;
static void *run(void *unused)
{
    (void)unused;
    void **p = calloc(live, sizeof(void *));
    if (!p) abort();
    pthread_barrier_wait(&start);
    for (size_t i = 0; i < iterations; ++i) {
        size_t j = i % live;
        free(p[j]);
        p[j] = malloc(64 + (i & 255));
        if (!p[j]) abort();
        ((volatile unsigned char *)p[j])[0] = (unsigned char)i;
    }
    for (size_t j = 0; j < live; ++j) free(p[j]);
    free(p);
    return NULL;
}
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    unsigned threads = (unsigned)strtoul(argv[1], NULL, 10);
    iterations = strtoull(argv[2], NULL, 10);
    live = strtoull(argv[3], NULL, 10);
    if (!threads || threads > 64 || !iterations || !live) return 2;
    pthread_t ids[64];
    pthread_barrier_init(&start, NULL, threads + 1);
    for (unsigned i = 0; i < threads; ++i)
        if (pthread_create(&ids[i], NULL, run, NULL)) abort();
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    pthread_barrier_wait(&start);
    for (unsigned i = 0; i < threads; ++i) pthread_join(ids[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double seconds = b.tv_sec - a.tv_sec + (b.tv_nsec - a.tv_nsec) / 1e9;
    printf("%.9f\n", seconds);
    return 0;
}
