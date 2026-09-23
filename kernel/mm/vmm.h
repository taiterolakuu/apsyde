#ifndef VMM_H
#define VMM_H
#define KERNEL_VIRT_BASE   0xFFFFFFFF80000000ULL
#include <stdint.h>

/* ============ Флаги PTE ============ */

#define VMM_PRESENT   (1ULL << 0)
#define VMM_WRITE     (1ULL << 1)
#define VMM_USER      (1ULL << 2)
#define VMM_PWT       (1ULL << 3)   /* write-through */
#define VMM_PCD       (1ULL << 4)   /* cache disable */
#define VMM_ACCESSED  (1ULL << 5)
#define VMM_DIRTY     (1ULL << 6)
#define VMM_HUGE      (1ULL << 7)
#define VMM_GLOBAL    (1ULL << 8)   /* НЕ используем пока — см. vmm_init */
#define VMM_NX        (1ULL << 63)

/* ============ Композитные флаги ============ */

/* VMM_GLOBAL временно не устанавливаем: при переключении CR3
 * глобальные записи не сбрасываются из TLB, что может привести
 * к использованию устаревших маппингов от UEFI.
 * Включим GLOBAL позже, когда убедимся, что всё стабильно. */
#define VMM_KERNEL_RW  (VMM_PRESENT | VMM_WRITE)
#define VMM_MMIO       (VMM_PRESENT | VMM_WRITE | VMM_PWT | VMM_PCD)
#define VMM_USER_RW    (VMM_PRESENT | VMM_WRITE | VMM_USER)

/* ============ Address space ============ */

typedef struct address_space {
    uint64_t *pml4;         /* физический адрес PML4 (identity mapped) */
    uint32_t  refcount;
    uint32_t  flags;
} address_space_t;

/* Глобальное address space ядра — в .bss, всегда доступно */
extern address_space_t *g_kernel_as;

/* ============ Инициализация ============ */

/* Создаёт g_kernel_as, делает identity mapping для:
 *   - ядро (__kernel_start..__kernel_end, включая стек)
 *   - всех USABLE RAM-регионов
 *   - framebuffer (MMIO)
 *
 * После построения таблиц переключается на них (CR3).
 *
 * ВАЖНО: вызывать ПОСЛЕ pmm_init + heap_init. */
void vmm_init(void);

/* ============ Управление address space ============ */

address_space_t *vmm_create_address_space(void);
void             vmm_destroy_address_space(address_space_t *as);
void             vmm_switch_to(address_space_t *as);

/* ============ Маппинг ============ */

int      vmm_map(address_space_t *as, uint64_t virt, uint64_t phys,
                 uint64_t flags);
int      vmm_map_range(address_space_t *as, uint64_t virt, uint64_t phys,
                       uint64_t pages, uint64_t flags);
int      vmm_unmap(address_space_t *as, uint64_t virt);
uint64_t vmm_get_phys(address_space_t *as, uint64_t virt);

/* ============ Утилиты phys <-> virt ============ */

/* Сейчас identity. При переходе на higher-half изменится на
 * 0xFFFFFFFF80000000 + phys. Весь код ядра должен использовать
 * эти функции, а не прямые преобразования. */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)phys;
}

static inline uint64_t virt_to_phys(void *virt) {
    return (uint64_t)(uintptr_t)virt;
}

/* ============ Отладка ============ */

void vmm_dump_current(void);
void vmm_walk(address_space_t *as, uint64_t virt);

#endif