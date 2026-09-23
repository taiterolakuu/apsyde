#include "pit.h"
#include "../arch/x86_64/io.h"

static volatile uint64_t g_ticks = 0;

void pit_init(uint32_t frequency_hz) {
    if (frequency_hz == 0) frequency_hz = 100;

    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;

    /* Отправляем команду: канал 0, режим 3 (square wave), binary */
    outb(PIT_COMMAND, 0x36);

    /* Делитель — 16 бит, сначала младший байт, потом старший */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    g_ticks = 0;
}

uint64_t pit_ticks(void) {
    return g_ticks;
}

void pit_tick(void) {
    g_ticks++;
}