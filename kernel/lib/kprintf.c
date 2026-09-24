#include "kprintf.h"
#include "../serial.h"
#include "../fb/fb.h"
#include <stdarg.h>
#include <stdint.h>

static void emit(char c) {
    serial_putc(c);
    if (fb_enabled()) fb_putc(c);
}

static void emit_str(const char *s) {
    while (*s) emit(*s++);
}

static void emit_dec(uint64_t v) {
    char buf[24];
    int i = 0;
    if (v == 0) { emit('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) emit(buf[--i]);
}

static void emit_hex(uint64_t v) {
    emit('0'); emit('x');

    if (v == 0) {
        emit('0');
        return;
    }

    /* Находим старший ненулевой nibble */
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int nib = (v >> i) & 0xF;
        if (!started && nib == 0) continue;
        started = 1;
        emit(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { emit(*p); continue; }
        p++;
        switch (*p) {
            case 'c': {
                char c = (char)va_arg(ap, int);
                emit(c);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                emit_str(s ? s : "(null)");
                break;
            }
            case 'd': {
                int64_t v = va_arg(ap, int64_t);
                if (v < 0) { emit('-'); v = -v; }
                emit_dec((uint64_t)v);
                break;
            }
            case 'u': {
                emit_dec(va_arg(ap, uint64_t));
                break;
            }
            case 'x': {
                emit_hex(va_arg(ap, uint64_t));
                break;
            }
            case 'p': {
                emit_hex((uint64_t)va_arg(ap, void *));
                break;
            }
            case '%': emit('%'); break;
            default:  emit('%'); emit(*p); break;
        }
    }
    va_end(ap);
}

void kpanic_raw(const char *msg) {
    emit_str(msg);
}