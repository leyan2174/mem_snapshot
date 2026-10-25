#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { SHARDS = 64, BUCKETS = 1024, BATCH = 256, FRAMES = 32,
       BOOT_SIZE = 1024 * 1024 };
typedef struct record {
    void *ptr;
    size_t size;
    uint64_t id;
    unsigned depth;
    void *frames[FRAMES];
} record;
typedef struct node { record rec; struct node *next; } node;
typedef struct shard {
    pthread_mutex_t lock;
    node *buckets[BUCKETS], *available;
    size_t mapped, count, bytes, dropped;
    uint64_t sequence;
} shard;
static shard table[SHARDS];
static size_t capacity = 4096;
static unsigned stack_depth = 16;
static _Atomic int enabled;
static _Thread_local unsigned nesting __attribute__((tls_model("initial-exec")));
static _Thread_local int resolving __attribute__((tls_model("initial-exec")));
static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static alignas(max_align_t) unsigned char bootstrap[BOOT_SIZE];
static _Atomic size_t boot_used;
static volatile sig_atomic_t notify_fd = -1;
static int read_fd = -1, output_fd = -1;
/* A private write buffer is not registered with stdio: fork children cannot
 * flush a copied snapshot into an unrelated descriptor during exit(). */
static char output_buffer[32768];
static size_t output_used;
static int output_failed;
static pid_t owner;
static void *(*next_malloc)(size_t);
static void (*next_free)(void *);
static void *(*next_calloc)(size_t, size_t);
static void *(*next_realloc)(void *, size_t);
static void *(*next_memalign)(size_t, size_t);
static void *(*next_aligned_alloc)(size_t, size_t);
static int (*next_posix_memalign)(void **, size_t, size_t);
static void *(*next_valloc)(size_t);
static void *(*next_pvalloc)(size_t);
static size_t (*next_usable_size)(void *);

static void message(const char *s)
{
    size_t left = strlen(s);
    while (left) {
        ssize_t n = write(STDERR_FILENO, s, left);
        if (n > 0) { s += n; left -= (size_t)n; }
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
}
static void flush_output(void)
{
    size_t written = 0;
    while (!output_failed && written < output_used) {
        ssize_t n = write(output_fd, output_buffer + written, output_used - written);
        if (n > 0) written += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else output_failed = 1;
    }
    output_used = 0;
}
__attribute__((format(printf, 1, 2))) static void emit(const char *format, ...)
{
    if (output_failed) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(output_buffer + output_used, sizeof(output_buffer) - output_used, format, args);
    va_end(args);
    if (n < 0) { output_failed = 1; return; }
    if ((size_t)n >= sizeof(output_buffer) - output_used) {
        flush_output();
        va_start(args, format);
        n = vsnprintf(output_buffer, sizeof(output_buffer), format, args);
        va_end(args);
        if (n < 0 || (size_t)n >= sizeof(output_buffer)) { output_failed = 1; return; }
    }
    output_used += (size_t)n;
}
static int boot_pointer(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    return v >= (uintptr_t)bootstrap && v < (uintptr_t)bootstrap + BOOT_SIZE;
}
static size_t boot_size(const void *p) { return ((const size_t *)p)[-1]; }
static void *boot_alloc(size_t alignment, size_t size)
{
    if (alignment < alignof(max_align_t)) alignment = alignof(max_align_t);
    if ((alignment & (alignment - 1)) || alignment > BOOT_SIZE || size > BOOT_SIZE) {
        errno = ENOMEM;
        return NULL;
    }
    size_t old = atomic_load_explicit(&boot_used, memory_order_relaxed);
    for (;;) {
        uintptr_t start = ((uintptr_t)bootstrap + old + sizeof(size_t) + alignment - 1)
                          & ~(uintptr_t)(alignment - 1);
        size_t end = start - (uintptr_t)bootstrap + (size ? size : 1);
        if (end > BOOT_SIZE) { errno = ENOMEM; return NULL; }
        if (atomic_compare_exchange_weak_explicit(&boot_used, &old, end,
                    memory_order_relaxed, memory_order_relaxed)) {
            ((size_t *)start)[-1] = size;
            return (void *)start;
        }
    }
}
static void resolve_symbols(void)
{
    resolving = 1;
#define RESOLVE(name) do { \
    *(void **)(&next_##name) = dlsym(RTLD_NEXT, #name); \
    if (!next_##name) { message("mem_snapshot: cannot resolve " #name "\n"); _exit(127); } \
} while (0)
    RESOLVE(malloc); RESOLVE(free); RESOLVE(calloc); RESOLVE(realloc);
    RESOLVE(memalign); RESOLVE(aligned_alloc); RESOLVE(posix_memalign);
    RESOLVE(valloc); RESOLVE(pvalloc);
    *(void **)(&next_usable_size) = dlsym(RTLD_NEXT, "malloc_usable_size");
    if (!next_usable_size) _exit(127);
#undef RESOLVE
    resolving = 0;
}
static void ensure_symbols(void) { pthread_once(&resolve_once, resolve_symbols); }

static uint64_t pointer_hash(void *ptr)
{
    uint64_t x = (uintptr_t)ptr;
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
static unsigned shard_index(void *p) { return pointer_hash(p) & (SHARDS - 1); }
static unsigned bucket_index(void *p) { return (pointer_hash(p) >> 6) & (BUCKETS - 1); }

/* All pool accesses happen under the owning shard's lock. */
static node *take_node(shard *s)
{
    if (!s->available) {
        if (s->mapped >= capacity) return NULL;
        node *block = mmap(NULL, sizeof(node) * BATCH, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (block == MAP_FAILED) return NULL;
        for (unsigned i = 0; i < BATCH; ++i) {
            block[i].next = s->available;
            s->available = &block[i];
        }
        s->mapped += BATCH;
    }
    node *n = s->available;
    s->available = n->next;
    return n;
}
static void link_node(shard *s, node *n)
{
    unsigned b = bucket_index(n->rec.ptr);
    n->next = s->buckets[b];
    s->buckets[b] = n;
    ++s->count; s->bytes += n->rec.size;
}
static node *detach(void *ptr, int recycle)
{
    if (!ptr) return NULL;
    shard *s = &table[shard_index(ptr)];
    pthread_mutex_lock(&s->lock);
    node **at = &s->buckets[bucket_index(ptr)];
    while (*at && (*at)->rec.ptr != ptr) at = &(*at)->next;
    node *n = *at;
    if (n) {
        *at = n->next; --s->count; s->bytes -= n->rec.size;
        if (recycle) { n->next = s->available; s->available = n; n = NULL; }
    }
    pthread_mutex_unlock(&s->lock);
    return n;
}
static void finish_detached(node *n, unsigned index, int restore)
{
    if (!n) return;
    shard *s = &table[index];
    pthread_mutex_lock(&s->lock);
    if (restore) link_node(s, n);
    else { n->next = s->available; s->available = n; }
    pthread_mutex_unlock(&s->lock);
}
__attribute__((noinline)) static void track(void *ptr, size_t size)
{
    if (!ptr) return;
    record r = { .ptr = ptr, .size = size };
    if (stack_depth) {
        void *frames[FRAMES + 2];
        int n = backtrace(frames, (int)stack_depth + 2);
        /* Remove track() and the public interposition wrapper. */
        if (n > 2) {
            r.depth = (unsigned)(n - 2);
            memcpy(r.frames, frames + 2, r.depth * sizeof(void *));
        }
    }
    unsigned index = shard_index(ptr);
    shard *s = &table[index];
    pthread_mutex_lock(&s->lock);
    node *n = take_node(s);
    if (n) {
        r.id = (++s->sequence << 6) | index;
        n->rec = r;
        link_node(s, n);
    } else ++s->dropped;
    pthread_mutex_unlock(&s->lock);
}
static int enter(void)
{
    return nesting++ == 0 && atomic_load_explicit(&enabled, memory_order_relaxed);
}
static void leave(void) { --nesting; }

void *malloc(size_t size)
{
    if (resolving) return boot_alloc(alignof(max_align_t), size);
    ensure_symbols();
    int tracking = enter();
    void *p = next_malloc(size);
    int saved = errno;
    if (tracking) track(p, size);
    leave(); errno = saved;
    return p;
}
void free(void *ptr)
{
    int saved = errno;
    if (!ptr || boot_pointer(ptr)) return;
    if (resolving) {
        if (!next_free) {
            message("mem_snapshot: unsupported recursive free during symbol resolution\n");
            _exit(127);
        }
        next_free(ptr); errno = saved; return;
    }
    ensure_symbols();
    int tracking = enter();
    if (tracking) (void)detach(ptr, 1);
    next_free(ptr);
    leave(); errno = saved;
}
void *calloc(size_t n, size_t size)
{
    if (resolving) {
        if (size && n > SIZE_MAX / size) { errno = ENOMEM; return NULL; }
        void *p = boot_alloc(alignof(max_align_t), n * size);
        if (p) memset(p, 0, n * size);
        return p;
    }
    ensure_symbols();
    int tracking = enter();
    void *p = next_calloc(n, size);
    int saved = errno;
    if (tracking && p) track(p, n * size);
    leave(); errno = saved;
    return p;
}
void *realloc(void *ptr, size_t size)
{
    if (resolving) {
        if (ptr && !boot_pointer(ptr)) {
            message("mem_snapshot: unsupported recursive realloc during symbol resolution\n");
            _exit(127);
        }
        if (ptr && !size) return NULL;
        void *p = boot_alloc(alignof(max_align_t), size);
        if (p && ptr) memcpy(p, ptr, size < boot_size(ptr) ? size : boot_size(ptr));
        return p;
    }
    ensure_symbols();
    int tracking = enter();
    /* Compute ownership before realloc can invalidate the old pointer. */
    unsigned old_index = tracking && ptr ? shard_index(ptr) : 0;
    node *old = tracking ? detach(ptr, 0) : NULL;
    void *p;
    if (boot_pointer(ptr)) {
        p = size ? next_malloc(size) : NULL;
        if (p) memcpy(p, ptr, size < boot_size(ptr) ? size : boot_size(ptr));
    } else p = next_realloc(ptr, size);
    int saved = errno;
    if (tracking) {
        finish_detached(old, old_index, !p && size != 0);
        if (p) track(p, size);
    }
    leave(); errno = saved;
    return p;
}

#define ALIGN_WRAPPER(name) \
void *name(size_t alignment, size_t size) \
{ \
    if (resolving) return boot_alloc(alignment, size); \
    ensure_symbols(); \
    int tracking = enter(); \
    void *p = next_##name(alignment, size); \
    int saved = errno; \
    if (tracking) track(p, size); \
    leave(); errno = saved; \
    return p; \
}
ALIGN_WRAPPER(memalign)
ALIGN_WRAPPER(aligned_alloc)

int posix_memalign(void **result, size_t alignment, size_t size)
{
    int saved = errno;
    if (alignment < sizeof(void *) || alignment % sizeof(void *) ||
        (alignment & (alignment - 1))) return EINVAL;
    if (resolving) {
        void *p = boot_alloc(alignment, size);
        errno = saved;
        if (!p) return ENOMEM;
        *result = p; return 0;
    }
    ensure_symbols();
    int tracking = enter();
    int rc = next_posix_memalign(result, alignment, size);
    if (tracking && rc == 0) track(*result, size);
    leave(); errno = saved;
    return rc;
}
#define PAGE_WRAPPER(name) \
void *name(size_t size) \
{ \
    if (resolving) { \
        size_t page = (size_t)sysconf(_SC_PAGESIZE); \
        if (size > SIZE_MAX - page) { errno = ENOMEM; return NULL; } \
        return boot_alloc(page, (size + page - 1) & ~(page - 1)); \
    } \
    ensure_symbols(); \
    int tracking = enter(); \
    void *p = next_##name(size); \
    int saved = errno; \
    if (tracking) track(p, size); \
    leave(); errno = saved; \
    return p; \
}
PAGE_WRAPPER(valloc)
PAGE_WRAPPER(pvalloc)
size_t malloc_usable_size(void *p)
{
    if (!p) return 0;
    if (boot_pointer(p)) return boot_size(p);
    ensure_symbols();
    return next_usable_size(p);
}
void cfree(void *p) { free(p); }
void free_sized(void *p, size_t size) { (void)size; free(p); }
void free_aligned_sized(void *p, size_t alignment, size_t size)
{ (void)alignment; (void)size; free(p); }

/* No locks, unwinding, or stdio in the signal handler. */
static void request_snapshot(int signal_number)
{
    (void)signal_number;
    int saved = errno;
    int fd = notify_fd;
    if (fd >= 0) {
        char byte = 1;
        ssize_t n;
        do { n = write(fd, &byte, 1); } while (n < 0 && errno == EINTR);
    }
    errno = saved;
}
static int snapshot(uint64_t sequence)
{
    size_t length = SHARDS * capacity * sizeof(record);
    record *copy = mmap(NULL, length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (copy == MAP_FAILED) { message("mem_snapshot: snapshot mmap failed\n"); return -1; }
    size_t count = 0, bytes = 0, dropped = 0, mapped = 0, max_chain = 0;
    for (unsigned i = 0; i < SHARDS; ++i) pthread_mutex_lock(&table[i].lock);
    for (unsigned i = 0; i < SHARDS; ++i) {
        shard *s = &table[i];
        bytes += s->bytes; dropped += s->dropped; mapped += s->mapped;
        for (unsigned b = 0; b < BUCKETS; ++b) {
            size_t chain = 0;
            for (node *n = s->buckets[b]; n; n = n->next) {
                copy[count++] = n->rec; ++chain;
            }
            if (chain > max_chain) max_chain = chain;
        }
    }
    for (unsigned i = SHARDS; i-- > 0;) pthread_mutex_unlock(&table[i].lock);
    emit("BEGIN seq=%llu pid=%ld count=%zu bytes=%zu dropped=%zu "
            "metadata_bytes=%zu node_size=%zu max_chain=%zu bootstrap_bytes=%zu depth=%u\n",
            (unsigned long long)sequence, (long)owner, count, bytes, dropped,
            mapped * sizeof(node), sizeof(node), max_chain,
            atomic_load_explicit(&boot_used, memory_order_relaxed), stack_depth);
    for (size_t i = 0; i < count; ++i) {
        record *r = &copy[i];
        emit("ALLOC id=%llu ptr=%p size=%zu depth=%u\n",
                (unsigned long long)r->id, r->ptr, r->size, r->depth);
        for (unsigned f = 0; f < r->depth; ++f) {
            Dl_info info = {0};
            dladdr(r->frames[f], &info);
            emit("FRAME pc=%p base=%p symbol=%s module=%s\n", r->frames[f],
                    info.dli_fbase, info.dli_sname ? info.dli_sname : "?",
                    info.dli_fname ? info.dli_fname : "?");
        }
    }
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[4096];
        while (fgets(line, sizeof(line), maps)) emit("MAP %s", line);
        fclose(maps);
    } else emit("MAPS_UNAVAILABLE errno=%d\n", errno);
    flush_output();
    if (!output_failed) {
        emit("END seq=%llu\n", (unsigned long long)sequence);
        flush_output();
    }
    munmap(copy, length);
    if (output_failed) message("mem_snapshot: snapshot output failed; missing END means incomplete\n");
    return output_failed ? -1 : 0;
}
static void *worker(void *unused)
{
    (void)unused;
    nesting = 1;
    uint64_t sequence = 0;
    char events[256];
    for (;;) {
        ssize_t n = read(read_fd, events, sizeof(events));
        if (n > 0) { (void)snapshot(++sequence); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    return NULL;
}
static void child_after_fork(void)
{
    atomic_store_explicit(&enabled, 0, memory_order_relaxed);
    int fd = notify_fd;
    notify_fd = -1;
    if (fd >= 0) close(fd);
    if (read_fd >= 0) close(read_fd);
    if (output_fd >= 0) close(output_fd);
    read_fd = output_fd = -1;
}
static int read_option(const char *name, size_t fallback, size_t min, size_t max, size_t *v)
{
    const char *s = getenv(name);
    if (!s) { *v = fallback; return 0; }
    if (*s < '0' || *s > '9') return -1;
    char *end;
    errno = 0;
    unsigned long value = strtoul(s, &end, 10);
    if (errno || *end || value < min || value > max) return -1;
    *v = (size_t)value;
    return 0;
}
__attribute__((constructor)) static void initialize(void)
{
    ++nesting;
    ensure_symbols();
    size_t depth;
    if (read_option("MS_DEPTH", 16, 0, FRAMES, &depth) ||
        read_option("MS_CAPACITY_PER_SHARD", 4096, BATCH, 65536, &capacity) ||
        capacity % BATCH) {
        message("mem_snapshot: invalid configuration; tracking disabled\n");
        --nesting; return;
    }
    stack_depth = (unsigned)depth;
    struct sigaction previous;
    if (sigaction(SIGUSR2, NULL, &previous) || previous.sa_handler != SIG_DFL) {
        message("mem_snapshot: SIGUSR2 already in use; tracking disabled\n");
        --nesting; return;
    }
    for (unsigned i = 0; i < SHARDS; ++i) {
        if (pthread_mutex_init(&table[i].lock, NULL)) {
            message("mem_snapshot: mutex initialization failed; tracking disabled\n");
            --nesting; return;
        }
    }
    if (depth) { void *warm[FRAMES]; (void)backtrace(warm, FRAMES); }
    owner = getpid();
    char default_path[128];
    snprintf(default_path, sizeof(default_path), "/tmp/mem_snapshot.%ld.log", (long)owner);
    const char *path = getenv("MS_OUTPUT");
    if (!path) path = default_path;
    output_fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output_fd < 0) {
        message("mem_snapshot: cannot create MS_OUTPUT (must not exist); tracking disabled\n");
        --nesting; return;
    }
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC)) goto fail;
    read_fd = pipes[0]; notify_fd = pipes[1];
    if (fcntl(pipes[1], F_SETFL, O_NONBLOCK) < 0) goto fail;
    if (pthread_atfork(NULL, NULL, child_after_fork)) goto fail;
    struct sigaction action = { .sa_handler = request_snapshot, .sa_flags = SA_RESTART };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR2, &action, NULL)) goto fail;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL)) {
        sigaction(SIGUSR2, &previous, NULL);
        goto fail;
    }
    pthread_detach(thread);
    atomic_store_explicit(&enabled, 1, memory_order_relaxed);
    --nesting;
    return;
fail:
    message("mem_snapshot: startup failed; tracking disabled\n");
    if (notify_fd >= 0) close(notify_fd);
    notify_fd = -1;
    if (read_fd >= 0) close(read_fd);
    read_fd = -1;
    close(output_fd); output_fd = -1;
    --nesting;
}
