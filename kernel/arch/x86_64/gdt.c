#include "gdt.h"

/* 3 записи: null, kernel code, kernel data.
 * Для ring0 хватит; TSS и user-сегменты добавим позже. */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_entry g_gdt[3];
static struct gdt_ptr   g_gdtp;

extern void gdt_flush(uint64_t gdtp_addr);

static void set_entry(int i, uint32_t base, uint32_t limit,
                      uint8_t access, uint8_t gran) {
    g_gdt[i].base_low    = base & 0xFFFF;
    g_gdt[i].base_mid    = (base >> 16) & 0xFF;
    g_gdt[i].base_high   = (base >> 24) & 0xFF;
    g_gdt[i].limit_low   = limit & 0xFFFF;
    g_gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    g_gdt[i].access      = access;
}

void gdt_init(void) {
    set_entry(0, 0, 0,          0,    0);        /* null */
    set_entry(1, 0, 0xFFFFF,    0x9A, 0xA0);     /* code: present, ring0, exec/read */
    set_entry(2, 0, 0xFFFFF,    0x92, 0xA0);     /* data: present, ring0, read/write */

    g_gdtp.limit = sizeof(g_gdt) - 1;
    g_gdtp.base  = (uint64_t)&g_gdt;

    gdt_flush((uint64_t)&g_gdtp);
}