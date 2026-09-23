#include "keyboard.h"
#include "../intr/irq.h"
#include "../lib/kprintf.h"
#include "../arch/x86_64/io.h"

#define KBD_DATA    0x60
#define KBD_STATUS  0x64

#define KBD_BUF_SIZE 256

/* Кольцевой буфер: храним int, чтобы влезали псевдо-символы KBD_KEY_*. */
static volatile int      g_buf[KBD_BUF_SIZE];
static volatile uint32_t g_head = 0;
static volatile uint32_t g_tail = 0;

/* Состояние модификаторов */
static int g_shift = 0;
static int g_ctrl  = 0;
static int g_alt   = 0;
static int g_caps  = 0;
static int g_extended = 0;   /* префикс 0xE0 */

/* Отладочный лог скан-кодов */
static uint8_t          g_sc_log[KBD_SC_LOG_SIZE];
static volatile int     g_sc_head  = 0;
static volatile int     g_sc_count = 0;
static volatile uint64_t g_sc_total = 0;

static void log_scancode(uint8_t sc) {
    g_sc_log[g_sc_head] = sc;
    g_sc_head = (g_sc_head + 1) % KBD_SC_LOG_SIZE;
    if (g_sc_count < KBD_SC_LOG_SIZE) g_sc_count++;
    g_sc_total++;
}

int keyboard_take_scancodes(uint8_t *out, int max) {
    int n = 0;
    int start = (g_sc_head - g_sc_count + KBD_SC_LOG_SIZE) % KBD_SC_LOG_SIZE;
    for (int i = 0; i < g_sc_count && n < max; i++) {
        out[n++] = g_sc_log[(start + i) % KBD_SC_LOG_SIZE];
    }
    g_sc_count = 0;
    return n;
}

uint64_t keyboard_total_scancodes(void) {
    return g_sc_total;
}

static void buf_push(int c) {
    uint32_t next = (g_head + 1) % KBD_BUF_SIZE;
    if (next == g_tail) return;   /* переполнение — теряем символ */
    g_buf[g_head] = c;
    g_head = next;
}

static int buf_pop(void) {
    if (g_tail == g_head) return -1;
    int c = g_buf[g_tail];
    g_tail = (g_tail + 1) % KBD_BUF_SIZE;
    return c;
}

/* Раскладка: скан-код set 1 → ASCII (без Shift) */
static const char keymap_lower[128] = {
    /* 0x00 */ 0,    27,  '1',  '2',  '3',  '4',  '5',  '6',
    /* 0x08 */ '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
    /* 0x10 */ 'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
    /* 0x18 */ 'o',  'p',  '[',  ']',  '\n', 0,   'a',  's',
    /* 0x20 */ 'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
    /* 0x28 */ '\'', '`',  0,   '\\', 'z',  'x',  'c',  'v',
    /* 0x30 */ 'b',  'n',  'm',  ',',  '.',  '/',  0,   '*',
    /* 0x38 */ 0,    ' ',  0,    0,    0,    0,    0,    0,
    /* 0x40 */ 0,    0,    0,    0,    0,    0,    0,    '7',
    /* 0x48 */ '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    /* 0x50 */ '2',  '3',  '0',  '.',  0,    0,    0,    0,
    /* 0x58 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x60 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x68 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x70 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x78 */ 0,    0,    0,    0,    0,    0,    0,    0,
};

static const char keymap_upper[128] = {
    /* 0x00 */ 0,    27,  '!',  '@',  '#',  '$',  '%',  '^',
    /* 0x08 */ '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
    /* 0x10 */ 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
    /* 0x18 */ 'O',  'P',  '{',  '}',  '\n', 0,   'A',  'S',
    /* 0x20 */ 'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
    /* 0x28 */ '"',  '~',  0,   '|',  'Z',  'X',  'C',  'V',
    /* 0x30 */ 'B',  'N',  'M',  '<',  '>',  '?',  0,   '*',
    /* 0x38 */ 0,    ' ',  0,    0,    0,    0,    0,    0,
    /* 0x40 */ 0,    0,    0,    0,    0,    0,    0,    '7',
    /* 0x48 */ '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
    /* 0x50 */ '2',  '3',  '0',  '.',  0,    0,    0,    0,
    /* 0x58 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x60 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x68 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x70 */ 0,    0,    0,    0,    0,    0,    0,    0,
    /* 0x78 */ 0,    0,    0,    0,    0,    0,    0,    0,
};

void keyboard_handler(regs_t *r) {
    (void)r;
    uint8_t sc = inb(KBD_DATA);
    log_scancode(sc);

    /* Префикс extended — следующая клавиша имеет другой смысл */
    if (sc == 0xE0) { g_extended = 1; return; }

    /* Extended-клавиши: обрабатываем стрелки */
    if (g_extended) {
        g_extended = 0;

        int released = sc & 0x80;
        uint8_t code = sc & 0x7F;

        if (released) return;   /* реагируем только на нажатие */

        switch (code) {
            case 0x48: buf_push(KBD_KEY_UP);    return;
            case 0x50: buf_push(KBD_KEY_DOWN);  return;
            case 0x4B: buf_push(KBD_KEY_LEFT);  return;
            case 0x4D: buf_push(KBD_KEY_RIGHT); return;
            default:   return;   /* прочие extended — игнорируем */
        }
    }

    /* Отпускание клавиши: бит 7 установлен */
    int released = sc & 0x80;
    uint8_t code = sc & 0x7F;

    /* Модификаторы обрабатываются и при нажатии, и при отпускании */
    switch (code) {
        case 0x2A: case 0x36:   /* LShift / RShift */
            g_shift = !released;
            return;
        case 0x1D:              /* LCtrl */
            g_ctrl = !released;
            return;
        case 0x38:              /* LAlt */
            g_alt = !released;
            return;
        case 0x3A:              /* CapsLock — только при нажатии */
            if (!released) g_caps = !g_caps;
            return;
    }

    if (released) return;

    char c = 0;
    if (g_shift ^ g_caps) {
        c = keymap_upper[code];
    } else {
        c = keymap_lower[code];
    }

    /* Ctrl+буква: превращаем в управляющий код */
    if (g_ctrl && c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 1);
    }

    if (c != 0) {
        buf_push((int)(unsigned char)c);
    }
}

void keyboard_init(void) {
    g_head = g_tail = 0;
    g_shift = g_ctrl = g_alt = g_caps = 0;
    g_extended = 0;
    g_sc_head = 0;
    g_sc_count = 0;
    g_sc_total = 0;

    irq_register(1, keyboard_handler);
    irq_enable(1);
}

int keyboard_getchar(void) {
    return buf_pop();
}

int keyboard_getchar_blocking(void) {
    int c;
    while ((c = buf_pop()) < 0) {
        __asm__ volatile ("hlt");
    }
    return c;
}