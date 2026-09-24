#ifndef EHCI_H
#define EHCI_H
#include <stdint.h>

/* ============ Capability Registers ============ */

#define EHCI_CAPLENGTH      0x00
#define EHCI_HCIVERSION     0x02
#define EHCI_HCSPARAMS      0x04
#define EHCI_HCCPARAMS      0x08

/* ============ Operational Registers ============ */

#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_USBINTR        0x08
#define EHCI_FRINDEX        0x0C
#define EHCI_CTRLDSSEGMENT  0x10
#define EHCI_PERIODICLIST   0x14
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CONFIGFLAG     0x40
#define EHCI_PORTSC(n)      (0x44 + (n) * 4)

/* ============ USBCMD bits ============ */

#define EHCI_CMD_RS         (1u << 0)
#define EHCI_CMD_HCRESET    (1u << 1)
#define EHCI_CMD_PSE        (1u << 4)
#define EHCI_CMD_ASE        (1u << 5)
#define EHCI_CMD_ITC(n)     (((n) & 0xFF) << 16)

/* ============ USBSTS bits ============ */

#define EHCI_STS_USBINT     (1u << 0)
#define EHCI_STS_ERRINT     (1u << 1)
#define EHCI_STS_PCD        (1u << 2)
#define EHCI_STS_FLR        (1u << 3)
#define EHCI_STS_HSE        (1u << 4)
#define EHCI_STS_IAA        (1u << 5)
#define EHCI_STS_HCH        (1u << 12)
#define EHCI_STS_PS         (1u << 14)
#define EHCI_STS_AS         (1u << 15)

/* ============ CONFIGFLAG ============ */

#define EHCI_CF_CF          (1u << 0)

/* ============ PORTSC bits ============ */

#define EHCI_PORT_CCS       (1u << 0)
#define EHCI_PORT_CSC       (1u << 1)
#define EHCI_PORT_PED       (1u << 2)
#define EHCI_PORT_PEDC      (1u << 3)
#define EHCI_PORT_OCA       (1u << 4)
#define EHCI_PORT_OCC       (1u << 5)
#define EHCI_PORT_FPR       (1u << 6)
#define EHCI_PORT_SUSPEND   (1u << 7)
#define EHCI_PORT_PR        (1u << 8)
#define EHCI_PORT_LS_MASK   (3u << 10)
#define EHCI_PORT_PP        (1u << 12)
#define EHCI_PORT_PO        (1u << 13)
#define EHCI_PORT_PIC_MASK  (3u << 14)
#define EHCI_PORT_PTC_MASK  (0xFu << 16)
#define EHCI_PORT_WKCNNT_E  (1u << 20)
#define EHCI_PORT_WKDSCNNT_E (1u << 21)
#define EHCI_PORT_WKOC_E    (1u << 22)
#define EHCI_PORT_RWCNNT_E  (1u << 23)

/* ============ QH Endpoint Characteristics bits ============ */

#define QH_DEVADDR_MASK     (0x7Fu)
#define QH_EP_MASK          (0xFu << 8)
#define QH_EPS_SHIFT        12
#define QH_DTC              (1u << 14)
#define QH_H                (1u << 15)
#define QH_MAXPKT_SHIFT     16
#define QH_C                (1u << 30)
#define QH_RL_SHIFT         28

/* ============ Token bits ============ */

/* Маска статуса — БЕЗ бита ACTIVE (бит 7).
 * ACTIVE сбрасывается HC по завершении transfer.
 * Остальные 7 бит — код завершения (0 = успех). */
#define QTD_STATUS_MASK     0x7Fu

#define QTD_PID_SHIFT       8
#define QTD_PID_OUT         0
#define QTD_PID_IN          1
#define QTD_PID_SETUP       2
#define QTD_CERR_SHIFT      10
#define QTD_CPAGE_SHIFT     12
#define QTD_TOTAL_SHIFT     16
#define QTD_IOC             (1u << 31)

#define QTD_ACTIVE          (1u << 7)

/* ============ Терминатор указателя ============ */

#define EHCI_TERMINATE      1u
#define EHCI_QH_TYPE        (1u << 1)   /* 1 = QH, 0 = qTD */

/* ============ Структуры ============ */

/* Queue Head. 48 байт, выровнен на 32. */
typedef struct __attribute__((aligned(32))) {
    uint32_t horiz_link;        /* next QH / qTD */
    uint32_t ep_char;           /* Endpoint Characteristics */
    uint32_t ep_caps;           /* Endpoint Capabilities */
    uint32_t current_qtd;       /* Current qTD (HC updates) */
    uint32_t next_qtd;          /* Next qTD */
    uint32_t alt_next_qtd;      /* Alt Next qTD */
    uint32_t token;             /* Token (overlay) */
    uint32_t buf[5];            /* Buffer pointers */
    uint32_t reserved[4];       /* до 64 байт для выравнивания */
} ehci_qh_t;

/* Queue Transfer Descriptor. 32 байта, выровнен на 32. */
typedef struct __attribute__((aligned(32))) {
    uint32_t next_qtd;
    uint32_t alt_next_qtd;
    uint32_t token;
    uint32_t buf[5];
    uint32_t _pad[3];           /* до 64 байт для выравнивания */
} ehci_qtd_t;

/* USB Device Descriptor (18 байт) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

/* ============ Публичный API ============ */

void ehci_init(void);
int  ehci_present(void);
void ehci_dump(void);

uint32_t ehci_read_cap(uint32_t off);
uint32_t ehci_read_op (uint32_t off);
void     ehci_write_op(uint32_t off, uint32_t val);

/* Управление контроллером */
void ehci_init_controller(void);
void ehci_reset_port(int port);
int  ehci_port_connected(int port);
int  ehci_find_device_port(void);   /* -1 если нет устройств */

/* Control transfer. Возвращает 0 при успехе. */
int ehci_control_transfer(uint8_t dev_addr, uint8_t ep,
                          const void *setup_pkt,
                          void *in_data, uint32_t in_len);

/* Обёртки для USB-дескрипторов */
int ehci_get_device_descriptor(uint8_t dev_addr,
                               usb_device_descriptor_t *out);

/* Отладка */
void ehci_dump_ports(void);

#endif