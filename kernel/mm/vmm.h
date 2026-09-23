#ifndef VMM_H
#define VMM_H

#include <stdint.h>

/* ============ Виртуальные адреса ядра ============ */

/* Higher-half base. Пока используется только для дополнительного
 * маппинга. Ядро и код работают в identity (virt == phys). */
#define KERNEL_VIRT_BASE   0xFFFFFFFF80000000ULL

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
 * к использованию устаревших маппингов от UEFI. */
#define VMM_KERNEL_RW  (VMM_PRESENT | VMM_WRITE)
#define VMM_MMIO       (VMM_PRESENT | VMM_WRITE | VMM_PWT | VMM_PCD)
#define VMM_USER_RW    (VMM_PRESENT | VMM_WRITE | VMM_USER)

/* ============ Address space ============ */

typedef struct address_space {
    uint64_t *pml4;         /* ФИЗИЧЕСКИЙ адрес PML4 (нужен для CR3) */
    uint32_t  refcount;
    uint32_t  flags;
} address_space_t;

/* Глобальное address space ядра — в .bss, всегда доступно */
extern address_space_t *g_kernel_as;

/* ============ Инициализация ============ */

/* Создаёт g_kernel_as, строит таблицы страниц:
 *   - identity mapping для RAM, ядра, framebuffer
 *   - higher-half mapping для тех же страниц (дополнительно)
 *
 * Переключает CR3 на свои таблицы.
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

/* ПОКА identity: virt == phys. Код ядра работает в identity mapping,
 * ядро загружено по низким адресам, стек и RIP — тоже.
 *
 * После этапа B4 (переключение RIP на higher-half) эти функции
 * станут higher-half-формулами. Пока — identity, чтобы не сломать
 * heap, PMM, VMM и остальной код, который обращается к памяти
 * по физическим адресам.
 *
 * Правило: если у вас физический адрес и вы хотите его
 * разыменовать — используйте phys_to_virt. Если у вас указатель
 * и нужно получить физический адрес — virt_to_phys. */
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