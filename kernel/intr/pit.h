#ifndef PIT_H
#define PIT_H
#include <stdint.h>

#define PIT_BASE_FREQ  1193182
#define PIT_CHANNEL0   0x40
#define PIT_COMMAND    0x43

void     pit_init(uint32_t frequency_hz);
uint64_t pit_ticks(void);
void     pit_tick(void);   /* вызывается из обработчика IRQ0 */

#endif