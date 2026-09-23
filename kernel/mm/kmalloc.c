#include "kmalloc.h"
#include "heap.h"

void *kmalloc(size_t size) {
    return heap_alloc(size);
}

void kfree(void *ptr) {
    heap_free(ptr);
}

void *krealloc(void *ptr, size_t new_size) {
    return heap_realloc(ptr, new_size);
}

void *kcalloc(size_t n, size_t size) {
    return heap_calloc(n, size);
}