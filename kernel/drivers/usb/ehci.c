#include "ehci.h"
#include "../../bus/pci.h"
#include "../../mm/vmm.h"
#include "../../lib/kprintf.h"
#include "../../lib/panic.h"
#include "../../lib/string.h"

/* ============ Состояние ============ */

static pci_device_t  g_ehci_dev;
static int           g_ehci_found   = 0;
static volatile uint8_t *g_ehci_mmio = 0;    /* база MMIO (capability regs) */
static uint8_t       g_caplength    = 0;
static uint32_t      g_hcsparams    = 0;
static uint32_t      g_hccparams    = 0;

/* ============ Доступ к регистрам ============ */

static inline uint32_t mmio_read32(uint32_t off) {
    volatile uint32_t *p = (volatile uint32_t *)(g_ehci_mmio + off);
    return *p;
}

static inline void mmio_write32(uint32_t off, uint32_t val) {
    volatile uint32_t *p = (volatile uint32_t *)(g_ehci_mmio + off);
    *p = val;
}

uint32_t ehci_read_cap(uint32_t off) {
    if (!g_ehci_mmio) return 0;
    return mmio_read32(off);
}

uint32_t ehci_read_op(uint32_t off) {
    if (!g_ehci_mmio) return 0;
    /* Операционные регистры идут после capability */
    return mmio_read32(g_caplength + off);
}

void ehci_write_op(uint32_t off, uint32_t val) {
    if (!g_ehci_mmio) return;
    mmio_write32(g_caplength + off, val);
}

/* ============ Инициализация ============ */

void ehci_init(void) {
    g_ehci_found = 0;

    /* 1. Найти EHCI в PCI */
    if (pci_find_ehci(&g_ehci_dev) <= 0) {
        kprintf("[+] EHCI: не найден\n");
        return;
    }

    g_ehci_found = 1;

    kprintf("[+] EHCI: найден %u:%u.%u (%x:%x)\n",
            (uint32_t)g_ehci_dev.bus,
            (uint32_t)g_ehci_dev.device,
            (uint32_t)g_ehci_dev.function,
            g_ehci_dev.vendor_id, g_ehci_dev.device_id);

    /* 2. Включить MMIO и Bus Master в Command Register */
    uint16_t cmd = pci_read16(g_ehci_dev.bus, g_ehci_dev.device,
                              g_ehci_dev.function, PCI_COMMAND);
    cmd |= 0x0002;   /* Memory Space Enable */
    cmd |= 0x0004;   /* Bus Master Enable */
    pci_write32(g_ehci_dev.bus, g_ehci_dev.device, g_ehci_dev.function,
                PCI_COMMAND, (uint32_t)cmd);
    /* Восстанавливаем верхние 16 бит (Status), которые мы затёрли
     * записью 32-бит. По спецификации запись верхней половины
     * Command Register игнорируется — Status остаётся. */

    /* 3. Читаем BAR0 */
    uint32_t bar0 = pci_read32(g_ehci_dev.bus, g_ehci_dev.device,
                               g_ehci_dev.function, PCI_BAR0);

    if (bar0 == 0 || bar0 == 0xFFFFFFFF) {
        kprintf("[!] EHCI: BAR0 невалиден: %x\n", bar0);
        g_ehci_found = 0;
        return;
    }

    /* BAR0 — MMIO, бит 0 = 0 */
    if (bar0 & 1) {
        kprintf("[!] EHCI: BAR0 не MMIO (bit0=1): %x\n", bar0);
        g_ehci_found = 0;
        return;
    }

    /* Маскируем младшие 4 бита (тип/prefetch) */
    uint64_t mmio_phys = (uint64_t)(bar0 & ~0xF);

    kprintf("[+] EHCI: BAR0 phys = %x\n", mmio_phys);

    /* 4. Размер BAR (4 КБ у EHCI) */
    uint32_t bar_size = pci_bar_size(g_ehci_dev.bus, g_ehci_dev.device,
                                     g_ehci_dev.function, 0);
    if (bar_size == 0) bar_size = 4096;   /* fallback */

    kprintf("[+] EHCI: BAR0 size = %u байт\n", bar_size);

    /* 5. Мапим MMIO в identity (phys == virt).
     *    Это упрощение: MMIO-адрес 0xFEBxxxxx не влезает
     *    в KERNEL_VIRT_BASE + phys из-за переполнения uint64_t.
     *    Позже сделаем нормальный ioremap. */
    uint64_t pages = (bar_size + 4095) / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = (mmio_phys & ~0xFFFULL) + i * 4096;
        int r = vmm_map(g_kernel_as, phys, phys, VMM_MMIO);
        if (r != 0) {
            kprintf("[!] EHCI: vmm_map failed for %x\n", phys);
            g_ehci_found = 0;
            return;
        }
    }

    g_ehci_mmio = (volatile uint8_t *)mmio_phys;

    kprintf("[+] EHCI: MMIO mapped at %x\n", mmio_phys);

    /* 6. Читаем capability-регистры */
    g_caplength = *(volatile uint8_t *)(g_ehci_mmio + EHCI_CAPLENGTH);
    uint16_t hciversion = *(volatile uint16_t *)(g_ehci_mmio + EHCI_HCIVERSION);
    g_hcsparams = mmio_read32(EHCI_HCSPARAMS);
    g_hccparams = mmio_read32(EHCI_HCCPARAMS);

    kprintf("[+] EHCI: CAPLENGTH = %u\n", (uint32_t)g_caplength);
    kprintf("[+] EHCI: HCIVERSION = %x\n", (uint32_t)hciversion);
    kprintf("[+] EHCI: HCSPARAMS = %x\n", g_hcsparams);
    kprintf("[+] EHCI: HCCPARAMS = %x\n", g_hccparams);

    /* 7. Расшифровка HCSPARAMS */
    uint32_t n_ports = g_hcsparams & 0xF;
    kprintf("[+] EHCI: портов = %u\n", n_ports);

    /* 8. Проверим, что контроллер живой — читаем USBSTS */
    uint32_t sts = ehci_read_op(EHCI_USBSTS);
    kprintf("[+] EHCI: USBSTS = %x%s\n", sts,
            (sts & EHCI_STS_HCH) ? " (HCHalted)" : "");
}

/* ============ Публичный API ============ */

int ehci_present(void) {
    return g_ehci_found;
}

void ehci_dump(void) {
    if (!g_ehci_found) {
        kprintf("EHCI не найден.\n");
        return;
    }

    kprintf("EHCI Controller:\n");
    kprintf("  PCI:        %u:%u.%u  %x:%x\n",
            (uint32_t)g_ehci_dev.bus, (uint32_t)g_ehci_dev.device,
            (uint32_t)g_ehci_dev.function,
            g_ehci_dev.vendor_id, g_ehci_dev.device_id);
    kprintf("  MMIO base:  %x\n", (uint64_t)g_ehci_mmio);
    kprintf("  CAPLENGTH:  %u\n", (uint32_t)g_caplength);
    kprintf("  HCSPARAMS:  %x  (портов: %u)\n",
            g_hcsparams, g_hcsparams & 0xF);
    kprintf("  HCCPARAMS:  %x\n", g_hccparams);

    kprintf("\n  Operational registers:\n");
    kprintf("    USBCMD:  %x\n", ehci_read_op(EHCI_USBCMD));
    kprintf("    USBSTS:  %x\n", ehci_read_op(EHCI_USBSTS));
    kprintf("    USBINTR: %x\n", ehci_read_op(EHCI_USBINTR));
    kprintf("    FRINDEX: %x\n", ehci_read_op(EHCI_FRINDEX));
    kprintf("    CONFIG:  %x\n", ehci_read_op(EHCI_CONFIGFLAG));

    uint32_t n_ports = g_hcsparams & 0xF;
    kprintf("\n  Ports:\n");
    for (uint32_t i = 0; i < n_ports && i < 8; i++) {
        uint32_t portsc = ehci_read_op(EHCI_PORTSC(i));
        kprintf("    Port %u: PORTSC = %x  %s%s\n",
                i, portsc,
                (portsc & EHCI_PORT_CCS) ? "CONNECTED " : "",
                (portsc & EHCI_PORT_PED) ? "ENABLED"   : "");
    }
}