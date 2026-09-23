#ifndef HEAP_H
#define HEAP_H
#include <stdint.h>
#include <stddef.h>

/* ============ Конфигурация ============ */

#define HEAP_ALIGNMENT          16          /* выравнивание возвращаемых указателей */
#define HEAP_INITIAL_PAGES      256         /* 1 МБ на старте */
#define HEAP_GROW_PAGES         256         /* шаг роста */

/* Magic-значения для заголовка блока */
#define HEAP_MAGIC_FREE         0xC0FFEE00u
#define HEAP_MAGIC_USED         0xC0FFEE01u

/* ============ Инициализация ============ */

/* Вызывать после pmm_init. Выделяет HEAP_INITIAL_PAGES страниц
 * и формирует из них один большой свободный блок. */
void heap_init(void);

/* ============ Основной API ============ */

void *heap_alloc(size_t size);
void  heap_free(void *ptr);
void *heap_realloc(void *ptr, size_t new_size);
void *heap_calloc(size_t n, size_t size);

/* ============ Статистика / отладка ============ */

typedef struct {
    uint64_t total_bytes;    /* всего памяти под heap */
    uint64_t used_bytes;     /* занято user-данными + заголовками */
    uint64_t free_bytes;     /* свободно */
    uint64_t total_blocks;   /* всего блоков */
    uint64_t free_blocks;    /* свободных блоков */
    uint64_t used_blocks;    /* занятых блоков */
    uint64_t alloc_count;    /* сколько раз вызывали alloc */
    uint64_t free_count;     /* сколько раз вызывали free */
} heap_stats_t;

void heap_get_stats(heap_stats_t *out);

/* Печатает карту блоков в kprintf (только первые N для краткости). */
void heap_dump(int max_blocks);

/* Проверка целостности heap: все ли magic-поля корректны,
 * нет ли дублирующихся блоков. Возвращает 1 если OK, 0 если повреждён. */
int heap_check(void);

/* ============ Для VMM ============ */

/* Возвращает физические границы heap. Используется VMM, чтобы
 * замапить heap в higher-half (этап B3). */
void heap_get_range(uint64_t *start, uint64_t *end);

#endif