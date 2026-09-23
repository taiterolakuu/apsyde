#ifndef FB_H
#define FB_H
#include <stdint.h>
#include <boot_info.h>

/* Инициализация framebuffer по boot_info.
 * Если fb_base == 0 — консоль отключается (fb_enabled() == 0). */
void fb_init(const boot_info_t *bi);

int  fb_enabled(void);
uint32_t fb_width(void);
uint32_t fb_height(void);

/* Очистить экран чёрным */
void fb_clear(void);

/* Залить весь экран цветом (формат BGRA) */
void fb_fill(uint32_t color_bgra);

/* Залить прямоугольник (x,y,w,h в пикселях) цветом BGRA */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color_bgra);

/* Поставить пиксель. Цвет — BGRA. */
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color_bgra);

/* Вывод символа в текущую позицию курсора.
 * Обрабатывает \n, \r, \b, \t. Прокручивает экран при заполнении. */
void fb_putc(char c);

/* Вывод строки */
void fb_puts(const char *s);

/* Установить курсор */
void fb_set_cursor(uint32_t col, uint32_t row);

/* Диагностика: получить текущие параметры framebuffer'а.
 * Любой из указателей может быть NULL, если значение не нужно. */
void fb_info(uint32_t *out_width, uint32_t *out_height,
             uint32_t *out_pitch, uint32_t *out_format,
             uint32_t *out_col, uint32_t *out_row);

#endif