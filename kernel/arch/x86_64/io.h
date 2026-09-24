#ifndef IO_H
#define IO_H
#include <stdint.h>

/* ============ 8-битные ============ */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* ============ 16-битные ============ */

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* ============ 32-битные ============ */

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* ============ Задержка для медленных устройств ============ */

/* Запись в неиспользуемый порт 0x80 создаёт короткую задержку.
 * Используется для устройств, которым нужно время между командами
 * (например, PIC 8259). */
static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif