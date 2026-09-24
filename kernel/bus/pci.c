#include "pci.h"
#include "../arch/x86_64/io.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

/* ============ Низкоуровневый доступ ============ */

static inline uint32_t pci_make_addr(uint8_t bus, uint8_t dev,
                                     uint8_t func, uint8_t off) {
    return (1u << 31)
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)func << 8)
         | ((uint32_t)off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    outl(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, func, off & 0xFC);
    return (uint16_t)(v >> ((off & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_read32(bus, dev, func, off & 0xFC);
    return (uint8_t)(v >> ((off & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, val);
}

/* ============ Внутренняя таблица ============ */

static pci_device_t g_devices[PCI_MAX_DEVICES];
static int          g_device_count = 0;

/* ============ Определение размера BAR ============ */

uint32_t pci_bar_size(uint8_t bus, uint8_t dev, uint8_t func, int bar) {
    uint8_t off = PCI_BAR0 + bar * 4;
    uint32_t orig = pci_read32(bus, dev, func, off);
    pci_write32(bus, dev, func, off, 0xFFFFFFFF);
    uint32_t size_mask = pci_read32(bus, dev, func, off);
    pci_write32(bus, dev, func, off, orig);

    if (size_mask == 0 || size_mask == 0xFFFFFFFF) return 0;

    if ((size_mask & 1) == 0) {
        size_mask &= ~0xF;
    } else {
        size_mask &= ~0x3;
    }

    return (~size_mask) + 1;
}

/* ============ Сканирование ============ */

static void add_device(uint8_t bus, uint8_t dev, uint8_t func) {
    if (g_device_count >= PCI_MAX_DEVICES) return;

    pci_device_t *d = &g_devices[g_device_count];

    d->bus         = bus;
    d->device      = dev;
    d->function    = func;
    d->vendor_id   = pci_read16(bus, dev, func, PCI_VENDOR_ID);
    d->device_id   = pci_read16(bus, dev, func, PCI_DEVICE_ID);
    d->revision    = pci_read8 (bus, dev, func, PCI_REVISION);
    d->prog_if     = pci_read8 (bus, dev, func, PCI_PROG_IF);
    d->subclass    = pci_read8 (bus, dev, func, PCI_SUBCLASS);
    d->class_code  = pci_read8 (bus, dev, func, PCI_CLASS);
    d->header_type = pci_read8 (bus, dev, func, PCI_HEADER_TYPE) & 0x7F;
    d->subsys_vendor = pci_read16(bus, dev, func, PCI_SUBSYS_VENDOR);
    d->subsys_id     = pci_read16(bus, dev, func, PCI_SUBSYS_ID);

    g_device_count++;
}

static void scan_function(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t vendor = pci_read16(bus, dev, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return;

    add_device(bus, dev, func);

    /* PCI-to-PCI bridge — сканируем вторичную шину */
    uint8_t header = pci_read8(bus, dev, func, PCI_HEADER_TYPE) & 0x7F;
    if (header == 0x01) {
        uint8_t secondary_bus = pci_read8(bus, dev, func, 0x19);
        if (secondary_bus != 0) {
            for (uint8_t d = 0; d < 32; d++) {
                for (uint8_t f = 0; f < 8; f++) {
                    scan_function(secondary_bus, d, f);
                }
            }
        }
    }
}

static void scan_bus(uint8_t bus) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        uint16_t vendor = pci_read16(bus, dev, 0, PCI_VENDOR_ID);
        if (vendor == 0xFFFF) continue;

        /* Функция 0 всегда существует, если vendor != 0xFFFF */
        scan_function(bus, dev, 0);

        /* Многофункциональное устройство? */
        uint8_t header = pci_read8(bus, dev, 0, PCI_HEADER_TYPE);
        if (header & 0x80) {
            for (uint8_t f = 1; f < 8; f++) {
                if (pci_read16(bus, dev, f, PCI_VENDOR_ID) != 0xFFFF) {
                    scan_function(bus, dev, f);
                }
            }
        }
    }
}

void pci_init(void) {
    g_device_count = 0;

    /* Сканируем все шины 0..255. Через PCI-to-PCI bridge
     * scan_function дойдёт до вторичных шин рекурсивно. */
    scan_bus(0);

    /* Страховка: некоторые чипсеты не имеют bridge на шине 0,
     * но имеют устройства на других шинах */
    if (g_device_count == 0) {
        for (uint8_t b = 1; b < 16; b++) {
            scan_bus(b);
        }
    }

    kprintf("[+] PCI: найдено устройств: %d\n", g_device_count);
}

/* ============ Доступ к результатам ============ */

int pci_device_count(void) {
    return g_device_count;
}

const pci_device_t *pci_device_get(int idx) {
    if (idx < 0 || idx >= g_device_count) return 0;
    return &g_devices[idx];
}

int pci_find(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
             pci_device_t *out, int max) {
    int found = 0;
    for (int i = 0; i < g_device_count && found < max; i++) {
        pci_device_t *d = &g_devices[i];

        if (d->class_code != class_code) continue;
        if (subclass != 0xFF && d->subclass != subclass) continue;
        if (prog_if  != 0xFF && d->prog_if  != prog_if)  continue;

        out[found++] = *d;
    }
    return found;
}

int pci_find_ehci(pci_device_t *out) {
    return pci_find(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                    PCI_PROGIF_EHCI, out, 1);
}

int pci_find_xhci(pci_device_t *out) {
    return pci_find(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                    PCI_PROGIF_XHCI, out, 1);
}

int pci_find_uhci(pci_device_t *out) {
    return pci_find(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                    PCI_PROGIF_UHCI, out, 1);
}

int pci_find_ohci(pci_device_t *out) {
    return pci_find(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                    PCI_PROGIF_OHCI, out, 1);
}

/* ============ Печать ============ */

static const char *class_name(uint8_t class_code) {
    switch (class_code) {
        case 0x00: return "Unclassified";
        case 0x01: return "Storage";
        case 0x02: return "Network";
        case 0x03: return "Display";
        case 0x04: return "Multimedia";
        case 0x05: return "Memory";
        case 0x06: return "Bridge";
        case 0x07: return "Communication";
        case 0x08: return "SysPeriph";
        case 0x09: return "Input";
        case 0x0A: return "Docking";
        case 0x0B: return "Processor";
        case 0x0C: return "SerialBus";
        case 0x0D: return "Wireless";
        case 0x0E: return "IntelligentIO";
        case 0x0F: return "Satellite";
        case 0x10: return "Encryption";
        case 0x11: return "SignalProc";
        default:   return "Unknown";
    }
}

static const char *usb_progif_name(uint8_t prog_if) {
    switch (prog_if) {
        case PCI_PROGIF_UHCI: return "UHCI";
        case PCI_PROGIF_OHCI: return "OHCI";
        case PCI_PROGIF_EHCI: return "EHCI";
        case PCI_PROGIF_XHCI: return "xHCI";
        default:              return "USB?";
    }
}

void pci_dump(void) {
    kprintf("PCI devices: %d\n\n", g_device_count);
    kprintf("B:D.F    Vendor:Device  Class Sub  ProgIF  Name\n");
    kprintf("---------------------------------------------------\n");

    for (int i = 0; i < g_device_count; i++) {
        pci_device_t *d = &g_devices[i];

        kprintf("%u:%u.%u   %x:%x   ",
                (uint32_t)d->bus, (uint32_t)d->device, (uint32_t)d->function,
                d->vendor_id, d->device_id);

        kprintf("%x  %x  %x   ",
                (uint32_t)d->class_code,
                (uint32_t)d->subclass,
                (uint32_t)d->prog_if);

        kprintf("%s", class_name(d->class_code));

        if (d->class_code == PCI_CLASS_SERIAL_BUS &&
            d->subclass == PCI_SUBCLASS_USB) {
            kprintf(" (%s)", usb_progif_name(d->prog_if));
        }

        kprintf("\n");
    }
}