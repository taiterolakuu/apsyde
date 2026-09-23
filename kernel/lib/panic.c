/* kernel/lib/panic.c */
#include "panic.h"
#include "kprintf.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

static char panic_buf[1024];

static void vsnprintf_simple(char *buf, size_t n, const char *fmt, va_list ap) {
    size_t pos = 0;
    char numbuf[24];

    for (const char *p = fmt; *p && pos < n - 1; p++) {
        if (*p != '%') { buf[pos++] = *p; continue; }
        p++;
        switch (*p) {
            case 'c':
                buf[pos++] = (char)va_arg(ap, int);
                break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && pos < n - 1) buf[pos++] = *s++;
                break;
            }
            case 'd': {
                int64_t v = va_arg(ap, int64_t);
                if (v < 0) { buf[pos++] = '-'; v = -v; }
                int i = 0;
                if (v == 0) numbuf[i++] = '0';
                while (v > 0) { numbuf[i++] = '0' + (v % 10); v /= 10; }
                while (i > 0 && pos < n - 1) buf[pos++] = numbuf[--i];
                break;
            }
            case 'x': {
                uint64_t v = va_arg(ap, uint64_t);
                int i = 0;
                if (v == 0) numbuf[i++] = '0';
                while (v > 0) {
                    int nib = v & 0xF;
                    numbuf[i++] = nib < 10 ? '0' + nib : 'a' + nib - 10;
                    v >>= 4;
                }
                while (i > 0 && pos < n - 1) buf[pos++] = numbuf[--i];
                break;
            }
            case '%':
                buf[pos++] = '%';
                break;
            default:
                buf[pos++] = '%';
                if (pos < n - 1) buf[pos++] = *p;
                break;
        }
    }
    buf[pos] = 0;
}

__attribute__((noreturn))
void panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_simple(panic_buf, sizeof(panic_buf), fmt, ap);
    va_end(ap);

    kprintf("\n*** KERNEL PANIC ***\n");
    kprintf("%s\n", panic_buf);
    kprintf("System halted.\n");

    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
    __builtin_unreachable();
}

__attribute__((noreturn))
void panic_at(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_simple(panic_buf, sizeof(panic_buf), fmt, ap);
    va_end(ap);

    kprintf("\n*** KERNEL PANIC at %s:%d ***\n", file, line);
    kprintf("%s\n", panic_buf);
    kprintf("System halted.\n");

    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
    __builtin_unreachable();
}