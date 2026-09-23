#include "irq.h"
#include "pic.h"
#include "../lib/kprintf.h"

#define IRQ_COUNT 16

static irq_handler_t g_handlers[IRQ_COUNT];

void irq_init(void) {
    /* Переназначаем PIC: master -> 0x20, slave -> 0x28.
     * Это уводит IRQ0..15 с векторов 0x08..0x0F (конфликт с исключениями) */
    pic_remap(0x20, 0x28);

    /* Маскируем все IRQ — каждый драйвер сам разрешит нужный */
    for (int i = 0; i < 16; i++) {
        pic_mask(i);
        g_handlers[i] = 0;
    }
}

void irq_register(int irq, irq_handler_t h) {
    if (irq < 0 || irq >= IRQ_COUNT) return;
    g_handlers[irq] = h;
}

void irq_unregister(int irq) {
    if (irq < 0 || irq >= IRQ_COUNT) return;
    g_handlers[irq] = 0;
}

void irq_enable(int irq) {
    if (irq < 0 || irq >= IRQ_COUNT) return;
    pic_unmask(irq);
}

void irq_disable(int irq) {
    if (irq < 0 || irq >= IRQ_COUNT) return;
    pic_mask(irq);
}

void irq_dispatch(regs_t *r) {
    /* Вектор = 32 + номер IRQ (после remap) */
    int irq = (int)r->vector - 32;

    if (irq < 0 || irq >= IRQ_COUNT) {
        /* Не должно случиться, но на всякий случай шлём EOI и выходим */
        pic_send_eoi(irq);
        return;
    }

    if (g_handlers[irq]) {
        g_handlers[irq](r);
    }

    /* EOI отправляем ВСЕГДА, даже если обработчика нет —
     * иначе PIC заблокирует все последующие IRQ этого или младшего уровня */
    pic_send_eoi(irq);
}