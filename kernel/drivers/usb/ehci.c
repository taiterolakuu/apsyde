#include "ehci.h"
#include "../../bus/pci.h"
#include "../../mm/vmm.h"
#include "../../lib/kprintf.h"
#include "../../lib/panic.h"
#include "../../lib/string.h"

/*
 * Aether EHCI driver
 *
 * Scope:
 *   - PCI EHCI discovery
 *   - MMIO initialization
 *   - Async schedule with one QH
 *   - Port detection/reset
 *   - Polled USB control IN transfers
 *   - GET_DESCRIPTOR(Device)
 *
 * This is intentionally a small, deterministic EHCI driver.
 * It does not yet implement interrupt/isochronous transfers,
 * companion-controller routing, or a general USB device manager.
 */

#define EHCI_QTD_ACTIVE       (1u << 7)
#define EHCI_QTD_HALTED       (1u << 6)
#define EHCI_QTD_DBE          (1u << 5)
#define EHCI_QTD_BABBLE       (1u << 4)
#define EHCI_QTD_XACTERR      (1u << 3)
#define EHCI_QTD_MMF          (1u << 2)
#define EHCI_QTD_STS_MASK     0x7Fu

#define EHCI_QTD_CERR_SHIFT   10
#define EHCI_QTD_PID_SHIFT    8

/*
 * Data toggle for the NEXT qTD lives in next_qtd[31].
 * It is NOT a field in token[].
 */
#define EHCI_QTD_TOGGLE       (1u << 31)

#define EHCI_QH_DTC           (1u << 14)

#define EHCI_STS_HCH          (1u << 12)
#define EHCI_STS_ASS          (1u << 15)
#define EHCI_STS_HSE          (1u << 4)

#define EHCI_CMD_RS           (1u << 0)
#define EHCI_CMD_HCRESET      (1u << 1)
#define EHCI_CMD_ASE          (1u << 5)

#define EHCI_PORT_PR          (1u << 8)
#define EHCI_PORT_PP          (1u << 12)

#define EHCI_CONFIGFLAG_CF    (1u << 0)

#define EHCI_MAX_QH           8
#define EHCI_MAX_QTD          32

#define EHCI_XFER_TIMEOUT     2000000u
#define EHCI_RESET_TIMEOUT    200000u
#define EHCI_STOP_TIMEOUT     200000u

#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_DT_DEVICE          0x01

#define USB_DIR_IN              0x80
#define USB_TYPE_STANDARD       0x00
#define USB_RECIP_DEVICE        0x00

/* ============ State ============ */

static pci_device_t g_ehci_dev;
static int g_ehci_found = 0;

static volatile uint8_t *g_ehci_mmio = 0;
static uint8_t g_caplength = 0;
static uint32_t g_hcsparams = 0;
static uint32_t g_hccparams = 0;
static int g_num_ports = 0;

/*
 * The controller is configured for 32-bit DMA.
 * Therefore these structures must remain below 4 GiB.
 */
static ehci_qh_t  g_qh_pool[EHCI_MAX_QH]
    __attribute__((aligned(32)));

static ehci_qtd_t g_qtd_pool[EHCI_MAX_QTD]
    __attribute__((aligned(32)));

static int g_qh_used = 0;
static int g_qtd_used = 0;

static uint8_t g_setup_buf[8]
    __attribute__((aligned(64)));

static uint8_t g_data_buf[256]
    __attribute__((aligned(64)));

static ehci_qh_t g_async_head
    __attribute__((aligned(32)));

/* ============ CPU/DMA ordering ============ */

static inline void ehci_wmb(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void ehci_rmb(void)
{
    __asm__ volatile("" ::: "memory");
}

/* ============ MMIO ============ */

static inline uint32_t mmio_read32(uint32_t off)
{
    return *(volatile uint32_t *)(g_ehci_mmio + off);
}

static inline void mmio_write32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(g_ehci_mmio + off) = val;
}

uint32_t ehci_read_cap(uint32_t off)
{
    if (!g_ehci_mmio)
        return 0;

    return mmio_read32(off);
}

uint32_t ehci_read_op(uint32_t off)
{
    if (!g_ehci_mmio)
        return 0;

    return mmio_read32(g_caplength + off);
}

void ehci_write_op(uint32_t off, uint32_t val)
{
    if (!g_ehci_mmio)
        return;

    mmio_write32(g_caplength + off, val);
}

/* ============ Pool ============ */

static void ehci_pool_reset(void)
{
    g_qh_used = 0;
    g_qtd_used = 0;

    memset(g_qh_pool, 0, sizeof(g_qh_pool));
    memset(g_qtd_pool, 0, sizeof(g_qtd_pool));
}

static ehci_qh_t *qh_alloc(void)
{
    if (g_qh_used >= EHCI_MAX_QH)
        return 0;

    ehci_qh_t *qh = &g_qh_pool[g_qh_used++];

    memset(qh, 0, sizeof(*qh));

    return qh;
}

static ehci_qtd_t *qtd_alloc(void)
{
    if (g_qtd_used >= EHCI_MAX_QTD)
        return 0;

    ehci_qtd_t *qtd = &g_qtd_pool[g_qtd_used++];

    memset(qtd, 0, sizeof(*qtd));

    return qtd;
}

/* ============ Polling helpers ============ */

static int ehci_wait_halted(int want_halted)
{
    for (uint32_t i = 0; i < EHCI_STOP_TIMEOUT; ++i)
    {
        uint32_t sts = ehci_read_op(EHCI_USBSTS);
        int halted = (sts & EHCI_STS_HCH) != 0;

        if (halted == want_halted)
            return 0;
    }

    return -1;
}

static int ehci_wait_reset_clear(void)
{
    for (uint32_t i = 0; i < EHCI_RESET_TIMEOUT; ++i)
    {
        if (!(ehci_read_op(EHCI_USBCMD) & EHCI_CMD_HCRESET))
            return 0;
    }

    return -1;
}

static int ehci_wait_async(int enabled)
{
    for (uint32_t i = 0; i < EHCI_STOP_TIMEOUT; ++i)
    {
        uint32_t sts = ehci_read_op(EHCI_USBSTS);
        int active = (sts & EHCI_STS_ASS) != 0;

        if (active == enabled)
            return 0;
    }

    return -1;
}

/* ============ Async schedule control ============ */

static int ehci_async_stop(void)
{
    uint32_t cmd = ehci_read_op(EHCI_USBCMD);

    if (!(cmd & EHCI_CMD_ASE))
        return 0;

    cmd &= ~EHCI_CMD_ASE;
    ehci_write_op(EHCI_USBCMD, cmd);

    return ehci_wait_async(0);
}

static int ehci_async_start(void)
{
    uint32_t cmd = ehci_read_op(EHCI_USBCMD);

    cmd |= EHCI_CMD_ASE | EHCI_CMD_RS;

    ehci_write_op(EHCI_USBCMD, cmd);

    if (ehci_wait_halted(0) != 0)
        return -1;

    return ehci_wait_async(1);
}

/* ============ Controller initialization ============ */

void ehci_init_controller(void)
{
    if (!g_ehci_found)
        return;

    kprintf("[+] EHCI: controller initialization\n");

    uint32_t cmd = ehci_read_op(EHCI_USBCMD);

    cmd &= ~(EHCI_CMD_RS | EHCI_CMD_ASE);
    ehci_write_op(EHCI_USBCMD, cmd);

    if (ehci_wait_halted(1) != 0)
    {
        kprintf("[!] EHCI: controller did not halt\n");
        return;
    }

    cmd = ehci_read_op(EHCI_USBCMD);
    cmd |= EHCI_CMD_HCRESET;
    ehci_write_op(EHCI_USBCMD, cmd);

    if (ehci_wait_reset_clear() != 0)
    {
        kprintf("[!] EHCI: HCRESET timeout\n");
        return;
    }

    kprintf("[+] EHCI: HCRESET complete\n");

    ehci_write_op(EHCI_CTRLDSSEGMENT, 0);

    ehci_pool_reset();

    memset(&g_async_head, 0, sizeof(g_async_head));

    uint32_t head_phys = virt_to_phys(&g_async_head);

    /*
     * EHCI async list anchor.
     * H = 1, horizontal link points back to itself.
     */
    g_async_head.horiz_link   = head_phys | EHCI_QH_TYPE;
    g_async_head.ep_char      = QH_H;
    g_async_head.ep_caps      = 0;
    g_async_head.current_qtd  = EHCI_TERMINATE;
    g_async_head.next_qtd     = EHCI_TERMINATE;
    g_async_head.alt_next_qtd = EHCI_TERMINATE;
    g_async_head.token        = 0;

    memset(g_async_head.buf, 0, sizeof(g_async_head.buf));

    ehci_wmb();

    ehci_write_op(EHCI_ASYNCLISTADDR, head_phys);
    ehci_write_op(EHCI_PERIODICLIST, 0);
    ehci_write_op(EHCI_USBINTR, 0);

    /*
     * Route ports to EHCI.
     */
    uint32_t cfg = ehci_read_op(EHCI_CONFIGFLAG);

    ehci_write_op(EHCI_CONFIGFLAG, cfg | EHCI_CONFIGFLAG_CF);

    uint32_t start_cmd = EHCI_CMD_RS;

    ehci_write_op(EHCI_USBCMD, start_cmd);

    if (ehci_wait_halted(0) != 0)
    {
        kprintf("[!] EHCI: controller failed to start\n");
        return;
    }

    if (ehci_async_start() != 0)
    {
        kprintf("[!] EHCI: async schedule failed to start\n");
        return;
    }

    kprintf(
        "[+] EHCI: running USBCMD=%x USBSTS=%x ASYNCLIST=%x\n",
        ehci_read_op(EHCI_USBCMD),
        ehci_read_op(EHCI_USBSTS),
        ehci_read_op(EHCI_ASYNCLISTADDR)
    );

    if (!(ehci_read_op(EHCI_CONFIGFLAG) & EHCI_CONFIGFLAG_CF))
    {
        kprintf("[!] EHCI: CONFIGFLAG.CF is not set\n");
    }
}

/* ============ Ports ============ */

int ehci_port_connected(int port)
{
    if (!g_ehci_found)
        return 0;

    if (port < 0 || port >= g_num_ports)
        return 0;

    return (ehci_read_op(EHCI_PORTSC(port)) & EHCI_PORT_CCS) != 0;
}

int ehci_find_device_port(void)
{
    for (int p = 0; p < g_num_ports; ++p)
    {
        uint32_t portsc = ehci_read_op(EHCI_PORTSC(p));

        if (portsc & EHCI_PORT_CCS)
            return p;
    }

    return -1;
}

static void ehci_clear_port_changes(int port, uint32_t portsc)
{
    uint32_t clear = 0;

    if (portsc & EHCI_PORT_CSC)
        clear |= EHCI_PORT_CSC;

    if (portsc & EHCI_PORT_PEDC)
        clear |= EHCI_PORT_PEDC;

    if (clear)
        ehci_write_op(EHCI_PORTSC(port), clear);
}

int ehci_reset_port_checked(int port)
{
    if (!g_ehci_found)
        return -1;

    if (port < 0 || port >= g_num_ports)
        return -1;

    uint32_t portsc = ehci_read_op(EHCI_PORTSC(port));

    kprintf("[dbg] EHCI: port %d before reset PORTSC=%x\n",
            port, portsc);

    if (!(portsc & EHCI_PORT_CCS))
    {
        kprintf("[!] EHCI: port %d is disconnected\n", port);
        return -1;
    }

    if (!(portsc & EHCI_PORT_PP))
    {
        uint32_t reset_value =
            portsc & ~(EHCI_PORT_CSC | EHCI_PORT_PEDC);

        reset_value |= EHCI_PORT_PP;

        ehci_write_op(EHCI_PORTSC(port), reset_value);
    }

    portsc = ehci_read_op(EHCI_PORTSC(port));

    if (!(portsc & EHCI_PORT_CCS))
    {
        kprintf("[!] EHCI: device disappeared before reset\n");
        return -1;
    }

    ehci_clear_port_changes(port, portsc);

    portsc = ehci_read_op(EHCI_PORTSC(port));

    portsc &= ~(EHCI_PORT_CSC | EHCI_PORT_PEDC);
    portsc |= EHCI_PORT_PR;

    ehci_write_op(EHCI_PORTSC(port), portsc);

    int reset_seen = 0;

    for (uint32_t i = 0; i < EHCI_RESET_TIMEOUT; ++i)
    {
        uint32_t ps = ehci_read_op(EHCI_PORTSC(port));

        if (ps & EHCI_PORT_PR)
        {
            reset_seen = 1;
            break;
        }
    }

    if (!reset_seen)
    {
        kprintf("[!] EHCI: port %d reset never asserted\n", port);
        return -1;
    }

    /*
     * Hold reset for at least 50 ms.
     * Without a timer service, use a bounded pause loop.
     */
    for (volatile uint32_t i = 0; i < 500000; ++i)
    {
        __asm__ volatile("pause");
    }

    portsc = ehci_read_op(EHCI_PORTSC(port));

    portsc &= ~EHCI_PORT_PR;
    portsc &= ~(EHCI_PORT_CSC | EHCI_PORT_PEDC);

    ehci_write_op(EHCI_PORTSC(port), portsc);

    for (uint32_t i = 0; i < EHCI_RESET_TIMEOUT; ++i)
    {
        portsc = ehci_read_op(EHCI_PORTSC(port));

        if (!(portsc & EHCI_PORT_PR))
            break;
    }

    if (portsc & EHCI_PORT_PR)
    {
        kprintf("[!] EHCI: port %d reset did not finish\n", port);
        return -1;
    }

    for (uint32_t i = 0; i < EHCI_RESET_TIMEOUT; ++i)
    {
        portsc = ehci_read_op(EHCI_PORTSC(port));

        if (portsc & EHCI_PORT_PED)
            break;
    }

    kprintf("[dbg] EHCI: port %d after reset PORTSC=%x%s%s\n",
            port, portsc,
            (portsc & EHCI_PORT_CCS) ? " CCS" : "",
            (portsc & EHCI_PORT_PED) ? " PED" : "");

    if (!(portsc & EHCI_PORT_CCS))
    {
        kprintf("[!] EHCI: device disconnected during reset\n");
        return -1;
    }

    if (!(portsc & EHCI_PORT_PED))
    {
        kprintf("[!] EHCI: port %d is not enabled; "
                "device is not High-Speed\n", port);
        return -2;
    }

    kprintf("[+] EHCI: port %d enabled, High-Speed path ready\n", port);

    return 0;
}

void ehci_reset_port(int port)
{
    (void)ehci_reset_port_checked(port);
}

/* ============ qTD helpers ============ */

static uint32_t qtd_status(uint32_t token)
{
    return token & EHCI_QTD_STS_MASK;
}

static const char *qtd_error_name(uint32_t token)
{
    if (token & EHCI_QTD_HALTED)  return "HALTED";
    if (token & EHCI_QTD_BABBLE)  return "BABBLE";
    if (token & EHCI_QTD_XACTERR) return "XACTERR";
    if (token & EHCI_QTD_DBE)     return "DBE";
    if (token & EHCI_QTD_MMF)     return "MMF";
    if (token & EHCI_QTD_ACTIVE)  return "ACTIVE";
    return "OK";
}

/*
 * Build a qTD token.
 * NOTE: toggle is NOT part of token. See qtd_prepare().
 */
static uint32_t qtd_make_token(
    uint32_t pid,
    uint32_t bytes,
    int ioc
)
{
    uint32_t token = 0;

    token |= (pid << QTD_PID_SHIFT);
    token |= (3u << EHCI_QTD_CERR_SHIFT);
    token |= ((bytes & 0x7FFFu) << QTD_TOTAL_SHIFT);

    if (ioc)
        token |= QTD_IOC;

    /* ACTIVE must be set last. */
    token |= EHCI_QTD_ACTIVE;

    return token;
}

/*
 * Prepare a qTD.
 * The data-toggle bit belongs to next_qtd[31], not to token.
 */
static void qtd_prepare(
    ehci_qtd_t *qtd,
    uint32_t next_phys,
    uint32_t token,
    int toggle_next
)
{
    memset(qtd, 0, sizeof(*qtd));

    qtd->next_qtd = next_phys;

    if (toggle_next)
        qtd->next_qtd |= EHCI_QTD_TOGGLE;

    qtd->alt_next_qtd = EHCI_TERMINATE;

    qtd->token = token;
}

static void qtd_set_buffer(ehci_qtd_t *qtd, uint32_t phys)
{
    memset(qtd->buf, 0, sizeof(qtd->buf));
    qtd->buf[0] = phys;
}

/* ============ Control transfer ============ */

int ehci_control_transfer(
    uint8_t dev_addr,
    uint8_t ep,
    const void *setup_pkt,
    void *in_data,
    uint32_t in_len
)
{
    if (!g_ehci_found)
        return -1;

    if (!setup_pkt)
        return -1;

    if (in_len > sizeof(g_data_buf))
        return -1;

    if (dev_addr > 127)
        return -1;

    if (ep > 15)
        return -1;

    if (in_len > 0 && !in_data)
        return -1;

    int port = ehci_find_device_port();

    if (port < 0)
    {
        kprintf("[!] EHCI: no connected USB device\n");
        return -1;
    }

    uint32_t portsc = ehci_read_op(EHCI_PORTSC(port));

    if (!(portsc & EHCI_PORT_PED))
    {
        kprintf("[!] EHCI: port %d is not enabled\n", port);
        return -1;
    }

    if (ep != 0)
        return -1;

    const uint32_t max_packet = 8;

    memcpy(g_setup_buf, setup_pkt, 8);

    if (in_len)
        memset(g_data_buf, 0, in_len);

    ehci_qh_t  *qh = qh_alloc();
    ehci_qtd_t *q0 = qtd_alloc();
    ehci_qtd_t *q1 = qtd_alloc();
    ehci_qtd_t *q2 = qtd_alloc();

    if (!qh || !q0 || !q1 || !q2)
    {
        kprintf("[!] EHCI: descriptor pool exhausted\n");
        return -1;
    }

    /*
     * virt_to_phys returns uint64_t. We must check the 64-bit
     * value before truncating to 32-bit for the DMA structures.
     */
    uint64_t qh_phys64    = virt_to_phys(qh);
    uint64_t q0_phys64    = virt_to_phys(q0);
    uint64_t q1_phys64    = virt_to_phys(q1);
    uint64_t q2_phys64    = virt_to_phys(q2);
    uint64_t setup_phys64 = virt_to_phys(g_setup_buf);
    uint64_t data_phys64  = virt_to_phys(g_data_buf);

    if (qh_phys64    > 0xFFFFFFFFULL ||
        q0_phys64    > 0xFFFFFFFFULL ||
        q1_phys64    > 0xFFFFFFFFULL ||
        q2_phys64    > 0xFFFFFFFFULL ||
        setup_phys64 > 0xFFFFFFFFULL ||
        data_phys64  > 0xFFFFFFFFULL)
    {
        kprintf("[!] EHCI: DMA address above 4 GiB\n");
        return -1;
    }

    uint32_t qh_phys    = (uint32_t)qh_phys64;
    uint32_t q0_phys    = (uint32_t)q0_phys64;
    uint32_t q1_phys    = (uint32_t)q1_phys64;
    uint32_t q2_phys    = (uint32_t)q2_phys64;
    uint32_t setup_phys = (uint32_t)setup_phys64;
    uint32_t data_phys  = (uint32_t)data_phys64;

    /*
     * QH: control endpoint 0, High-Speed, max packet 64, DTC=1.
     */
    qh->horiz_link = EHCI_TERMINATE;

    qh->ep_char =
        (dev_addr & 0x7Fu) |
        ((uint32_t)(ep & 0xFu) << 8) |
        (2u << QH_EPS_SHIFT) |
        (max_packet << QH_MAXPKT_SHIFT) |
        EHCI_QH_DTC;

    qh->ep_caps      = 0;
    qh->current_qtd  = EHCI_TERMINATE;
    qh->next_qtd     = q0_phys;
    qh->alt_next_qtd = EHCI_TERMINATE;
    qh->token        = 0;

    memset(qh->buf, 0, sizeof(qh->buf));

    /*
     * SETUP → DATA: following toggle = 1
     * DATA IN → STATUS: following toggle = 1
     * STATUS: terminator
     */
    qtd_prepare(q0, q1_phys,
                qtd_make_token(QTD_PID_SETUP, 8, 0),
                1);
    qtd_set_buffer(q0, setup_phys);

    qtd_prepare(q1, q2_phys,
                qtd_make_token(QTD_PID_IN, in_len, 0),
                1);
    if (in_len)
        qtd_set_buffer(q1, data_phys);

    qtd_prepare(q2, EHCI_TERMINATE,
                qtd_make_token(QTD_PID_OUT, 0, 1),
                0);

    ehci_wmb();

    kprintf("[dbg] CTRL dev=%u ep=%u len=%u\n",
            (uint32_t)dev_addr, (uint32_t)ep, in_len);

    kprintf("[dbg] QH  phys=%x ep_char=%x (DTC=%d) next=%x\n",
            qh_phys, qh->ep_char,
            (qh->ep_char & EHCI_QH_DTC) ? 1 : 0,
            qh->next_qtd);

    kprintf("[dbg] q0  phys=%x token=%x buf=%x next=%x\n",
            q0_phys, q0->token, q0->buf[0], q0->next_qtd);

    kprintf("[dbg] q1  phys=%x token=%x buf=%x next=%x\n",
            q1_phys, q1->token, q1->buf[0], q1->next_qtd);

    kprintf("[dbg] q2  phys=%x token=%x next=%x\n",
            q2_phys, q2->token, q2->next_qtd);

    if (ehci_async_stop() != 0)
    {
        kprintf("[!] EHCI: failed to stop async schedule\n");
        return -1;
    }

    uint32_t old_next = g_async_head.horiz_link;

    qh->horiz_link = old_next;

    ehci_wmb();

    g_async_head.horiz_link = qh_phys | EHCI_QH_TYPE;

    ehci_wmb();

    if (ehci_async_start() != 0)
    {
        kprintf("[!] EHCI: failed to restart async schedule\n");

        ehci_async_stop();
        g_async_head.horiz_link = old_next;
        ehci_wmb();
        ehci_async_start();
        return -1;
    }

    for (volatile uint32_t i = 0; i < 100000; ++i)
    {
        __asm__ volatile("pause");
    }

    kprintf("[dbg] после старта: q0->token=%x ACTIVE=%d "
            "qh->current_qtd=%x (q0=%x) qh->token=%x\n",
            q0->token,
            (q0->token & EHCI_QTD_ACTIVE) ? 1 : 0,
            qh->current_qtd, q0_phys, qh->token);

    uint32_t timeout = EHCI_XFER_TIMEOUT;

    while (timeout--)
    {
        ehci_rmb();

        uint32_t t0 = q0->token;
        uint32_t t1 = q1->token;
        uint32_t t2 = q2->token;

        if (!(t0 & EHCI_QTD_ACTIVE) &&
            !(t1 & EHCI_QTD_ACTIVE) &&
            !(t2 & EHCI_QTD_ACTIVE))
        {
            break;
        }

        if (ehci_read_op(EHCI_USBSTS) & EHCI_STS_HSE)
        {
            kprintf("[!] EHCI: Host System Error during transfer\n");
            break;
        }

        __asm__ volatile("pause");
    }

    int schedule_ok = ehci_async_stop() == 0;

    if (!schedule_ok)
    {
        kprintf("[!] EHCI: failed to stop async schedule after transfer\n");
        return -1;
    }

    ehci_rmb();

    uint32_t t0 = q0->token;
    uint32_t t1 = q1->token;
    uint32_t t2 = q2->token;

    kprintf("[dbg] qTD final: setup=%x data=%x status=%x\n", t0, t1, t2);

    if ((t0 | t1 | t2) & EHCI_QTD_ACTIVE)
    {
        kprintf("[!] EHCI: transfer timeout "
                "setup=%s data=%s status=%s\n",
                qtd_error_name(t0),
                qtd_error_name(t1),
                qtd_error_name(t2));

        g_async_head.horiz_link = old_next;
        ehci_wmb();

        if (ehci_async_start() != 0)
            kprintf("[!] EHCI: async schedule restart failed\n");

        return -1;
    }

    uint32_t s0 = qtd_status(t0);
    uint32_t s1 = qtd_status(t1);
    uint32_t s2 = qtd_status(t2);

    if (s0 || s1 || s2)
    {
        kprintf("[!] EHCI: transfer error "
                "setup=%s data=%s status=%s\n",
                qtd_error_name(t0),
                qtd_error_name(t1),
                qtd_error_name(t2));

        g_async_head.horiz_link = old_next;
        ehci_wmb();

        if (ehci_async_start() != 0)
            kprintf("[!] EHCI: async schedule restart failed\n");

        return -1;
    }

    g_async_head.horiz_link = old_next;
    ehci_wmb();

    if (ehci_async_start() != 0)
    {
        kprintf("[!] EHCI: async schedule restart failed\n");
        return -1;
    }

    uint32_t remaining =
        (t1 >> QTD_TOTAL_SHIFT) & 0x7FFFu;

    uint32_t actual =
        (in_len >= remaining)
            ? (in_len - remaining)
            : 0;

    if (in_data && in_len)
    {
        memcpy(in_data, g_data_buf, in_len);
    }

    kprintf("[+] EHCI: control transfer complete, actual=%u\n", actual);

    ehci_pool_reset();

    return 0;
}

/* ============ GET DEVICE DESCRIPTOR ============ */

int ehci_get_device_descriptor(
    uint8_t dev_addr,
    usb_device_descriptor_t *out
)
{
    if (!out)
        return -1;

    uint8_t setup[8];

    setup[0] = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    setup[1] = USB_REQ_GET_DESCRIPTOR;
    setup[2] = 0x00;
    setup[3] = USB_DT_DEVICE;
    setup[4] = 0x00;
    setup[5] = 0x00;
    setup[6] = 18;
    setup[7] = 0;

    int rc = ehci_control_transfer(dev_addr, 0, setup, out, 18);

    if (rc != 0)
        return rc;

    const uint8_t *raw = (const uint8_t *)out;

    if (raw[0] != 18 || raw[1] != USB_DT_DEVICE)
    {
        kprintf("[!] EHCI: invalid Device Descriptor "
                "len=%u type=%u\n",
                (uint32_t)raw[0], (uint32_t)raw[1]);
        return -1;
    }

    kprintf("[+] USB Device Descriptor:\n"
            "    bcdUSB=0x%x VID=0x%x PID=0x%x "
            "bMaxPacketSize0=%u bNumConfigurations=%u\n",
            (uint32_t)out->bcdUSB,
            (uint32_t)out->idVendor,
            (uint32_t)out->idProduct,
            (uint32_t)out->bMaxPacketSize0,
            (uint32_t)out->bNumConfigurations);

    return 0;
}

/* ============ Controller discovery / initialization ============ */

void ehci_init(void)
{
    g_ehci_found = 0;
    g_ehci_mmio = 0;
    g_caplength = 0;
    g_hcsparams = 0;
    g_hccparams = 0;
    g_num_ports = 0;

    if (pci_find_ehci(&g_ehci_dev) <= 0)
    {
        kprintf("[+] EHCI: controller not found\n");
        return;
    }

    g_ehci_found = 1;

    kprintf("[+] EHCI: found %u:%u.%u (%x:%x)\n",
            (uint32_t)g_ehci_dev.bus,
            (uint32_t)g_ehci_dev.device,
            (uint32_t)g_ehci_dev.function,
            g_ehci_dev.vendor_id,
            g_ehci_dev.device_id);

    uint16_t cmd = pci_read16(
        g_ehci_dev.bus,
        g_ehci_dev.device,
        g_ehci_dev.function,
        PCI_COMMAND);

    cmd |= 0x0002;
    cmd |= 0x0004;

    pci_write32(
        g_ehci_dev.bus,
        g_ehci_dev.device,
        g_ehci_dev.function,
        PCI_COMMAND,
        (uint32_t)cmd);

    uint32_t bar0 = pci_read32(
        g_ehci_dev.bus,
        g_ehci_dev.device,
        g_ehci_dev.function,
        PCI_BAR0);

    if (bar0 == 0 || bar0 == 0xFFFFFFFF || (bar0 & 1))
    {
        kprintf("[!] EHCI: invalid BAR0=%x\n", bar0);
        g_ehci_found = 0;
        return;
    }

    uint64_t mmio_phys = (uint64_t)(bar0 & ~0xFULL);

    uint32_t bar_size = pci_bar_size(
        g_ehci_dev.bus,
        g_ehci_dev.device,
        g_ehci_dev.function,
        0);

    if (!bar_size)
        bar_size = 4096;

    uint64_t pages = ((uint64_t)bar_size + 4095ULL) / 4096ULL;

    for (uint64_t i = 0; i < pages; ++i)
    {
        uint64_t phys = (mmio_phys & ~0xFFFULL) + i * 4096ULL;

        if (vmm_map(g_kernel_as, phys, phys, VMM_MMIO) != 0)
        {
            kprintf("[!] EHCI: vmm_map failed for %x\n", (uint32_t)phys);
            g_ehci_found = 0;
            g_ehci_mmio = 0;
            return;
        }
    }

    g_ehci_mmio = (volatile uint8_t *)mmio_phys;

    kprintf("[+] EHCI: MMIO=%x size=%x\n",
            (uint32_t)mmio_phys, bar_size);

    g_caplength =
        *(volatile uint8_t *)(g_ehci_mmio + EHCI_CAPLENGTH);

    uint16_t hciversion =
        *(volatile uint16_t *)(g_ehci_mmio + EHCI_HCIVERSION);

    g_hcsparams = mmio_read32(EHCI_HCSPARAMS);
    g_hccparams = mmio_read32(EHCI_HCCPARAMS);
    g_num_ports = (int)(g_hcsparams & 0x0F);

    if (g_caplength < 0x20)
    {
        kprintf("[!] EHCI: suspicious CAPLENGTH=%u\n",
                (uint32_t)g_caplength);
    }

    kprintf("[+] EHCI: CAPLENGTH=%u HCIVERSION=%x "
            "HCSPARAMS=%x HCCPARAMS=%x ports=%d\n",
            (uint32_t)g_caplength,
            (uint32_t)hciversion,
            g_hcsparams,
            g_hccparams,
            g_num_ports);

    if (g_hccparams & 0x1u)
    {
        kprintf("[dbg] EHCI: 64-bit DMA capability present; "
                "driver uses low-4GiB DMA\n");
    }

    ehci_init_controller();

    if (!g_ehci_found)
        return;

    kprintf("[+] EHCI: initialization complete\n");
}

/* ============ Status / debug ============ */

int ehci_present(void)
{
    return g_ehci_found;
}

void ehci_dump(void)
{
    if (!g_ehci_found)
    {
        kprintf("EHCI: not present\n");
        return;
    }

    kprintf("EHCI Controller:\n"
            "  PCI:       %u:%u.%u %x:%x\n"
            "  MMIO:      %p\n"
            "  CAPLENGTH: %u\n"
            "  HCSPARAMS: %x\n"
            "  HCCPARAMS: %x\n"
            "  Ports:     %d\n",
            (uint32_t)g_ehci_dev.bus,
            (uint32_t)g_ehci_dev.device,
            (uint32_t)g_ehci_dev.function,
            g_ehci_dev.vendor_id,
            g_ehci_dev.device_id,
            (void *)g_ehci_mmio,
            (uint32_t)g_caplength,
            g_hcsparams,
            g_hccparams,
            g_num_ports);

    kprintf("\nOperational:\n"
            "  USBCMD:       %x\n"
            "  USBSTS:       %x\n"
            "  USBINTR:      %x\n"
            "  FRINDEX:      %x\n"
            "  ASYNCLIST:    %x\n"
            "  CONFIGFLAG:   %x\n",
            ehci_read_op(EHCI_USBCMD),
            ehci_read_op(EHCI_USBSTS),
            ehci_read_op(EHCI_USBINTR),
            ehci_read_op(EHCI_FRINDEX),
            ehci_read_op(EHCI_ASYNCLISTADDR),
            ehci_read_op(EHCI_CONFIGFLAG));
}

void ehci_dump_ports(void)
{
    if (!g_ehci_found)
        return;

    kprintf("EHCI ports (n=%d):\n", g_num_ports);

    for (int i = 0; i < g_num_ports; ++i)
    {
        uint32_t portsc = ehci_read_op(EHCI_PORTSC(i));

        kprintf("  Port %d: PORTSC=%x %s%s%s%s\n",
                i, portsc,
                (portsc & EHCI_PORT_CCS)  ? "CCS " : "",
                (portsc & EHCI_PORT_PED)  ? "PED " : "",
                (portsc & EHCI_PORT_CSC)  ? "CSC " : "",
                (portsc & EHCI_PORT_PEDC) ? "PEDC" : "");
    }
}