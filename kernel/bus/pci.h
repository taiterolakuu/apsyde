#ifndef PCI_H
#define PCI_H
#include <stdint.h>

/* ============ Конфигурационное пространство ============ */

#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* Стандартные offset'ы в конфигурации */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION        0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_SUBSYS_VENDOR   0x2C
#define PCI_SUBSYS_ID       0x2E
#define PCI_CAP_PTR         0x34

/* Classes (class_code << 8 | subclass) */
#define PCI_CLASS_STORAGE       0x01
#define PCI_CLASS_NETWORK       0x02
#define PCI_CLASS_DISPLAY       0x03
#define PCI_CLASS_MULTIMEDIA    0x04
#define PCI_CLASS_BRIDGE        0x06
#define PCI_CLASS_SERIAL_BUS    0x0C

/* Subclasses */
#define PCI_SUBCLASS_USB        0x03

/* Prog IF для USB */
#define PCI_PROGIF_UHCI         0x00
#define PCI_PROGIF_OHCI         0x10
#define PCI_PROGIF_EHCI         0x20
#define PCI_PROGIF_XHCI         0x30

/* ============ Структура устройства ============ */

#define PCI_MAX_DEVICES     64

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  revision;
    uint8_t  prog_if;
    uint8_t  subclass;
    uint8_t  class_code;
    uint8_t  header_type;
    uint16_t subsys_vendor;
    uint16_t subsys_id;
} pci_device_t;

/* ============ API ============ */

/* Инициализация: сканирование всех шин, заполнение внутренней таблицы */
void pci_init(void);

/* Сырое чтение/запись конфигурационного пространства */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);

/* Определение размера BAR (в байтах). Возвращает 0 при ошибке.
 * Для MMIO — маскирует младшие 4 бита, для I/O — младшие 2. */
uint32_t pci_bar_size(uint8_t bus, uint8_t dev, uint8_t func, int bar);

/* Доступ к найденным устройствам */
int                pci_device_count(void);
const pci_device_t *pci_device_get(int idx);

/* Поиск по классу/subclass/prog_if */
int pci_find(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
             pci_device_t *out, int max);

/* Удобные обёртки */
int pci_find_ehci(pci_device_t *out);
int pci_find_xhci(pci_device_t *out);
int pci_find_uhci(pci_device_t *out);
int pci_find_ohci(pci_device_t *out);

/* Печать таблицы устройств */
void pci_dump(void);

#endif