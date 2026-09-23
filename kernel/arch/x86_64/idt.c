#include "idt.h"
#include "../../lib/kprintf.h"
#include "../../serial.h"
#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry g_idt[256];
static struct idt_ptr   g_idtp;

static void set_gate(int n, uint64_t handler) {
    g_idt[n].offset_low  = handler & 0xFFFF;
    g_idt[n].offset_mid  = (handler >> 16) & 0xFFFF;
    g_idt[n].offset_high = (handler >> 32) & 0xFFFFFFFF;
    g_idt[n].selector    = 0x08;
    g_idt[n].ist         = 0;
    g_idt[n].type_attr   = 0x8E;    /* present, ring0, 64-bit interrupt gate */
    g_idt[n].reserved    = 0;
}

void idt_init(void) {
    g_idtp.limit = sizeof(g_idt) - 1;
    g_idtp.base  = (uint64_t)&g_idt;

    /* Исключения 0..31 */
    void (*isr_handlers[32])(void) = {
        isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
        isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    for (int i = 0; i < 32; i++) {
        set_gate(i, (uint64_t)isr_handlers[i]);
    }

    /* IRQ 0..15 -> вектора 32..47 */
    void (*irq_handlers[16])(void) = {
        irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
        irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15
    };
    for (int i = 0; i < 16; i++) {
        set_gate(32 + i, (uint64_t)irq_handlers[i]);
    }

    __asm__ volatile ("lidt %0" : : "m"(g_idtp));
}

static const char *exception_names[32] = {
    "Divide Error",           "Debug",
    "NMI",                    "Breakpoint",
    "Overflow",               "Bound Range Exceeded",
    "Invalid Opcode",         "Device Not Available",
    "Double Fault",           "Coprocessor Segment Overrun",
    "Invalid TSS",            "Segment Not Present",
    "Stack-Segment Fault",    "General Protection Fault",
    "Page Fault",             "Reserved",
    "x87 FP Exception",       "Alignment Check",
    "Machine Check",          "SIMD FP Exception",
    "Virtualization",         "Control Protection",
    "Reserved",               "Reserved",
    "Reserved",               "Reserved",
    "Reserved",               "Reserved",
    "Hypervisor Injection",   "VMM Communication",
    "Security Exception",     "Reserved"
};

void isr_dispatch(regs_t *r) {
    uint64_t v = r->vector;

    kprintf("\n");
    kprintf("!!! EXCEPTION %u: %s !!!\n", v, v < 32 ? exception_names[v] : "Unknown");

    /* Код ошибки есть только у некоторых векторов */
    if (v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21) {
        kprintf("    error_code = %x\n", r->error_code);
    }

    kprintf("    RAX=%x RBX=%x RCX=%x RDX=%x\n", r->rax, r->rbx, r->rcx, r->rdx);
    kprintf("    RSI=%x RDI=%x RBP=%x RSP=%x\n", r->rsi, r->rdi, r->rbp, r->rsp);
    kprintf("    R8 =%x R9 =%x R10=%x R11=%x\n", r->r8, r->r9, r->r10, r->r11);
    kprintf("    R12=%x R13=%x R14=%x R15=%x\n", r->r12, r->r13, r->r14, r->r15);
    kprintf("    RIP=%x CS =%x RFLAGS=%x SS=%x\n", r->rip, r->cs, r->rflags, r->ss);

    if (v == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("    CR2 (faulting address) = %x\n", cr2);
        kprintf("    Error bits: %s%s%s%s\n",
                (r->error_code & 1) ? "P " : "NP",
                (r->error_code & 2) ? " W" : " R",
                (r->error_code & 4) ? " U" : " S",
                (r->error_code & 8) ? " RSVD" : "");
    }

    kprintf("\nSystem halted.\n");
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}