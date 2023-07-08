#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

union header {
    struct {
        size_t size;
        unsigned is_free;
        union header *next;
    } s;
    /* force the header to be aligned to 16 bytes */
    char stub[16];
};
typedef union header header_t;

#define MAX_CPU_COUNT 256
static header_t *headers[MAX_CPU_COUNT];  // we have 256 heaps
static pthread_mutex_t locks[MAX_CPU_COUNT];
static int rand_stat;

void *extend_heap_size(size_t size) {
    // mmap itself is thread-safe
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return ptr;
}

void *malloc(size_t size) {
    int thread_hash = pthread_self() % MAX_CPU_COUNT;

    pthread_mutex_lock(&locks[thread_hash]);
    header_t *first = headers[thread_hash];
    if (first == NULL) {
        first = extend_heap_size(sizeof(header_t));
        headers[thread_hash] = first;
    }
    header_t *prev = NULL;
    header_t *cur = first;
    // find the first free block that is large enough
    while (cur->s.is_free == 0 || cur->s.size < size) {
        prev = cur;
        cur = cur->s.next;
        if (cur == NULL) {
            cur = extend_heap_size(sizeof(header_t) + size);
            prev->s.next = cur;
        }
    }
    // mark the block as used
    cur->s.is_free = 0;
    cur->s.size = size;
    pthread_mutex_unlock(&locks[thread_hash]);
    // return the pointer to the user data
    return cur + 1;
}

void *calloc(size_t nitems, size_t nsize) { return malloc(nitems * nsize); }

void free(void *ptr) {
    /*
     * Insert free implementation here
     */

    return;
}

void *realloc(void *ptr, size_t size) {
    void *new_ptr = malloc(size);
    memcpy(new_ptr, ptr, size);
    free(ptr);
    return new_ptr;
}
