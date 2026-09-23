#ifndef PMM_H
#define PMM_H
#include <stdint.h>
#include <boot_info.h>

/* ============ Конфигурация ============ */

#define PMM_PAGE_SIZE       4096ULL
#define PMM_MAX_REGIONS     128

/* Флаги регионов */
#define PMM_REGION_USABLE    (1u << 0)   /* можно выделять страницы отсюда */
#define PMM_REGION_RESERVED  (1u << 1)   /* нельзя трогать */
#define PMM_REGION_KERNEL    (1u << 2)   /* занято ядром */
#define PMM_REGION_ACPI      (1u << 3)   /* ACPI: можно позже освободить */
#define PMM_REGION_DMA       (1u << 4)   /* подходит для DMA (<16 МБ) */

/* ============ Типы данных ============ */

typedef struct {
    uint64_t base;      /* физический адрес начала региона */
    uint64_t pages;     /* размер в страницах 4 КБ */
    uint32_t type;      /* mem_desc_t::Type (EfiConventionalMemory, ...) */
    uint32_t flags;     /* PMM_REGION_* */
} pmm_region_t;

/* ============ Инициализация ============ */

/* Инициализирует PMM по карте памяти из boot_info.
 * Находит все RAM-регионы, строит bitmap, помечает занятые области
 * (ядро, стек, BootServices). После этого pmm_alloc_page() работает. */
void pmm_init(const boot_info_t *bi);

/* ============ Статистика ============ */

uint64_t pmm_stat_total_pages(void);      /* всего под управлением */
uint64_t pmm_stat_free_pages(void);       /* свободных */
uint64_t pmm_stat_used_pages(void);       /* занятых */
uint64_t pmm_stat_total_bytes(void);
uint64_t pmm_stat_free_bytes(void);

/* ============ Аллокация ============ */

/* Выделить одну страницу. Возвращает физический адрес или NULL. */
void    *pmm_alloc_page(void);

/* Выделить count подряд идущих страниц. Возвращает физический адрес
 * или NULL. Не используйте для count > 32 без нужды — поиск медленный. */
void    *pmm_alloc_pages(uint64_t count);

/* Освободить страницу по физическому адресу. */
void     pmm_free_page(void *phys);

/* Освободить диапазон. */
void     pmm_free_pages(void *phys, uint64_t count);

/* ============ Управление регионами ============ */

/* Доступ к таблице регионов (для shell, отладки). */
const pmm_region_t *pmm_regions(int *count_out);

/* Пометить весь регион занятым/свободным. Используется для
 * освобождения BootServices после ExitBootServices. */
void pmm_region_set_used(uint64_t base, uint64_t pages, int used);

/* ============ Отладка ============ */

/* Краткая статистика. */
void pmm_dump_stats(void);

/* Полная карта: все регионы + их флаги. */
void pmm_dump_regions(void);

#endif