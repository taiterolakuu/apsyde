#ifndef PIC_H
#define PIC_H
#include <stdint.h>

/* Порты 8259 PIC */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC_EOI    0x20

void pic_remap(int offset1, int offset2);
void pic_mask(int irq);
void pic_unmask(int irq);
void pic_send_eoi(int irq);

#endif