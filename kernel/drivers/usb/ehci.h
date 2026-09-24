#ifndef EHCI_H
#define EHCI_H
#include <stdint.h>

/* ============ Структура EHCI регистров ============ */

/* Capability Registers (read-only) */
#define EHCI_CAPLENGTH      0x00   /* 1 байт: длина cap regs */
#define EHCI_HCIVERSION     0x02   /* 2 байта: версия */
#define EHCI_HCSPARAMS      0x04   /* 4 байта: structural params */
#define EHCI_HCCPARAMS      0x08   /* 4 байта: capability params */

/* Operational Registers (offset от CAPLENGTH) */
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

#define EHCI_CMD_RS         (1u << 0)   /* Run/Stop */
#define EHCI_CMD_HCRESET    (1u << 1)   /* Host Controller Reset */
#define EHCI_CMD_PSE        (1u << 4)   /* Periodic Schedule Enable */
#define EHCI_CMD_ASE        (1u << 5)   /* Async Schedule Enable */
#define EHCI_CMD_ITC_MASK   (0xFFu << 16)

/* ============ USBSTS bits ============ */

#define EHCI_STS_HCH        (1u << 12)  /* HCHalted */
#define EHCI_STS_PCD        (1u << 4)   /* Port Change Detect */
#define EHCI_STS_PS         (1u << 14)  /* Periodic Schedule Status */
#define EHCI_STS_AS         (1u << 15)  /* Async Schedule Status */

/* ============ CONFIGFLAG bits ============ */

#define EHCI_CF_CF          (1u << 0)   /* Configure Flag */

/* ============ PORTSC bits ============ */

#define EHCI_PORT_CCS       (1u << 0)   /* Current Connect Status */
#define EHCI_PORT_CSC       (1u << 1)   /* Connect Status Change */
#define EHCI_PORT_PED       (1u << 2)   /* Port Enabled/Disabled */
#define EHCI_PORT_PEDC      (1u << 3)   /* Port Enable/Disable Change */
#define EHCI_PORT_OCA       (1u << 4)   /* Over-current Active */
#define EHCI_PORT_OCC       (1u << 5)   /* Over-current Change */
#define EHCI_PORT_FPR       (1u << 6)   /* Force Port Resume */
#define EHCI_PORT_SUSPEND   (1u << 7)
#define EHCI_PORT_PR        (1u << 8)   /* Port Reset */
#define EHCI_PORT_LS_MASK   (3u << 10)  /* Line Status */
#define EHCI_PORT_PP        (1u << 12)  /* Port Power */
#define EHCI_PORT_PO        (1u << 13)  /* Port Owner */
#define EHCI_PORT_PIC_MASK  (3u << 14)  /* Port Indicator Control */
#define EHCI_PORT_PTC_MASK  (0xFu << 16)/* Port Test Control */
#define EHCI_PORT_WKCNNT_E  (1u << 20)
#define EHCI_PORT_WKDSCNNT_E (1u << 21)
#define EHCI_PORT_WKOC_E    (1u << 22)
#define EHCI_PORT_RWCNNT_E  (1u << 23)

/* ============ Публичный API ============ */

/* Инициализация: чтение BAR0, маппинг MMIO, чтение capability-регистров.
 * Вызывать после pci_init. */
void ehci_init(void);

/* Есть ли EHCI в системе? */
int ehci_present(void);

/* Информация о контроллере */
void ehci_dump(void);

/* Сырой доступ к регистрам (для отладки и следующих заходов) */
uint32_t ehci_read_cap(uint32_t off);
uint32_t ehci_read_op (uint32_t off);
void     ehci_write_op(uint32_t off, uint32_t val);

#endif