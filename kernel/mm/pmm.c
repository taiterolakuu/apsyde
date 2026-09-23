#include "pmm.h"
#include "../lib/kprintf.h"
#include "../lib/panic.h"
#include "../lib/string.h"

/* ============ Глобальное состояние ============ */

static uint8_t       g_bitmap[131072];     /* 128 КБ = 1 ГБ RAM */
static uint64_t      g_bitmap_pages = 0;

static pmm_region_t  g_regions[PMM_MAX_REGIONS];
static int           g_region_count = 0;

static uint64_t      g_total_pages = 0;
static uint64_t      g_free_pages  = 0;

static uint64_t      g_cover_lo = 0;
static uint64_t      g_cover_hi = 0;

/* ============ Работа с bitmap ============ */

static inline int bitmap_test(uint64_t idx) {
    return (g_bitmap[idx >> 3] >> (idx & 7)) & 1;
}

static inline void bitmap_set(uint64_t idx) {
    g_bitmap[idx >> 3] |= (1u << (idx & 7));
}

static inline void bitmap_clear(uint64_t idx) {
    g_bitmap[idx >> 3] &= ~(1u << (idx & 7));
}

static int64_t phys_to_bit(uint64_t phys) {
    if (phys < g_cover_lo || phys >= g_cover_hi) return -1;
    if (phys & (PMM_PAGE_SIZE - 1)) return -1;
    return (int64_t)((phys - g_cover_lo) / PMM_PAGE_SIZE);
}

static uint64_t bit_to_phys(uint64_t bit) {
    return g_cover_lo + bit * PMM_PAGE_SIZE;
}

/* ============ Классификация UEFI-типов ============ */

static uint32_t classify_uefi_type(uint32_t uefi_type) {
    switch (uefi_type) {
        case EFI_CONVENTIONAL_MEMORY:
            return PMM_REGION_USABLE;

        case EFI_LOADER_CODE:
        case EFI_LOADER_DATA:
            /* Память, выделенная загрузчиком под ядро, boot_info и др.
             * Помечаем как RESERVED — там наши критичные данные. */
            return PMM_REGION_RESERVED;

        case EFI_BOOT_SERVICES_CODE:
        case EFI_BOOT_SERVICES_DATA:
            /* После ExitBootServices формально можно освободить.
             * Пока держим USABLE, но в pmm_init пометим как занятые. */
            return PMM_REGION_USABLE;

        case EFI_RUNTIME_SERVICES_CODE:
        case EFI_RUNTIME_SERVICES_DATA:
            return PMM_REGION_RESERVED;

        case EFI_ACPI_RECLAIM_MEMORY:
            return PMM_REGION_ACPI;

        case EFI_ACPI_MEMORY_NVS:
            return PMM_REGION_RESERVED;

        case EFI_UNUSABLE_MEMORY:
        case EFI_RESERVED_MEMORY_TYPE:
        case EFI_MEMORY_MAPPED_IO:
        case EFI_MEMORY_MAPPED_IO_PORT_SPACE:
        case EFI_PAL_CODE:
        case EFI_PERSISTENT_MEMORY:
        default:
            return PMM_REGION_RESERVED;
    }
}

static void add_region(uint64_t base, uint64_t pages, uint32_t type, uint32_t flags) {
    if (g_region_count >= PMM_MAX_REGIONS) {
        kprintf("[!] PMM: превышен лимит регионов (%d)\n", PMM_MAX_REGIONS);
        return;
    }
    g_regions[g_region_count].base  = base;
    g_regions[g_region_count].pages = pages;
    g_regions[g_region_count].type  = type;
    g_regions[g_region_count].flags = flags;
    g_region_count++;
}

/* ============ Инициализация ============ */

void pmm_init(const boot_info_t *bi) {
    if (!bi || !bi->memory_map) panic("pmm_init: нет карты памяти");

    g_region_count = 0;
    g_total_pages = 0;
    g_free_pages = 0;

    /* --- Проход 1: строим таблицу регионов --- */
    uint64_t cover_lo = UINT64_MAX;
    uint64_t cover_hi = 0;

    uint8_t *p = (uint8_t *)bi->memory_map;
    uint64_t desc_count = bi->memory_map_size / bi->descriptor_size;

    for (uint64_t i = 0; i < desc_count; i++) {
        mem_desc_t *d = (mem_desc_t *)(p + i * bi->descriptor_size);

        if (d->NumberOfPages == 0) continue;

        /* ВСЁ ниже 1 МБ — BIOS/IVT/BDA/EBDA. Никогда не трогаем. */
        if (d->PhysicalStart < 0x100000) {
            add_region(d->PhysicalStart, d->NumberOfPages,
                       d->Type, PMM_REGION_RESERVED);
            continue;
        }

        uint32_t flags = classify_uefi_type(d->Type);

        /* Регионы без полезной памяти: сохраняем только крупные для отладки */
        if ((flags & PMM_REGION_USABLE) == 0) {
            if (d->NumberOfPages >= 256) {
                add_region(d->PhysicalStart, d->NumberOfPages,
                           d->Type, flags);
            }
            continue;
        }

        /* Регион, откуда можно выделять. Покрываем его bitmap'ом. */
        uint64_t base = d->PhysicalStart;
        uint64_t end  = base + d->NumberOfPages * PMM_PAGE_SIZE;

        add_region(base, d->NumberOfPages, d->Type, flags);

        if (base < cover_lo) cover_lo = base;
        if (end  > cover_hi) cover_hi = end;

        g_total_pages += d->NumberOfPages;
    }

    if (g_total_pages == 0)
        panic("pmm_init: нет ни одного RAM-региона");

    g_cover_lo = cover_lo;
    g_cover_hi = cover_hi;

    uint64_t cover_pages = (cover_hi - cover_lo) / PMM_PAGE_SIZE;
    uint64_t bitmap_bytes = (cover_pages + 7) / 8;

    if (bitmap_bytes > sizeof(g_bitmap))
        panic("pmm_init: bitmap не влезает (%u > %u байт)",
              (uint32_t)bitmap_bytes, (uint32_t)sizeof(g_bitmap));

    g_bitmap_pages = cover_pages;

    /* --- Проход 2: зануляем bitmap --- */
    for (uint64_t i = 0; i < bitmap_bytes; i++) g_bitmap[i] = 0;

    /* Стартуем с «всё занято» — безопаснее. Потом снимем биты для USABLE. */
    for (uint64_t i = 0; i < cover_pages; i++) bitmap_set(i);

    /* --- Проход 3: помечаем USABLE-регионы как свободные --- */
    uint64_t free_pages = 0;
    for (int i = 0; i < g_region_count; i++) {
        pmm_region_t *r = &g_regions[i];
        if ((r->flags & PMM_REGION_USABLE) == 0) continue;

        uint64_t start_bit = (r->base - g_cover_lo) / PMM_PAGE_SIZE;
        for (uint64_t j = 0; j < r->pages; j++) {
            if (start_bit + j >= cover_pages) break;
            bitmap_clear(start_bit + j);
            free_pages++;
        }
    }

    /* --- Проход 4: помечаем страницы ядра как занятые --- */
    extern char __kernel_start[];
    extern char __kernel_end[];

    uint64_t kstart = (uint64_t)__kernel_start & ~(PMM_PAGE_SIZE - 1);
    uint64_t kend   = ((uint64_t)__kernel_end + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    int64_t  kbit   = phys_to_bit(kstart);

    if (kbit >= 0) {
        uint64_t kcount = (kend - kstart) / PMM_PAGE_SIZE;
        for (uint64_t i = 0; i < kcount; i++) {
            uint64_t bit = (uint64_t)kbit + i;
            if (bit >= cover_pages) break;
            if (!bitmap_test(bit)) {
                bitmap_set(bit);
                free_pages--;
            }
        }
        kprintf("[+] PMM: ядро занимает %x..%x (%u страниц)\n",
                kstart, kend, (uint32_t)kcount);
    }

    /* --- Проход 5: помечаем стек как занятый --- */
    extern char __stack_bottom[];
    extern char __stack_top[];

    uint64_t sstart = (uint64_t)__stack_bottom & ~(PMM_PAGE_SIZE - 1);
    uint64_t send   = ((uint64_t)__stack_top + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    int64_t  sbit   = phys_to_bit(sstart);

    if (sbit >= 0) {
        uint64_t scount = (send - sstart) / PMM_PAGE_SIZE;
        for (uint64_t i = 0; i < scount; i++) {
            uint64_t bit = (uint64_t)sbit + i;
            if (bit >= cover_pages) break;
            if (!bitmap_test(bit)) {
                bitmap_set(bit);
                free_pages--;
            }
        }
        kprintf("[+] PMM: стек занимает %x..%x (%u страниц)\n",
                sstart, send, (uint32_t)scount);
    }

    /* --- Проход 6: BootServices пока помечаем как занятые --- */
    for (int i = 0; i < g_region_count; i++) {
        pmm_region_t *r = &g_regions[i];
        if (r->type == EFI_BOOT_SERVICES_CODE ||
            r->type == EFI_BOOT_SERVICES_DATA) {
            uint64_t start_bit = (r->base - g_cover_lo) / PMM_PAGE_SIZE;
            for (uint64_t j = 0; j < r->pages; j++) {
                uint64_t bit = start_bit + j;
                if (bit >= cover_pages) break;
                if (!bitmap_test(bit)) {
                    bitmap_set(bit);
                    free_pages--;
                }
            }
        }
    }

    g_free_pages = free_pages;

    kprintf("[+] PMM: %u страниц всего (%u МБ), %u свободно (%u МБ)\n",
            (uint32_t)g_total_pages,
            (uint32_t)(g_total_pages * PMM_PAGE_SIZE / (1024 * 1024)),
            (uint32_t)g_free_pages,
            (uint32_t)(g_free_pages * PMM_PAGE_SIZE / (1024 * 1024)));
    kprintf("[+] PMM: bitmap покрывает %x..%x (%u КБ)\n",
            g_cover_lo, g_cover_hi,
            (uint32_t)((cover_pages + 7) / 8 / 1024));
}

/* ============ Аллокация ============ */

void *pmm_alloc_page(void) {
    if (g_free_pages == 0) return NULL;

    for (uint64_t i = 0; i < g_bitmap_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            g_free_pages--;
            return (void *)bit_to_phys(i);
        }
    }
    return NULL;
}

void *pmm_alloc_pages(uint64_t count) {
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_page();
    if (count > g_free_pages) return NULL;

    uint64_t run = 0;
    for (uint64_t i = 0; i < g_bitmap_pages; i++) {
        if (bitmap_test(i)) {
            run = 0;
            continue;
        }
        run++;
        if (run == count) {
            uint64_t start = i + 1 - count;
            for (uint64_t j = 0; j < count; j++) bitmap_set(start + j);
            g_free_pages -= count;
            return (void *)bit_to_phys(start);
        }
    }
    return NULL;
}

void pmm_free_page(void *phys) {
    pmm_free_pages(phys, 1);
}

void pmm_free_pages(void *phys, uint64_t count) {
    if (!phys || count == 0) return;

    int64_t bit = phys_to_bit((uint64_t)phys);
    if (bit < 0) return;

    for (uint64_t i = 0; i < count; i++) {
        uint64_t b = (uint64_t)bit + i;
        if (b >= g_bitmap_pages) break;
        if (bitmap_test(b)) {
            bitmap_clear(b);
            g_free_pages++;
        }
    }
}

/* ============ Управление регионами ============ */

const pmm_region_t *pmm_regions(int *count_out) {
    if (count_out) *count_out = g_region_count;
    return g_regions;
}

void pmm_region_set_used(uint64_t base, uint64_t pages, int used) {
    int64_t bit = phys_to_bit(base & ~(PMM_PAGE_SIZE - 1));
    if (bit < 0) return;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t b = (uint64_t)bit + i;
        if (b >= g_bitmap_pages) break;

        int was_used = bitmap_test(b);
        if (used && !was_used) {
            bitmap_set(b);
            g_free_pages--;
        } else if (!used && was_used) {
            bitmap_clear(b);
            g_free_pages++;
        }
    }
}

/* ============ Статистика ============ */

uint64_t pmm_stat_total_pages(void) { return g_total_pages; }
uint64_t pmm_stat_free_pages(void)  { return g_free_pages; }
uint64_t pmm_stat_used_pages(void)  { return g_total_pages - g_free_pages; }
uint64_t pmm_stat_total_bytes(void) { return g_total_pages * PMM_PAGE_SIZE; }
uint64_t pmm_stat_free_bytes(void)  { return g_free_pages  * PMM_PAGE_SIZE; }