#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "kmalloc.h"
#include "../lib/kprintf.h"
#include "../lib/panic.h"
#include "../lib/string.h"
#include <boot_info.h>

/* Экспорт из main.c */
extern const boot_info_t *shell_get_boot_info(void);

/* ============ Адресация таблиц ============ */

#define PML4_INDEX(v)  (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v)  (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)    (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)    (((v) >> 12) & 0x1FF)

#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define TABLE_ENTRIES  512

/* ============ Глобальное состояние ============ */

static address_space_t g_kernel_as_storage;
address_space_t *g_kernel_as = &g_kernel_as_storage;

/* ============ Внутренние функции ============ */

static inline uint64_t *table_ptr(uint64_t phys) {
    return (uint64_t *)phys_to_virt(phys & PTE_ADDR_MASK);
}

static inline int is_page_aligned(uint64_t addr) {
    return (addr & 0xFFF) == 0;
}

static uint64_t alloc_table(void) {
    void *page = pmm_alloc_page();
    if (!page) return 0;
    uint64_t phys = (uint64_t)page;
    memset(phys_to_virt(phys), 0, 4096);
    return phys;
}

static uint64_t get_or_create_table(uint64_t *entry, int create) {
    if (*entry & VMM_PRESENT) {
        return *entry & PTE_ADDR_MASK;
    }
    if (!create) return 0;

    uint64_t phys = alloc_table();
    if (!phys) return 0;

    *entry = phys | VMM_PRESENT | VMM_WRITE | VMM_USER;
    return phys;
}

static inline void invlpg(uint64_t virt) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

/* ============ Маппинг ============ */

int vmm_map(address_space_t *as, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!as || !as->pml4) return -1;
    if (!is_page_aligned(virt) || !is_page_aligned(phys)) return -1;

    uint64_t *pml4 = table_ptr((uint64_t)as->pml4);

    uint64_t pdpt_phys = get_or_create_table(&pml4[PML4_INDEX(virt)], 1);
    if (!pdpt_phys) return -1;
    uint64_t *pdpt = table_ptr(pdpt_phys);

    uint64_t pd_phys = get_or_create_table(&pdpt[PDPT_INDEX(virt)], 1);
    if (!pd_phys) return -1;
    uint64_t *pd = table_ptr(pd_phys);

    uint64_t pt_phys = get_or_create_table(&pd[PD_INDEX(virt)], 1);
    if (!pt_phys) return -1;
    uint64_t *pt = table_ptr(pt_phys);

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags | VMM_PRESENT;

    invlpg(virt);
    return 0;
}

int vmm_map_range(address_space_t *as, uint64_t virt, uint64_t phys,
                  uint64_t pages, uint64_t flags) {
    for (uint64_t i = 0; i < pages; i++) {
        int r = vmm_map(as, virt + i * 4096, phys + i * 4096, flags);
        if (r != 0) return r;
    }
    return 0;
}

int vmm_unmap(address_space_t *as, uint64_t virt) {
    if (!as || !as->pml4) return -1;
    if (!is_page_aligned(virt)) return -1;

    uint64_t *pml4 = table_ptr((uint64_t)as->pml4);

    if (!(pml4[PML4_INDEX(virt)] & VMM_PRESENT)) return -1;
    uint64_t *pdpt = table_ptr(pml4[PML4_INDEX(virt)]);

    if (!(pdpt[PDPT_INDEX(virt)] & VMM_PRESENT)) return -1;
    uint64_t *pd = table_ptr(pdpt[PDPT_INDEX(virt)]);

    if (!(pd[PD_INDEX(virt)] & VMM_PRESENT)) return -1;
    uint64_t *pt = table_ptr(pd[PD_INDEX(virt)]);

    pt[PT_INDEX(virt)] = 0;

    invlpg(virt);
    return 0;
}

uint64_t vmm_get_phys(address_space_t *as, uint64_t virt) {
    if (!as || !as->pml4) return 0;

    uint64_t *pml4 = table_ptr((uint64_t)as->pml4);

    if (!(pml4[PML4_INDEX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pdpt = table_ptr(pml4[PML4_INDEX(virt)]);

    if (!(pdpt[PDPT_INDEX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pd = table_ptr(pdpt[PDPT_INDEX(virt)]);

    if (pd[PD_INDEX(virt)] & VMM_HUGE) {
        uint64_t base = pd[PD_INDEX(virt)] & 0x000FFFFFFFE00000ULL;
        return base + (virt & 0x1FFFFF);
    }

    if (!(pd[PD_INDEX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pt = table_ptr(pd[PD_INDEX(virt)]);

    if (!(pt[PT_INDEX(virt)] & VMM_PRESENT)) return 0;

    return (pt[PT_INDEX(virt)] & PTE_ADDR_MASK) | (virt & 0xFFF);
}

/* ============ Вспомогательное ============ */

static void map_higher_half(uint64_t phys_start, uint64_t phys_end,
                            uint64_t flags, const char *what) {
    uint64_t start = phys_start & ~0xFFFULL;
    uint64_t end   = (phys_end + 0xFFF) & ~0xFFFULL;
    uint64_t pages = (end - start) / 4096;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = start + i * 4096;
        uint64_t virt = KERNEL_VIRT_BASE + phys;
        vmm_map(g_kernel_as, virt, phys, flags);
    }

    kprintf("[+] VMM: %s higher-half mapped: %u страниц (%x..%x)\n",
            what, (uint32_t)pages,
            KERNEL_VIRT_BASE + start,
            KERNEL_VIRT_BASE + end);
}

/* ============ Инициализация ============ */

void vmm_init(void) {
    uint64_t pml4_phys = alloc_table();
    if (!pml4_phys) panic("vmm_init: не удалось выделить PML4");

    g_kernel_as->pml4 = (uint64_t *)pml4_phys;
    g_kernel_as->refcount = 1;
    g_kernel_as->flags = 0;

    /* ============ 1. Ядро — identity ============ */

    extern char __kernel_start[];
    extern char __kernel_end[];

    uint64_t kstart = (uint64_t)__kernel_start & ~0xFFFULL;
    uint64_t kend   = ((uint64_t)__kernel_end + 0xFFF) & ~0xFFFULL;
    uint64_t kpages = (kend - kstart) / 4096;

    for (uint64_t i = 0; i < kpages; i++) {
        uint64_t addr = kstart + i * 4096;
        vmm_map(g_kernel_as, addr, addr, VMM_KERNEL_RW);
    }
    kprintf("[+] VMM: kernel mapped: %u страниц (%x..%x)\n",
            (uint32_t)kpages, kstart, kend);

    /* ============ 2. RAM — identity + higher-half ============ */

    int region_count = 0;
    const pmm_region_t *regions = pmm_regions(&region_count);

    uint64_t ram_pages = 0;

    for (int i = 0; i < region_count; i++) {
        const pmm_region_t *r = &regions[i];

        if (r->flags & PMM_REGION_USABLE) {
            for (uint64_t j = 0; j < r->pages; j++) {
                uint64_t phys = r->base + j * 4096;

                vmm_map(g_kernel_as, phys, phys, VMM_KERNEL_RW);
                vmm_map(g_kernel_as, KERNEL_VIRT_BASE + phys, phys,
                        VMM_KERNEL_RW);

                ram_pages++;
            }
        }
    }

    kprintf("[+] VMM: RAM mapped: %u страниц (%u МБ), identity + higher-half\n",
            (uint32_t)ram_pages,
            (uint32_t)(ram_pages * 4096 / (1024 * 1024)));

    /* ============ 3. Framebuffer — ТОЛЬКО identity ============ */

    /* Framebuffer = 0x80000000 (2 ГБ). KERNEL_VIRT_BASE + phys даёт
     * переполнение uint64_t, поэтому higher-half маппинг невозможен
     * без отдельного виртуального региона для MMIO.
     *
     * Это нормально: fb.c обращается к framebuffer через identity-адрес
     * (тот, что дал UEFI), и он работает. */

    const boot_info_t *bi = shell_get_boot_info();
    if (bi && bi->framebuffer_base) {
        uint64_t fb_start = bi->framebuffer_base & ~0xFFFULL;
        uint64_t fb_end   = ((uint64_t)bi->framebuffer_base +
                             bi->framebuffer_size + 0xFFF) & ~0xFFFULL;
        uint64_t fb_pages = (fb_end - fb_start) / 4096;

        for (uint64_t i = 0; i < fb_pages; i++) {
            uint64_t phys = fb_start + i * 4096;
            vmm_map(g_kernel_as, phys, phys, VMM_MMIO);
        }
        kprintf("[+] VMM: framebuffer mapped: %u страниц (identity only, MMIO)\n",
                (uint32_t)fb_pages);
    }

    /* ============ 4. Higher-half для ядра и heap (явно) ============ */

    map_higher_half(kstart, kend, VMM_KERNEL_RW, "kernel");

    uint64_t hstart = 0, hend = 0;
    heap_get_range(&hstart, &hend);

    if (hstart && hend > hstart) {
        map_higher_half(hstart, hend, VMM_KERNEL_RW, "heap");
    }

    /* ============ 5. Self-check ============ */

    uint64_t self_addr = (uint64_t)vmm_init;
    uint64_t self_phys = vmm_get_phys(g_kernel_as, self_addr);

    kprintf("[+] VMM: self-check virt %x -> phys %x: %s\n",
            self_addr, self_phys,
            (self_phys == self_addr) ? "OK" : "FAIL");

    if (self_phys != self_addr) {
        panic("vmm_init: self-check failed — переключение CR3 отменено");
    }

    /* Проверяем higher-half маппинг */
    uint64_t hh_addr = KERNEL_VIRT_BASE + self_addr;
    uint64_t hh_phys = vmm_get_phys(g_kernel_as, hh_addr);

    kprintf("[+] VMM: higher-half self-check virt %x -> phys %x: %s\n",
            hh_addr, hh_phys,
            (hh_phys == self_addr) ? "OK" : "FAIL");

    if (hh_phys != self_addr) {
        panic("vmm_init: higher-half self-check failed");
    }

    /* ============ 6. Переключаемся на свои таблицы ============ */

    kprintf("[+] VMM: переключаемся на CR3 = %x\n", pml4_phys);
    vmm_switch_to(g_kernel_as);
    kprintf("[+] VMM: CR3 переключён, ядро живо\n");
}

/* ============ Управление address space ============ */

address_space_t *vmm_create_address_space(void) {
    address_space_t *as = (address_space_t *)kmalloc(sizeof(address_space_t));
    if (!as) return NULL;

    uint64_t pml4_phys = alloc_table();
    if (!pml4_phys) {
        kfree(as);
        return NULL;
    }

    as->pml4     = (uint64_t *)pml4_phys;
    as->refcount = 1;
    as->flags    = 0;

    uint64_t *dst = table_ptr(pml4_phys);
    uint64_t *src = table_ptr((uint64_t)g_kernel_as->pml4);

    for (int i = 0; i < TABLE_ENTRIES; i++) {
        dst[i] = src[i];
    }

    return as;
}

void vmm_destroy_address_space(address_space_t *as) {
    if (!as) return;
    if (as == g_kernel_as) return;

    if (as->refcount > 1) {
        as->refcount--;
        return;
    }

    kfree(as);
}

void vmm_switch_to(address_space_t *as) {
    if (!as || !as->pml4) panic("vmm_switch_to: null as");

    uint64_t cr3 = (uint64_t)as->pml4;

    __asm__ volatile (
        "mov %0, %%cr3\n"
        "mov %%cr3, %%rax\n"
        "mov %%rax, %%cr3\n"
        : : "r"(cr3) : "rax", "memory"
    );
}

/* ============ Отладка ============ */

void vmm_dump_current(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    kprintf("CR3:            %x\n", cr3);
    kprintf("g_kernel_as:    %p\n", g_kernel_as);
    kprintf("kernel PML4:    %p\n", g_kernel_as->pml4);
    kprintf("refcount:       %u\n", g_kernel_as->refcount);
    kprintf("KERNEL_VIRT_BASE: %x\n", KERNEL_VIRT_BASE);
}

void vmm_walk(address_space_t *as, uint64_t virt) {
    if (!as) as = g_kernel_as;

    kprintf("Walk %x в AS %p:\n", virt, as);

    uint64_t i4 = PML4_INDEX(virt);
    uint64_t i3 = PDPT_INDEX(virt);
    uint64_t i2 = PD_INDEX(virt);
    uint64_t i1 = PT_INDEX(virt);

    uint64_t *pml4 = table_ptr((uint64_t)as->pml4);
    uint64_t e4 = pml4[i4];
    kprintf("  PML4[%u] = %x %s\n", (uint32_t)i4, e4,
            (e4 & VMM_PRESENT) ? "" : "(not present)");
    if (!(e4 & VMM_PRESENT)) return;

    uint64_t *pdpt = table_ptr(e4);
    uint64_t e3 = pdpt[i3];
    kprintf("  PDPT[%u] = %x %s\n", (uint32_t)i3, e3,
            (e3 & VMM_PRESENT) ? "" : "(not present)");
    if (!(e3 & VMM_PRESENT)) return;

    uint64_t *pd = table_ptr(e3);
    uint64_t e2 = pd[i2];
    kprintf("  PD[%u]   = %x %s\n", (uint32_t)i2, e2,
            (e2 & VMM_PRESENT) ? "" : "(not present)");
    if (!(e2 & VMM_PRESENT)) return;

    if (e2 & VMM_HUGE) {
        uint64_t base = e2 & 0x000FFFFFFFE00000ULL;
        kprintf("  -> 2 MiB huge page, phys = %x\n", base + (virt & 0x1FFFFF));
        return;
    }

    uint64_t *pt = table_ptr(e2);
    uint64_t e1 = pt[i1];
    kprintf("  PT[%u]   = %x %s\n", (uint32_t)i1, e1,
            (e1 & VMM_PRESENT) ? "" : "(not present)");
    if (!(e1 & VMM_PRESENT)) return;

    uint64_t phys = (e1 & PTE_ADDR_MASK) | (virt & 0xFFF);
    kprintf("  -> phys = %x\n", phys);
}