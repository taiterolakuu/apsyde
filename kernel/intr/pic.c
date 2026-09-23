#include "pic.h"
#include "../arch/x86_64/io.h"

/* Классическая последовательность инициализации 8259.
 * ICW1: начало инициализации, ожидаем ICW4
 * ICW2: базовый вектор (offset)
 * ICW3: каскадирование (master: slave на IRQ2 -> 0x04; slave: подключён к IRQ2 -> 0x02)
 * ICW4: режим 8086 */
void pic_remap(int offset1, int offset2) {
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();

    outb(PIC1_DATA, offset1); io_wait();
    outb(PIC2_DATA, offset2); io_wait();

    outb(PIC1_DATA, 0x04); io_wait();   /* slave на IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();   /* slave identity */

    outb(PIC1_DATA, 0x01); io_wait();   /* 8086/88 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    /* Восстанавливаем маски */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_mask(int irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) | (1 << irq);
    outb(port, value);
}

void pic_unmask(int irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

void pic_send_eoi(int irq) {
    /* Для IRQ >= 8 нужно отправить EOI и в slave, и в master */
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}