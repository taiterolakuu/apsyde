#include <stdint.h>
#include <stddef.h>
#include <boot_info.h>
#include "serial.h"
#include "fb/fb.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "intr/pic.h"
#include "intr/pit.h"
#include "intr/irq.h"
#include "drivers/keyboard.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "bus/pci.h"
#include "drivers/usb/ehci.h"
#include "input/input.h"
#include "shell/shell.h"

static boot_info_t g_boot_info;

const boot_info_t *shell_get_boot_info(void) {
    return &g_boot_info;
}

static void validate_boot_info(const boot_info_t *bi) {
    if (bi == NULL)
        panic("boot_info is NULL");
    if (bi->magic != BOOT_INFO_MAGIC)
        panic("boot_info magic mismatch: got %x", bi->magic);
    if (bi->framebuffer_base == 0)
        panic("boot_info: framebuffer_base == 0");
    if (bi->horizontal_resolution == 0 || bi->vertical_resolution == 0)
        panic("boot_info: invalid resolution %ux%u",
              bi->horizontal_resolution, bi->vertical_resolution);
    if (bi->pixels_per_scanline < bi->horizontal_resolution)
        panic("boot_info: pitch %u < width %u",
              bi->pixels_per_scanline, bi->horizontal_resolution);
    if (bi->memory_map == NULL || bi->memory_map_size == 0)
        panic("boot_info: memory_map missing");
    if (bi->descriptor_size == 0)
        panic("boot_info: descriptor_size == 0");
    if (bi->memory_map_size % bi->descriptor_size != 0)
        panic("boot_info: memory_map_size %% descriptor_size != 0");
}

static void timer_handler(regs_t *r) {
    (void)r;
    pit_tick();
}

/* ============ Автотест EHCI ============ */

static void ehci_autotest(void) {
    kprintf("\n[AUTO] EHCI ports scan:\n");

    int found_port = -1;
    for (int p = 0; p < 16; p++) {
        if (ehci_port_connected(p)) {
            kprintf("[AUTO] устройство на порту %d\n", p);
            found_port = p;
            break;
        }
    }

    if (found_port < 0) {
        kprintf("[AUTO] устройств на портах нет\n");
        return;
    }

    ehci_reset_port(found_port);

    usb_device_descriptor_t desc;
    if (ehci_get_device_descriptor(0, &desc) != 0) {
        kprintf("[AUTO] GET_DESCRIPTOR failed\n");
        return;
    }

    kprintf("[AUTO] Device Descriptor:\n");
    kprintf("       bLength            = %u\n", (uint32_t)desc.bLength);
    kprintf("       bDescriptorType    = %u\n", (uint32_t)desc.bDescriptorType);
    kprintf("       bcdUSB             = %x\n", (uint32_t)desc.bcdUSB);
    kprintf("       bDeviceClass       = %u\n", (uint32_t)desc.bDeviceClass);
    kprintf("       bDeviceSubClass    = %u\n", (uint32_t)desc.bDeviceSubClass);
    kprintf("       bDeviceProtocol    = %u\n", (uint32_t)desc.bDeviceProtocol);
    kprintf("       bMaxPacketSize0    = %u\n", (uint32_t)desc.bMaxPacketSize0);
    kprintf("       idVendor           = %x\n", (uint32_t)desc.idVendor);
    kprintf("       idProduct          = %x\n", (uint32_t)desc.idProduct);
    kprintf("       bcdDevice          = %x\n", (uint32_t)desc.bcdDevice);
    kprintf("       bNumConfigurations = %u\n", (uint32_t)desc.bNumConfigurations);
}

/* ============ kernel_main ============ */

void kernel_main(boot_info_t *bi) {
    serial_init();
    validate_boot_info(bi);
    memcpy(&g_boot_info, bi, sizeof(boot_info_t));

    fb_init(&g_boot_info);
    fb_clear();

    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Celestis Kernel\n");
    kprintf("========================================\n");

    kprintf("[+] boot_info скопирован в .bss\n");

    gdt_init();
    kprintf("[+] GDT инициализирован\n");

    idt_init();
    kprintf("[+] IDT инициализирован\n");

    irq_init();
    kprintf("[+] PIC переназначен\n");

    irq_register(0, timer_handler);
    pit_init(100);
    irq_enable(0);
    kprintf("[+] PIT настроен (100 Гц)\n");

    keyboard_init();
    kprintf("[+] PS/2 клавиатура подключена (IRQ1)\n");

    /* --- Input abstraction (PS/2 + USB + DEMO fallback) --- */
    input_init();

    /* --- PMM --- */
    pmm_init(&g_boot_info);

    {
        uint64_t map_start = (uint64_t)g_boot_info.memory_map & ~0xFFFULL;
        uint64_t map_end   = ((uint64_t)g_boot_info.memory_map +
                              g_boot_info.memory_map_size + 0xFFF) & ~0xFFFULL;
        uint64_t map_pages = (map_end - map_start) / 4096;
        pmm_region_set_used(map_start, map_pages, 1);
        kprintf("[+] PMM: страницы memory_map защищены (%u страниц)\n",
                (uint32_t)map_pages);
    }

    {
        uint64_t fb_start = g_boot_info.framebuffer_base & ~0xFFFULL;
        uint64_t fb_end   = ((uint64_t)g_boot_info.framebuffer_base +
                             g_boot_info.framebuffer_size + 0xFFF) & ~0xFFFULL;
        uint64_t fb_pages = (fb_end - fb_start) / 4096;
        pmm_region_set_used(fb_start, fb_pages, 1);
        kprintf("[+] PMM: framebuffer зарезервирован (%u страниц)\n",
                (uint32_t)fb_pages);
    }

    /* --- Heap --- */
    heap_init();

    /* --- VMM --- */
    vmm_init();

    /* --- PCI --- */
    pci_init();

    /* --- USB EHCI --- */
    ehci_init();
    if (ehci_present()) {
        ehci_init_controller();
        ehci_autotest();
    }

    kprintf("\n[*] Framebuffer: base=%x  size=%u\n",
            g_boot_info.framebuffer_base, g_boot_info.framebuffer_size);
    kprintf("[*] Resolution:  %ux%u  pitch=%u  format=%u\n",
            g_boot_info.horizontal_resolution, g_boot_info.vertical_resolution,
            g_boot_info.pixels_per_scanline, g_boot_info.pixel_format);

    kprintf("\n[*] Включаем прерывания (sti)...\n");
    __asm__ volatile ("sti");

    shell_run();

    panic("shell вернулся — этого не должно быть");
}