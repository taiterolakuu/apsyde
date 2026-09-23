#include "serial.h"
#include "arch/x86_64/io.h"    /* ← добавлен x86_64 */

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_puthex(uint64_t v) {
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int nib = (v >> i) & 0xF;
        serial_putc(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
}

void serial_putdec(uint64_t v) {
    char buf[24];
    int i = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) serial_putc(buf[--i]);
}