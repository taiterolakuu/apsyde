#include "input.h"
#include "../drivers/keyboard.h"
#include "../intr/pit.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

/* ============ Конфигурация DEMO ============ */

/*
 * Скрипт DEMO-режима.
 * Каждая строка вводится в shell посимвольно с задержкой,
 * затем отправляется '\r' для выполнения.
 *
 * Символ '\r' в конце обязателен — это Enter в shell.
 */
static const char *g_demo_script[] = {
    "help\r",
    "pci_info\r",
    "ehci_info\r",
    "ehci_ports\r",
    "ehci_reset 0\r",
    "ehci_device\r",
    "vmm_info\r",
    "heap_info\r",
    "pmm_info\r",
    "fb_info\r",
    NULL,   /* терминатор */
};

/* Пауза между символами одной команды (тиков PIT, 100 Гц) */
#define DEMO_CHAR_DELAY_TICKS    5      /* 50 мс */

/* Пауза между командами (тиков) */
#define DEMO_LINE_DELAY_TICKS    50     /* 500 мс */

/* Пауза после последней команды, перед началом заново */
#define DEMO_RESTART_DELAY_TICKS 500    /* 5 сек */

/* Через сколько тишины на PS/2/USB включается DEMO (тиков) */
#define DEMO_TIMEOUT_TICKS       300    /* 3 сек */

/* ============ Состояние ============ */

static int g_source = -1;               /* -1 = auto */

static int g_ps2_available = 0;
static int g_usb_available = 0;

/* DEMO state */
static int  g_demo_active     = 0;
static int  g_demo_line       = 0;
static int  g_demo_char       = 0;
static uint64_t g_demo_next_tick = 0;

/* Таймаут DEMO: сколько тиков назад был реальный ввод */
static uint64_t g_last_real_input = 0;

/* ============ Вспомогательное ============ */

static inline uint64_t now_ticks(void) {
    return pit_ticks();
}

/* Заглушка USB HID — когда появится драйвер, заменить на реальный вызов */
static int usb_get_char(void) {
    return -1;
}

/* ============ DEMO ============ */

static void demo_start(void) {
    g_demo_active = 1;
    g_demo_line = 0;
    g_demo_char = 0;
    g_demo_next_tick = now_ticks() + DEMO_LINE_DELAY_TICKS;

    kprintf("\n[DEMO] автоматический ввод включён "
            "(источник: DEMO-скрипт)\n");
    kprintf("[DEMO] всего команд: ");

    int n = 0;
    while (g_demo_script[n]) n++;

    char buf[8];
    int i = 0;
    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    }
    while (i > 0) kprintf("%c", buf[--i]);
    kprintf("\n\n");
}

static void demo_stop(void) {
    if (!g_demo_active) return;
    g_demo_active = 0;
    g_demo_line = 0;
    g_demo_char = 0;
    kprintf("\n[DEMO] автоматический ввод выключен (есть реальный ввод)\n");
}

/*
 * Возвращает следующий символ DEMO-скрипта или -1, если ещё
 * рано (по таймеру).
 */
static int demo_get_char(void) {
    if (!g_demo_active) return -1;

    uint64_t now = now_ticks();

    if (now < g_demo_next_tick) {
        return -1;
    }

    /* Конец скрипта */
    if (!g_demo_script[g_demo_line]) {
        g_demo_line = 0;
        g_demo_char = 0;
        g_demo_next_tick = now + DEMO_RESTART_DELAY_TICKS;
        return -1;
    }

    const char *line = g_demo_script[g_demo_line];
    char c = line[g_demo_char];

    /* Конец строки */
    if (c == 0) {
        g_demo_line++;
        g_demo_char = 0;
        g_demo_next_tick = now + DEMO_LINE_DELAY_TICKS;
        return -1;
    }

    /* Выдаём символ */
    g_demo_char++;
    g_demo_next_tick = now + DEMO_CHAR_DELAY_TICKS;

    return (int)(unsigned char)c;
}

/* ============ Инициализация ============ */

void input_init(void) {
    g_source = -1;

    g_ps2_available = 1;
    g_usb_available = 0;

    g_last_real_input = now_ticks();
    g_demo_active = 0;

    kprintf("[+] INPUT: PS/2=%s USB=%s DEMO=%s\n",
            g_ps2_available ? "yes" : "no",
            g_usb_available ? "yes" : "no",
            "yes (fallback)");
}

/* ============ Публичный API ============ */

void input_set_source(int src) {
    g_source = src;
    if (src != INPUT_SRC_DEMO) {
        g_demo_active = 0;
    }
}

int input_get_source(void) {
    if (g_source >= 0) return g_source;
    if (g_demo_active) return INPUT_SRC_DEMO;
    if (g_usb_available) return INPUT_SRC_USB;
    if (g_ps2_available) return INPUT_SRC_PS2;
    return INPUT_SRC_DEMO;
}

void input_reset_demo_timeout(void) {
    g_last_real_input = now_ticks();
    if (g_demo_active) {
        demo_stop();
    }
}

int input_getchar(void) {
    /* Принудительный источник (для отладки) */
    if (g_source >= 0) {
        switch (g_source) {
            case INPUT_SRC_PS2:
                return keyboard_getchar();
            case INPUT_SRC_USB:
                return usb_get_char();
            case INPUT_SRC_DEMO:
                if (!g_demo_active) demo_start();
                return demo_get_char();
        }
    }

    /*
     * AUTO-режим.
     *
     * ВАЖНО: PS/2 и USB проверяются ВСЕГДА первыми,
     * даже когда DEMO активен. Это позволяет остановить
     * DEMO первым нажатием клавиши.
     *
     * Порядок: PS/2 > USB > DEMO.
     *
     * Приоритет PS/2 перед USB выбран потому, что на QEMU
     * PS/2 есть, а USB HID пока не реализован. На MacBook
     * PS/2 вернёт -1, и управление перейдёт к USB (когда будет).
     */

    /* 1. PS/2 — проверяется всегда */
    if (g_ps2_available) {
        int c = keyboard_getchar();
        if (c >= 0) {
            input_reset_demo_timeout();
            return c;
        }
    }

    /* 2. USB — тоже всегда */
    if (g_usb_available) {
        int c = usb_get_char();
        if (c >= 0) {
            input_reset_demo_timeout();
            return c;
        }
    }

    /* 3. DEMO активируется после длительной тишины */
    if (!g_demo_active) {
        uint64_t now = now_ticks();
        if (now - g_last_real_input >= DEMO_TIMEOUT_TICKS) {
            demo_start();
        }
    }

    /* 4. DEMO выдаёт символы */
    if (g_demo_active) {
        return demo_get_char();
    }

    return -1;
}

int input_getchar_blocking(void) {
    for (;;) {
        int c = input_getchar();
        if (c >= 0) return c;
        __asm__ volatile ("hlt");
    }
}