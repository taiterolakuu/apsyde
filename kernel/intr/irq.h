#ifndef IRQ_H
#define IRQ_H
#include "../arch/x86_64/idt.h"   /* regs_t */

typedef void (*irq_handler_t)(regs_t *);

void irq_init(void);                            /* PIC remap + сброс таблицы */
void irq_register(int irq, irq_handler_t h);
void irq_unregister(int irq);
void irq_dispatch(regs_t *r);                   /* вызывается из isr.asm */
void irq_enable(int irq);                       /* разрешает на уровне PIC */
void irq_disable(int irq);                      /* запрещает на уровне PIC */

#endif