#ifndef KPRINTF_H
#define KPRINTF_H

void kprintf(const char *fmt, ...);

/* Прямая печать в serial, без форматирования — для случая, когда
 * kprintf ещё не готов (например, внутри обработчика #DF). */
void kpanic_raw(const char *msg);

#endif