#!/usr/bin/env python3
"""Конвертер Terminus BDF (8x16) -> font8x16.c

Читает BDF-файл с шрифтом Terminus (например ter-u16n.bdf),
отбирает глифы для CP1251 (Windows-1251) и генерирует
kernel/fb/font8x16.c в формате const uint8_t font8x16[256][16].

Формат глифа: 16 байт, старший бит — левый пиксель.
"""

import re
import sys

SRC  = sys.argv[1] if len(sys.argv) > 1 else "ter-u16n.bdf"
DEST = sys.argv[2] if len(sys.argv) > 2 else "kernel/fb/font8x16.c"

# ---------------------------------------------------------------
# 1. Таблица CP1251: какие Unicode-коды куда класть
# ---------------------------------------------------------------
# 0x80–0xBF: спецсимволы, пунктуация, буквы с диакритикой (западноевропейские)
CP1251_HIGH = {
    0x80: 0x0402,  # Ђ
    0x81: 0x0403,  # Ѓ
    0x82: 0x201A,  # ‚
    0x83: 0x0453,  # ѓ
    0x84: 0x201E,  # „
    0x85: 0x2026,  # …
    0x86: 0x2020,  # †
    0x87: 0x2021,  # ‡
    0x88: 0x20AC,  # €
    0x89: 0x2030,  # ‰
    0x8A: 0x0409,  # Љ
    0x8B: 0x2039,  # ‹
    0x8C: 0x040A,  # Њ
    0x8D: 0x040C,  # Ќ
    0x8E: 0x040B,  # Ћ
    0x8F: 0x040F,  # Џ
    0x90: 0x0452,  # ђ
    0x91: 0x2018,  # ‘
    0x92: 0x2019,  # ’
    0x93: 0x201C,  # “
    0x94: 0x201D,  # ”
    0x95: 0x2022,  # •
    0x96: 0x2013,  # –
    0x97: 0x2014,  # —
    0x99: 0x2122,  # ™
    0x9A: 0x0459,  # љ
    0x9B: 0x203A,  # ›
    0x9C: 0x045A,  # њ
    0x9D: 0x045C,  # ќ
    0x9E: 0x045B,  # ћ
    0x9F: 0x045F,  # џ
    0xA0: 0x00A0,  # NBSP
    0xA1: 0x040E,  # Ў
    0xA2: 0x045E,  # ў
    0xA3: 0x0408,  # Ј
    0xA4: 0x00A4,  # ¤
    0xA5: 0x0490,  # Ґ
    0xA6: 0x00A6,  # ¦
    0xA7: 0x00A7,  # §
    0xA8: 0x0401,  # Ё
    0xA9: 0x00A9,  # ©
    0xAA: 0x0404,  # Є
    0xAB: 0x00AB,  # «
    0xAC: 0x00AC,  # ¬
    0xAD: 0x00AD,  # SHY
    0xAE: 0x00AE,  # ®
    0xAF: 0x0407,  # Ї
    0xB0: 0x00B0,  # °
    0xB1: 0x00B1,  # ±
    0xB2: 0x0406,  # І
    0xB3: 0x0456,  # і
    0xB4: 0x0491,  # ґ
    0xB5: 0x00B5,  # µ
    0xB6: 0x00B6,  # ¶
    0xB7: 0x00B7,  # ·
    0xB8: 0x0451,  # ё
    0xB9: 0x2116,  # №
    0xBA: 0x0454,  # є
    0xBB: 0x00BB,  # »
    0xBC: 0x0458,  # ј
    0xBD: 0x0405,  # Ѕ
    0xBE: 0x0455,  # ѕ
    0xBF: 0x0457,  # ї
}

# 0xC0–0xFF: основная кириллица, порядок совпадает с Unicode!
# В CP1251 буквы идут подряд от U+0410 (А) до U+044F (я),
# что ровно совпадает с диапазоном 0xC0–0xFF. Ничего мапить не нужно.

# ---------------------------------------------------------------
# 2. Парсер BDF
# ---------------------------------------------------------------

def parse_bdf(path):
    """Возвращает dict {unicode_codepoint: [16 байт]}."""
    glyphs = {}
    with open(path, 'r', encoding='latin-1') as f:
        lines = f.readlines()

    i = 0
    while i < len(lines):
        line = lines[i].strip()

        if line.startswith('STARTCHAR'):
            encoding = None
            bbx = None
            bitmap = []
            i += 1
            # Парсим свойства глифа до BEGINBITMAP
            while i < len(lines) and not lines[i].strip().startswith('BITMAP'):
                l = lines[i].strip()
                if l.startswith('ENCODING'):
                    encoding = int(l.split()[1])
                elif l.startswith('BBX'):
                    parts = l.split()
                    bbx = (int(parts[1]), int(parts[2]),
                           int(parts[3]), int(parts[4]))
                i += 1
            # После BITMAP идут строки с hex
            i += 1
            while i < len(lines) and not lines[i].strip().startswith('ENDCHAR'):
                l = lines[i].strip()
                if l and all(c in '0123456789abcdefABCDEF' for c in l):
                    bitmap.append(l)
                i += 1

            if encoding is not None and encoding >= 0 and bbx is not None:
                glyphs[encoding] = bdf_to_8x16(bitmap, bbx)
        i += 1

    return glyphs


def bdf_to_8x16(hex_lines, bbx):
    """Преобразует BDF-глиф в 16 байт по 8 пикселей.

    Terminus 8x16: bbx = (8, 16, 0, -4).
    BDF-строки — big-endian, по 8 бит на строку, с выравниванием
    по ширине BBX. Наш формат: 16 байт, старший бит — левый пиксель.
    """
    width, height, xoff, yoff = bbx

    # Разбираем hex-строки в отдельные строки пикселей (по 8 бит)
    rows = []
    for line in hex_lines:
        # Каждая hex-строка — это одна строка глифа шириной ceil(width/8) байт
        val = int(line, 16)
        # Сдвигаем так, чтобы биты соответствовали пикселям слева
        shift = (len(line) * 4) - width
        rows.append((val >> shift) & 0xFF)

    # Terminus может иметь высоту > 16 (например, для антиалиасинга).
    # Обычно 16, но если меньше — дополняем нулями, если больше — обрезаем.
    if len(rows) < 16:
        # BDF рисует снизу вверх; для Terminus yoff = -4 означает,
        # что базовые 16 строк идут как есть.
        rows = rows + [0] * (16 - len(rows))
    elif len(rows) > 16:
        rows = rows[:16]

    return rows


# ---------------------------------------------------------------
# 3. Основная логика
# ---------------------------------------------------------------

def main():
    print(f"Читаю BDF: {SRC}")
    bdf_glyphs = parse_bdf(SRC)
    print(f"Загружено глифов в BDF: {len(bdf_glyphs)}")

    # Собираем итоговый массив 256 x 16
    out = [[0] * 16 for _ in range(256)]

    # ASCII: 0x20-0x7E берём напрямую
    missing = []
    for code in range(0x20, 0x7F):
        if code in bdf_glyphs:
            out[code] = bdf_glyphs[code]
        else:
            missing.append(f"0x{code:02X}")

    # 0x00-0x1F: пустые (управляющие)
    for code in range(0x20):
        out[code] = [0] * 16

    # CP1251: 0x80-0xBF
    for cp, uni in CP1251_HIGH.items():
        if uni in bdf_glyphs:
            out[cp] = bdf_glyphs[uni]
        else:
            missing.append(f"0x{cp:02X}(U+{uni:04X})")

    # CP1251: 0xC0-0xFF = U+0410-U+044F (А..я)
    for cp in range(0xC0, 0x100):
        uni = 0x0410 + (cp - 0xC0)
        if uni in bdf_glyphs:
            out[cp] = bdf_glyphs[uni]
        else:
            missing.append(f"0x{cp:02X}(U+{uni:04X})")

    if missing:
        print(f"⚠ Отсутствуют глифы ({len(missing)}): "
              f"{', '.join(missing[:10])}"
              f"{'...' if len(missing) > 10 else ''}")
        print("  Они будут нарисованы как пустые квадраты.")
    else:
        print("✓ Все глифы найдены")

    # Генерируем C-файл
    with open(DEST, 'w') as f:
        f.write("/* Auto-generated from Terminus BDF by gen_font.py */\n")
        f.write("/* Encoding: ASCII (0x00-0x7F) + CP1251 (0x80-0xFF) */\n")
        f.write('#include "font8x16.h"\n\n')
        f.write("const uint8_t font8x16[256][16] = {\n")
        for ch in range(256):
            f.write("    /* 0x%02X */ {" % ch)
            f.write(", ".join("0x%02X" % b for b in out[ch]))
            f.write("},\n")
        f.write("};\n")

    print(f"✓ Сгенерирован {DEST}")


if __name__ == "__main__":
    main()