#include "ehci.h"
#include "../../bus/pci.h"
#include "../../mm/vmm.h"
#include "../../lib/kprintf.h"
#include "../../lib/panic.h"
#include "../../lib/string.h"

/* ============ Состояние ============ */

static pci_device_t  g_ehci_dev;
static int           g_ehci_found   = 0;
static volatile uint8_t *g_ehci_mmio = 0;
static uint8_t       g_caplength    = 0;
static uint32_t      g_hcsparams    = 0;
static uint32_t      g_hccparams    = 0;
static int           g_num_ports    = 0;

/* ============ DMA-пулы (в .bss, identity-mapped) ============ */

static ehci_qh_t  g_qh_pool[8]  __attribute__((aligned(32)));
static ehci_qtd_t g_qtd_pool[16] __attribute__((aligned(32)));
static int        g_qh_used  = 0;
static int        g_qtd_used = 0;

static uint8_t g_setup_buf[8]  __attribute__((aligned(64)));
static uint8_t g_data_buf[256] __attribute__((aligned(64)));

static ehci_qh_t g_async_head __attribute__((aligned(32)));

/* ============ Доступ к регистрам ============ */

static inline uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(g_ehci_mmio + off);
}

static inline void mmio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_ehci_mmio + off) = val;
}

uint32_t ehci_read_cap(uint32_t off) {
    if (!g_ehci_mmio) return 0;
    return mmio_read32(off);
}

uint32_t ehci_read_op(uint32_t off) {
    if (!g_ehci_mmio) return 0;
    return mmio_read32(g_caplength + off);
}

void ehci_write_op(uint32_t off, uint32_t val) {
    if (!g_ehci_mmio) return;
    mmio_write32(g_caplength + off, val);
}

/* ============ Аллокация из пулов ============ */

static ehci_qh_t *qh_alloc(void) {
    if (g_qh_used >= 8) return 0;
    ehci_qh_t *qh = &g_qh_pool[g_qh_used++];
    memset(qh, 0, sizeof(*qh));
    return qh;
}

static ehci_qtd_t *qtd_alloc(void) {
    if (g_qtd_used >= 16) return 0;
    ehci_qtd_t *qtd = &g_qtd_pool[g_qtd_used++];
    memset(qtd, 0, sizeof(*qtd));
    return qtd;
}

/* ============ Инициализация контроллера ============ */

static int wait_hcreset_clear(void) {
    for (int i = 0; i < 100000; i++) {
        uint32_t cmd = ehci_read_op(EHCI_USBCMD);
        if (!(cmd & EHCI_CMD_HCRESET)) return 0;
    }
    return -1;
}

void ehci_init_controller(void) {
    if (!g_ehci_found) return;

    kprintf("[+] EHCI: инициализация контроллера...\n");

    /* 1. Остановить HC */
    uint32_t cmd = ehci_read_op(EHCI_USBCMD);
    cmd &= ~EHCI_CMD_RS;
    ehci_write_op(EHCI_USBCMD, cmd);

    for (int i = 0; i < 100000; i++) {
        if (ehci_read_op(EHCI_USBSTS) & EHCI_STS_HCH) break;
    }

    /* 2. HCRESET */
    cmd = ehci_read_op(EHCI_USBCMD);
    ehci_write_op(EHCI_USBCMD, cmd | EHCI_CMD_HCRESET);

    if (wait_hcreset_clear() != 0) {
        kprintf("[!] EHCI: HCRESET не завершился\n");
        return;
    }
    kprintf("[+] EHCI: HCRESET завершён\n");

    /* 3. CTRLDSSEGMENT = 0 */
    ehci_write_op(EHCI_CTRLDSSEGMENT, 0);

    /* 4. Head QH для async list */
    memset(&g_async_head, 0, sizeof(g_async_head));

    uint32_t head_phys = virt_to_phys(&g_async_head);
    g_async_head.horiz_link   = head_phys | EHCI_QH_TYPE;
    g_async_head.ep_char      = QH_H;
    g_async_head.next_qtd     = EHCI_TERMINATE;
    g_async_head.alt_next_qtd = EHCI_TERMINATE;
    g_async_head.token        = EHCI_TERMINATE;

    kprintf("[+] EHCI: head QH phys = %x\n", head_phys);

    ehci_write_op(EHCI_ASYNCLISTADDR, head_phys);
    ehci_write_op(EHCI_PERIODICLIST, 0);
    ehci_write_op(EHCI_USBINTR, 0);

    /* 5. Запуск: RS + ASE */
    cmd = EHCI_CMD_RS | EHCI_CMD_ASE | EHCI_CMD_ITC(8);
    ehci_write_op(EHCI_USBCMD, cmd);

    for (volatile int i = 0; i < 100000; i++) {}

    uint32_t sts = ehci_read_op(EHCI_USBSTS);
    kprintf("[+] EHCI: USBCMD = %x, USBSTS = %x%s\n",
            ehci_read_op(EHCI_USBCMD), sts,
            (sts & EHCI_STS_HCH) ? " (HCHalted!)" : "");

    /* 6. CONFIGFLAG = 1 */
    uint32_t cfg = ehci_read_op(EHCI_CONFIGFLAG);
    ehci_write_op(EHCI_CONFIGFLAG, cfg | EHCI_CF_CF);
    kprintf("[+] EHCI: CONFIGFLAG = %x\n", ehci_read_op(EHCI_CONFIGFLAG));
}

/* ============ Порты ============ */

int ehci_port_connected(int port) {
    if (port < 0 || port >= g_num_ports) return 0;
    uint32_t portsc = ehci_read_op(EHCI_PORTSC(port));
    return (portsc & EHCI_PORT_CCS) ? 1 : 0;
}

/* Найти первый порт с подключённым устройством. -1 если нет. */
int ehci_find_device_port(void) {
    for (int p = 0; p < g_num_ports; p++) {
        if (ehci_port_connected(p)) return p;
    }
    return -1;
}

void ehci_reset_port(int port) {
    if (port < 0 || port >= g_num_ports) return;

    kprintf("[+] EHCI: reset port %d\n", port);

    uint32_t portsc = ehci_read_op(EHCI_PORTSC(port));
    kprintf("[dbg] PORTSC до reset: %x\n", portsc);

    /* Проверяем, что устройство подключено */
    if (!(portsc & EHCI_PORT_CCS)) {
        kprintf("[!] EHCI: на порту %d нет устройства\n", port);
        return;
    }

    /* PP=1 */
    if (!(portsc & EHCI_PORT_PP)) {
        portsc |= EHCI_PORT_PP;
        ehci_write_op(EHCI_PORTSC(port), portsc);
        for (volatile int i = 0; i < 100000; i++) {}
    }

    /* PR = 1 */
    portsc = ehci_read_op(EHCI_PORTSC(port));
    portsc &= ~(EHCI_PORT_CSC | EHCI_PORT_PEDC);
    portsc |= EHCI_PORT_PR;
    ehci_write_op(EHCI_PORTSC(port), portsc);

    /* 50 мс */
    for (volatile int i = 0; i < 500000; i++) {}

    /* PR = 0 */
    portsc = ehci_read_op(EHCI_PORTSC(port));
    portsc &= ~EHCI_PORT_PR;
    portsc &= ~(EHCI_PORT_CSC | EHCI_PORT_PEDC);
    ehci_write_op(EHCI_PORTSC(port), portsc);

    /* Ждём PR=0 */
    for (int i = 0; i < 20000; i++) {
        portsc = ehci_read_op(EHCI_PORTSC(port));
        if (!(portsc & EHCI_PORT_PR)) break;
    }

    /* Ждём PED=1 */
    for (int i = 0; i < 100000; i++) {
        portsc = ehci_read_op(EHCI_PORTSC(port));
        if (portsc & EHCI_PORT_PED) break;
    }

    kprintf("[dbg] PORTSC после reset: %x%s%s\n", portsc,
            (portsc & EHCI_PORT_CCS) ? " CCS" : "",
            (portsc & EHCI_PORT_PED) ? " PED" : "");

    if (portsc & EHCI_PORT_PED) {
        kprintf("[+] EHCI: порт %d enabled\n", port);
    } else {
        kprintf("[!] EHCI: порт %d НЕ enabled\n", port);
    }
}

/* ============ Control Transfer ============ */

int ehci_control_transfer(uint8_t dev_addr, uint8_t ep,
                          const void *setup_pkt,
                          void *in_data, uint32_t in_len) {
    if (!g_ehci_found) return -1;
    if (in_len > sizeof(g_data_buf)) return -1;

    /* Проверка: есть ли вообще устройство на портах.
     * Для dev_addr=0 — это новое устройство на каком-то порту.
     * Для dev_addr>0 — устройство уже настроено, но должно быть на порту. */
    int dev_port = ehci_find_device_port();
    if (dev_port < 0) {
        kprintf("[!] EHCI: нет устройств на портах\n");
        return -1;
    }

    /* 1. Setup-пакет */
    memcpy(g_setup_buf, setup_pkt, 8);

    /* 2. Data-буфер */
    if (in_data && in_len > 0) {
        memset(g_data_buf, 0, in_len);
    }

    /* 3. QH + 3 qTD */
    ehci_qh_t  *qh  = qh_alloc();
    ehci_qtd_t *q0  = qtd_alloc();
    ehci_qtd_t *q1  = qtd_alloc();
    ehci_qtd_t *q2  = qtd_alloc();

    if (!qh || !q0 || !q1 || !q2) {
        kprintf("[!] EHCI: qh/qtd pool exhausted\n");
        return -1;
    }

    uint32_t setup_phys = virt_to_phys(g_setup_buf);
    uint32_t data_phys  = virt_to_phys(g_data_buf);

    /* 4. QH */
    qh->horiz_link = EHCI_TERMINATE;
    qh->ep_char = (dev_addr & 0x7F)
                | ((ep & 0xF) << 8)
                | (2u << QH_EPS_SHIFT)
                | (64u << QH_MAXPKT_SHIFT);
    qh->ep_caps      = 0;
    qh->current_qtd  = 0;
    qh->next_qtd     = virt_to_phys(q0);
    qh->alt_next_qtd = EHCI_TERMINATE;
    qh->token        = 0;
    memset(qh->buf, 0, sizeof(qh->buf));

    /* 5. SETUP qTD */
    q0->next_qtd     = virt_to_phys(q1);
    q0->alt_next_qtd = EHCI_TERMINATE;
    q0->token = (QTD_PID_SETUP << QTD_PID_SHIFT)
              | (3u << QTD_CERR_SHIFT)
              | (8u << QTD_TOTAL_SHIFT);
    memset(q0->buf, 0, sizeof(q0->buf));
    q0->buf[0] = setup_phys;

    /* 6. DATA qTD */
    q1->next_qtd     = virt_to_phys(q2);
    q1->alt_next_qtd = EHCI_TERMINATE;
    q1->token = (QTD_PID_IN << QTD_PID_SHIFT)
              | (3u << QTD_CERR_SHIFT)
              | (in_len << QTD_TOTAL_SHIFT)
              | QTD_IOC;
    memset(q1->buf, 0, sizeof(q1->buf));
    if (in_len > 0) q1->buf[0] = data_phys;

    /* 7. STATUS qTD */
    q2->next_qtd     = EHCI_TERMINATE;
    q2->alt_next_qtd = EHCI_TERMINATE;
    q2->token = (QTD_PID_OUT << QTD_PID_SHIFT)
              | (3u << QTD_CERR_SHIFT)
              | (0u << QTD_TOTAL_SHIFT)
              | QTD_IOC;
    memset(q2->buf, 0, sizeof(q2->buf));

    /* 8. Добавить QH в async list */
    uint32_t old_next = g_async_head.horiz_link & ~0x1Fu;
    qh->horiz_link = old_next | EHCI_QH_TYPE;
    g_async_head.horiz_link = virt_to_phys(qh) | EHCI_QH_TYPE;

    /* 9. Ждать SETUP */
    int timeout = 1000000;
    while (timeout-- > 0) {
        if (!(q0->token & QTD_ACTIVE)) break;
    }

    if (timeout <= 0 || (q0->token & QTD_ACTIVE)) {
        kprintf("[!] EHCI: timeout SETUP (token=%x)\n", q0->token);
        g_async_head.horiz_link = old_next | EHCI_QH_TYPE;
        return -1;
    }

    /* 10. Ждать DATA */
    timeout = 1000000;
    while (timeout-- > 0) {
        if (!(q1->token & QTD_ACTIVE)) break;
    }

    if (timeout <= 0 || (q1->token & QTD_ACTIVE)) {
        kprintf("[!] EHCI: timeout DATA (token=%x)\n", q1->token);
        g_async_head.horiz_link = old_next | EHCI_QH_TYPE;
        return -1;
    }

    /* 11. Отключить QH */
    g_async_head.horiz_link = old_next | EHCI_QH_TYPE;

    /* 12. Проверка статусов (маска без ACTIVE) */
    uint32_t s0 = q0->token & 0x7F;
    uint32_t s1 = q1->token & 0x7F;
    uint32_t s2 = q2->token & 0x7F;

    if (s0 != 0 || s1 != 0) {
        kprintf("[!] EHCI: transfer error: setup=%x data=%x status=%x\n",
                s0, s1, s2);
        kprintf("[dbg] q0.token=%x q1.token=%x q2.token=%x\n",
                q0->token, q1->token, q2->token);
        return -1;
    }

    /* 13. Скопировать данные */
    if (in_data && in_len > 0) {
        memcpy(in_data, g_data_buf, in_len);
    }

    (void)setup_phys;
    (void)data_phys;
    return 0;
}

/* ============ GET_DESCRIPTOR ============ */

int ehci_get_device_descriptor(uint8_t dev_addr,
                               usb_device_descriptor_t *out) {
    uint8_t setup[8];
    setup[0] = 0x80;   /* IN, standard, device */
    setup[1] = 0x06;   /* GET_DESCRIPTOR */
    setup[2] = 0x00;   /* index 0 */
    setup[3] = 0x01;   /* DEVICE */
    setup[4] = 0x00;
    setup[5] = 0x00;
    setup[6] = 0x12;   /* 18 */
    setup[7] = 0x00;

    return ehci_control_transfer(dev_addr, 0, setup, out, 18);
}

/* ============ Инициализация ============ */

void ehci_init(void) {
    g_ehci_found = 0;

    if (pci_find_ehci(&g_ehci_dev) <= 0) {
        kprintf("[+] EHCI: не найден\n");
        return;
    }

    g_ehci_found = 1;

    kprintf("[+] EHCI: найден %u:%u.%u (%x:%x)\n",
            (uint32_t)g_ehci_dev.bus, (uint32_t)g_ehci_dev.device,
            (uint32_t)g_ehci_dev.function,
            g_ehci_dev.vendor_id, g_ehci_dev.device_id);

    uint16_t cmd = pci_read16(g_ehci_dev.bus, g_ehci_dev.device,
                              g_ehci_dev.function, PCI_COMMAND);
    cmd |= 0x0002 | 0x0004;
    pci_write32(g_ehci_dev.bus, g_ehci_dev.device, g_ehci_dev.function,
                PCI_COMMAND, (uint32_t)cmd);

    uint32_t bar0 = pci_read32(g_ehci_dev.bus, g_ehci_dev.device,
                               g_ehci_dev.function, PCI_BAR0);
    if (bar0 == 0 || bar0 == 0xFFFFFFFF || (bar0 & 1)) {
        kprintf("[!] EHCI: BAR0 невалиден: %x\n", bar0);
        g_ehci_found = 0;
        return;
    }

    uint64_t mmio_phys = (uint64_t)(bar0 & ~0xF);
    uint32_t bar_size = pci_bar_size(g_ehci_dev.bus, g_ehci_dev.device,
                                     g_ehci_dev.function, 0);
    if (bar_size == 0) bar_size = 4096;

    uint64_t pages = (bar_size + 4095) / 4096;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = (mmio_phys & ~0xFFFULL) + i * 4096;
        if (vmm_map(g_kernel_as, phys, phys, VMM_MMIO) != 0) {
            kprintf("[!] EHCI: vmm_map failed for %x\n", phys);
            g_ehci_found = 0;
            return;
        }
    }

    g_ehci_mmio = (volatile uint8_t *)mmio_phys;
    kprintf("[+] EHCI: MMIO at %x\n", mmio_phys);

    g_caplength = *(volatile uint8_t *)(g_ehci_mmio + EHCI_CAPLENGTH);
    uint16_t hciversion = *(volatile uint16_t *)(g_ehci_mmio + EHCI_HCIVERSION);
    g_hcsparams = mmio_read32(EHCI_HCSPARAMS);
    g_hccparams = mmio_read32(EHCI_HCCPARAMS);
    g_num_ports = g_hcsparams & 0xF;

    kprintf("[+] EHCI: CAPLENGTH=%u HCIVERSION=%x HCSPARAMS=%x HCCPARAMS=%x\n",
            (uint32_t)g_caplength, (uint32_t)hciversion,
            g_hcsparams, g_hccparams);
    kprintf("[+] EHCI: портов = %d\n", g_num_ports);

    g_qh_used  = 0;
    g_qtd_used = 0;
}

/* ============ Отладка ============ */

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
    kprintf("  HCSPARAMS:  %x  (портов: %d)\n", g_hcsparams, g_num_ports);
    kprintf("  HCCPARAMS:  %x\n", g_hccparams);

    kprintf("\n  Operational registers:\n");
    kprintf("    USBCMD:  %x\n", ehci_read_op(EHCI_USBCMD));
    kprintf("    USBSTS:  %x\n", ehci_read_op(EHCI_USBSTS));
    kprintf("    USBINTR: %x\n", ehci_read_op(EHCI_USBINTR));
    kprintf("    FRINDEX: %x\n", ehci_read_op(EHCI_FRINDEX));
    kprintf("    ASYNCLISTADDR: %x\n", ehci_read_op(EHCI_ASYNCLISTADDR));
    kprintf("    CONFIG:  %x\n", ehci_read_op(EHCI_CONFIGFLAG));

    kprintf("\n  Ports:\n");
    for (int i = 0; i < g_num_ports; i++) {
        uint32_t portsc = ehci_read_op(EHCI_PORTSC(i));
        kprintf("    Port %d: PORTSC = %x  %s%s\n",
                i, portsc,
                (portsc & EHCI_PORT_CCS) ? "CONNECTED " : "",
                (portsc & EHCI_PORT_PED) ? "ENABLED"   : "");
    }
}

void ehci_dump_ports(void) {
    if (!g_ehci_found) return;

    kprintf("EHCI ports (n=%d):\n", g_num_ports);
    for (int i = 0; i < g_num_ports; i++) {
        uint32_t portsc = ehci_read_op(EHCI_PORTSC(i));
        kprintf("  Port %d: %x  %s%s%s%s\n",
                i, portsc,
                (portsc & EHCI_PORT_CCS) ? "CCS " : "",
                (portsc & EHCI_PORT_PED) ? "PED " : "",
                (portsc & EHCI_PORT_CSC) ? "CSC " : "",
                (portsc & EHCI_PORT_PEDC) ? "PEDC" : "");
    }
}