#ifndef KEYBOARD_H
#define KEYBOARD_H
#include <stdint.h>
#include "../arch/x86_64/idt.h"   /* для regs_t */

/* Псевдо-символы для стрелок и других extended-клавиш.
 * Они не влезают в char, поэтому keyboard_getchar() возвращает int. */
#define KBD_KEY_UP     0x100
#define KBD_KEY_DOWN   0x101
#define KBD_KEY_LEFT   0x102
#define KBD_KEY_RIGHT  0x103

/* Инициализация: регистрирует обработчик IRQ1 и разрешает его. */
void keyboard_init(void);

/* Возвращает следующий символ или псевдо-символ (KBD_KEY_*).
 * -1, если очередь пуста. Не блокирует. */
int keyboard_getchar(void);

/* Блокирующий вариант: ждёт, пока не появится символ. */
int keyboard_getchar_blocking(void);

/* Обработчик IRQ1 (внутренний, вызывается через irq_register) */
void keyboard_handler(regs_t *r);

/* ============ Отладочный API ============ */

#define KBD_SC_LOG_SIZE 16

/* Забирает накопленные скан-коды и очищает лог.
 * Возвращает количество записанных байт. */
int keyboard_take_scancodes(uint8_t *out, int max);

/* Общее число скан-кодов с момента старта. */
uint64_t keyboard_total_scancodes(void);

#endif