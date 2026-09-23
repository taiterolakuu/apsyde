#include "heap.h"
#include "pmm.h"
#include "../lib/kprintf.h"
#include "../lib/panic.h"
#include "../lib/string.h"

/* ============ Внутренние структуры ============ */

typedef struct block_header {
    uint32_t magic;
    uint32_t size;
    struct block_header *next_free;
    uint32_t flags;
    uint32_t _pad0;
    uint64_t _pad1;
} block_header_t;

#define HEADER_SIZE  sizeof(block_header_t)

_Static_assert(sizeof(block_header_t) == 32,
               "block_header_t must be 32 bytes for 16-byte alignment");

#define ALIGN_UP(x)  (((x) + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1))

static inline block_header_t *next_block(block_header_t *b) {
    return (block_header_t *)((uint8_t *)b + HEADER_SIZE + b->size);
}

/* ============ Состояние heap ============ */

static uint8_t  *g_heap_start = NULL;
static uint8_t  *g_heap_end   = NULL;
static block_header_t *g_free_list = NULL;

static uint64_t g_alloc_count  = 0;
static uint64_t g_free_count   = 0;
static uint64_t g_total_blocks = 0;
static uint64_t g_free_blocks  = 0;

/* ============ Инициализация ============ */

void heap_init(void) {
    void *base = pmm_alloc_pages(HEAP_INITIAL_PAGES);
    if (!base) panic("heap_init: не удалось выделить %u страниц",
                     HEAP_INITIAL_PAGES);

    g_heap_start = (uint8_t *)base;
    g_heap_end   = g_heap_start + HEAP_INITIAL_PAGES * 4096;

    block_header_t *b = (block_header_t *)g_heap_start;
    b->magic     = HEAP_MAGIC_FREE;
    b->size      = (uint32_t)((g_heap_end - g_heap_start) - HEADER_SIZE);
    b->next_free = NULL;
    b->flags     = 1;
    b->_pad0     = 0;
    b->_pad1     = 0;

    g_free_list = b;
    g_total_blocks = 1;
    g_free_blocks  = 1;

    kprintf("[+] Heap: %u КБ по адресу %p (HEADER_SIZE=%u)\n",
            (uint32_t)(HEAP_INITIAL_PAGES * 4096 / 1024), base,
            (uint32_t)HEADER_SIZE);
}

/* ============ Вспомогательные ============ */

static int is_in_heap(void *ptr) {
    uint8_t *p = (uint8_t *)ptr;
    return p >= g_heap_start && p < g_heap_end;
}

/* ============ Основной алгоритм ============ */

void *heap_alloc(size_t size) {
    if (size == 0) return NULL;

    size_t aligned = ALIGN_UP(size);
    if (aligned > (uint32_t)-1) return NULL;

    block_header_t *prev = NULL;
    block_header_t *b = g_free_list;

    while (b) {
        if (b->magic != HEAP_MAGIC_FREE) {
            panic("heap_alloc: повреждён magic в free-list: %x", b->magic);
        }

        if (b->size >= aligned) {
            uint32_t remaining = b->size - (uint32_t)aligned;

            if (remaining >= HEADER_SIZE + HEAP_ALIGNMENT) {
                block_header_t *new_b =
                    (block_header_t *)((uint8_t *)b + HEADER_SIZE + aligned);

                new_b->magic     = HEAP_MAGIC_FREE;
                new_b->size      = remaining - HEADER_SIZE;
                new_b->flags     = 1;
                new_b->_pad0     = 0;
                new_b->_pad1     = 0;
                new_b->next_free = b->next_free;

                if (prev) prev->next_free = new_b;
                else      g_free_list = new_b;

                g_total_blocks++;
            } else {
                if (prev) prev->next_free = b->next_free;
                else      g_free_list = b->next_free;
                g_free_blocks--;
            }

            b->magic = HEAP_MAGIC_USED;
            b->size  = (uint32_t)aligned;
            b->flags = 0;
            b->next_free = NULL;

            g_alloc_count++;
            return (void *)((uint8_t *)b + HEADER_SIZE);
        }

        prev = b;
        b = b->next_free;
    }

    return NULL;
}

void heap_free(void *ptr) {
    if (!ptr) return;

    if (!is_in_heap(ptr)) {
        panic("heap_free: указатель вне heap: %p", ptr);
    }

    block_header_t *b = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);

    if (b->magic == HEAP_MAGIC_FREE) {
        panic("heap_free: двойное освобождение %p", ptr);
    }
    if (b->magic != HEAP_MAGIC_USED) {
        panic("heap_free: повреждён заголовок %p (magic=%x)", ptr, b->magic);
    }

    b->magic = HEAP_MAGIC_FREE;
    b->flags = 1;
    b->next_free = g_free_list;
    g_free_list = b;
    g_free_blocks++;
    g_free_count++;

    block_header_t *next = next_block(b);
    if ((uint8_t *)next < g_heap_end &&
        next->magic == HEAP_MAGIC_FREE) {

        if (g_free_list == next) {
            g_free_list = next->next_free;
        } else {
            block_header_t *prev = g_free_list;
            while (prev && prev->next_free != next) prev = prev->next_free;
            if (prev) prev->next_free = next->next_free;
        }
        g_free_blocks--;

        b->size += HEADER_SIZE + next->size;
        g_total_blocks--;
    }
}

void *heap_realloc(void *ptr, size_t new_size) {
    if (!ptr) return heap_alloc(new_size);

    if (new_size == 0) {
        heap_free(ptr);
        return NULL;
    }

    block_header_t *b = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);

    size_t old_size = b->size;
    size_t copy_size = (old_size < new_size) ? old_size : new_size;

    void *new_ptr = heap_alloc(new_size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, copy_size);
    heap_free(ptr);
    return new_ptr;
}

void *heap_calloc(size_t n, size_t size) {
    if (n == 0 || size == 0) return NULL;
    if (n > ((size_t)-1) / size) return NULL;

    size_t total = n * size;
    void *p = heap_alloc(total);
    if (p) memset(p, 0, total);
    return p;
}

/* ============ Статистика / отладка ============ */

void heap_get_stats(heap_stats_t *out) {
    if (!out) return;

    uint64_t total = (uint64_t)(g_heap_end - g_heap_start);
    uint64_t used_bytes = 0;
    uint64_t used_blocks = 0;

    block_header_t *b = (block_header_t *)g_heap_start;
    while ((uint8_t *)b < g_heap_end) {
        if (b->magic == HEAP_MAGIC_USED) {
            used_bytes += HEADER_SIZE + b->size;
            used_blocks++;
        } else if (b->magic == HEAP_MAGIC_FREE) {
            /* ок */
        } else {
            break;
        }
        b = next_block(b);
    }

    out->total_bytes  = total;
    out->used_bytes   = used_bytes;
    out->free_bytes   = total - used_bytes;
    out->total_blocks = g_total_blocks;
    out->free_blocks  = g_free_blocks;
    out->used_blocks  = used_blocks;
    out->alloc_count  = g_alloc_count;
    out->free_count   = g_free_count;
}

void heap_dump(int max_blocks) {
    kprintf("Heap: %p..%p\n", g_heap_start, g_heap_end);
    kprintf("Free-list: %p\n", g_free_list);
    kprintf("\n");
    kprintf("Адрес              Размер   Статус\n");
    kprintf("------------------------------------\n");

    block_header_t *b = (block_header_t *)g_heap_start;
    int shown = 0;

    while ((uint8_t *)b < g_heap_end) {
        if (shown >= max_blocks) {
            kprintf("... (показаны первые %d)\n", max_blocks);
            break;
        }

        const char *status;
        if (b->magic == HEAP_MAGIC_FREE)      status = "FREE";
        else if (b->magic == HEAP_MAGIC_USED) status = "USED";
        else                                  status = "BAD!";

        kprintf("%p  %u  %s\n", b, b->size, status);

        b = next_block(b);
        shown++;
    }
}

int heap_check(void) {
    block_header_t *b = (block_header_t *)g_heap_start;

    while ((uint8_t *)b < g_heap_end) {
        if (b->magic != HEAP_MAGIC_FREE && b->magic != HEAP_MAGIC_USED) {
            return 0;
        }
        block_header_t *next = next_block(b);
        if ((uint8_t *)next > g_heap_end) return 0;
        if ((uint8_t *)next == (uint8_t *)b) return 0;
        b = next;
    }
    return 1;
}

/* ============ Для VMM ============ */

void heap_get_range(uint64_t *start, uint64_t *end) {
    if (start) *start = (uint64_t)g_heap_start;
    if (end)   *end   = (uint64_t)g_heap_end;
}