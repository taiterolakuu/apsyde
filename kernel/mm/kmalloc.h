#ifndef KMALLOC_H
#define KMALLOC_H
#include <stddef.h>

/* Публичный API ядра для мелких аллокаций.
 * Реализация — над kernel/mm/heap.c.
 *
 * Обещания:
 *   - указатели выровнены на 16 байт
 *   - kmalloc(0) == NULL
 *   - kfree(NULL) — безопасно
 *   - krealloc(NULL, n) == kmalloc(n)
 *   - krealloc(p, 0) == kfree(p); return NULL
 *   - двойное освобождение → panic
 */

void *kmalloc(size_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void *kcalloc(size_t n, size_t size);

#endif