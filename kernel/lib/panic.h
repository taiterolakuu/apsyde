/* kernel/lib/panic.h */
#ifndef PANIC_H
#define PANIC_H

__attribute__((noreturn))
void panic(const char *fmt, ...);

__attribute__((noreturn))
void panic_at(const char *file, int line, const char *fmt, ...);

#define PANIC(...) panic_at(__FILE__, __LINE__, __VA_ARGS__)

#endif