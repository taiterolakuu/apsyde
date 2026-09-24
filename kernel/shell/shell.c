#include "shell.h"
#include "../drivers/keyboard.h"
#include "../lib/kprintf.h"
#include "../lib/panic.h"
#include "../lib/string.h"
#include "../intr/pit.h"
#include "../fb/fb.h"
#include "../mm/pmm.h"
#include "../mm/kmalloc.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../bus/pci.h"
#include "../drivers/usb/ehci.h"
#include <stdint.h>
#include <boot_info.h>

#define LINE_MAX       128
#define HISTORY_SIZE   8

/* ---------- История команд ---------- */

static char g_history[HISTORY_SIZE][LINE_MAX];
static int  g_hist_count = 0;
static int  g_hist_head  = 0;

static void history_add(const char *line) {
    if (line[0] == 0) return;

    int last = (g_hist_head - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    if (g_hist_count > 0 && strcmp(g_history[last], line) == 0) return;

    int len = 0;
    while (line[len] && len < LINE_MAX - 1) {
        g_history[g_hist_head][len] = line[len];
        len++;
    }
    g_history[g_hist_head][len] = 0;

    g_hist_head = (g_hist_head + 1) % HISTORY_SIZE;
    if (g_hist_count < HISTORY_SIZE) g_hist_count++;
}

static const char *history_get(int back) {
    if (back >= g_hist_count) return NULL;
    int idx = (g_hist_head - 1 - back + HISTORY_SIZE * 2) % HISTORY_SIZE;
    return g_history[idx];
}

/* ---------- Вспомогательные ---------- */

static int str_starts_with(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void pad_name(const char *s, int width) {
    int n = 0;
    while (s[n]) { kprintf("%c", s[n]); n++; }
    for (int i = n; i < width; i++) kprintf(" ");
}

static uint64_t parse_hex(const char *s) {
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

    uint64_t v = 0;
    while (*s) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | (uint32_t)d;
        s++;
    }
    return v;
}

extern const boot_info_t *shell_get_boot_info(void);

/* ---------- Команды ---------- */

static void cmd_help(void) {
    kprintf("Доступные команды:\n");
    kprintf("  help          - этот список\n");
    kprintf("  ticks         - сколько тиков таймера прошло\n");
    kprintf("  uptime        - время с момента запуска (сек)\n");
    kprintf("  time          - подробное время: тики + секунды + мс\n");
    kprintf("  mem           - карта памяти из boot_info\n");
    kprintf("  regs          - текущие значения регистров CPU\n");
    kprintf("  kbd_test      - показать последние скан-коды\n");
    kprintf("  kbd_dump      - выгрузить накопленные скан-коды\n");
    kprintf("  fb_test       - залить экран цветными полосами\n");
    kprintf("  fb_info       - параметры framebuffer'а\n");
    kprintf("  pmm_info      - статистика PMM\n");
    kprintf("  pmm_regions   - список PMM-регионов\n");
    kprintf("  pmm_alloc     - выделить и освободить одну страницу\n");
    kprintf("  pmm_test      - стресс-тест PMM (100 страниц)\n");
    kprintf("  heap_info     - статистика heap\n");
    kprintf("  heap_dump     - карта блоков heap\n");
    kprintf("  heap_check    - проверка целостности heap\n");
    kprintf("  heap_alloc    - выделить 100 байт через kmalloc\n");
    kprintf("  heap_test     - стресс-тест kmalloc/kfree\n");
    kprintf("  vmm_info      - текущее address space, CR3\n");
    kprintf("  vmm_walk X    - пройти по 4 уровням для адреса X\n");
    kprintf("  vmm_test      - создать AS, замапить, переключиться\n");
    kprintf("  pci_info      - список PCI устройств\n");
    kprintf("  pci_ehci      - поиск EHCI/xHCI/UHCI контроллеров\n");
    kprintf("  ehci_info     - информация об EHCI контроллере\n");
    kprintf("  history       - история команд\n");
    kprintf("  echo X        - напечатать X\n");
    kprintf("  clear         - очистить экран\n");
    kprintf("  panic         - намеренная паника\n");
}

static void cmd_ticks(void) {
    kprintf("ticks = %u\n", (uint32_t)pit_ticks());
}

static void cmd_uptime(void) {
    uint64_t t = pit_ticks() / 100;
    kprintf("uptime = %u сек\n", (uint32_t)t);
}

static void cmd_time(void) {
    uint64_t t = pit_ticks();
    kprintf("ticks   = %u\n", (uint32_t)t);
    kprintf("seconds = %u\n", (uint32_t)(t / 100));
    kprintf("ms      = %u\n", (uint32_t)((t % 100) * 10));
}

/* --- mem --- */

static const char *mem_type_name(uint32_t type) {
    switch (type) {
        case 0:  return "Reserved";
        case 1:  return "LoaderCode";
        case 2:  return "LoaderData";
        case 3:  return "BootSvcCode";
        case 4:  return "BootSvcData";
        case 5:  return "RuntimeCode";
        case 6:  return "RuntimeData";
        case 7:  return "Conventional";
        case 8:  return "Unusable";
        case 9:  return "ACPIReclaim";
        case 10: return "ACPIMemNVS";
        case 11: return "MMIO";
        case 12: return "MMIOPort";
        case 13: return "PalCode";
        case 14: return "Persistent";
        default: return "Unknown";
    }
}

static void cmd_mem(void) {
    const boot_info_t *bi = shell_get_boot_info();
    if (!bi || !bi->memory_map) {
        kprintf("boot_info или memory_map недоступны\n");
        return;
    }

    uint64_t descriptor_count = bi->memory_map_size / bi->descriptor_size;

    kprintf("Всего дескрипторов: %u\n\n", (uint32_t)descriptor_count);
    kprintf("Тип              Начало(физ)        Страниц   Размер\n");
    kprintf("-------------------------------------------------------\n");

    uint8_t *p = (uint8_t *)bi->memory_map;
    uint64_t total_pages = 0, conv_pages = 0;

    for (uint64_t i = 0; i < descriptor_count; i++) {
        mem_desc_t *d = (mem_desc_t *)(p + i * bi->descriptor_size);

        total_pages += d->NumberOfPages;
        if (d->Type == EFI_CONVENTIONAL_MEMORY) conv_pages += d->NumberOfPages;

        if (d->NumberOfPages >= 256) {
            uint64_t bytes = d->NumberOfPages * 4096ULL;
            pad_name(mem_type_name(d->Type), 16);
            kprintf(" %x  %u  %u МБ\n",
                    (uint64_t)d->PhysicalStart,
                    (uint64_t)d->NumberOfPages,
                    (uint32_t)(bytes / (1024 * 1024)));
        }
    }

    kprintf("-------------------------------------------------------\n");
    kprintf("Всего страниц:    %u (%u МБ)\n",
            (uint32_t)total_pages,
            (uint32_t)(total_pages * 4096 / (1024 * 1024)));
    kprintf("Свободных (RAM):  %u (%u МБ)\n",
            (uint32_t)conv_pages,
            (uint32_t)(conv_pages * 4096 / (1024 * 1024)));
}

/* --- regs --- */

static void cmd_regs(void) {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, cr0, cr2, cr3, cr4;
    uint16_t cs, ds, ss, es, fs, gs;

    __asm__ volatile ("mov %%rax, %0" : "=r"(rax));
    __asm__ volatile ("mov %%rbx, %0" : "=r"(rbx));
    __asm__ volatile ("mov %%rcx, %0" : "=r"(rcx));
    __asm__ volatile ("mov %%rdx, %0" : "=r"(rdx));
    __asm__ volatile ("mov %%rsi, %0" : "=r"(rsi));
    __asm__ volatile ("mov %%rdi, %0" : "=r"(rdi));
    __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile ("mov %%r8,  %0" : "=r"(r8));
    __asm__ volatile ("mov %%r9,  %0" : "=r"(r9));
    __asm__ volatile ("mov %%r10, %0" : "=r"(r10));
    __asm__ volatile ("mov %%r11, %0" : "=r"(r11));
    __asm__ volatile ("mov %%r12, %0" : "=r"(r12));
    __asm__ volatile ("mov %%r13, %0" : "=r"(r13));
    __asm__ volatile ("mov %%r14, %0" : "=r"(r14));
    __asm__ volatile ("mov %%r15, %0" : "=r"(r15));
    __asm__ volatile ("lea (%%rip), %0" : "=r"(rip));
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile ("mov %%ds, %0" : "=r"(ds));
    __asm__ volatile ("mov %%ss, %0" : "=r"(ss));
    __asm__ volatile ("mov %%es, %0" : "=r"(es));
    __asm__ volatile ("mov %%fs, %0" : "=r"(fs));
    __asm__ volatile ("mov %%gs, %0" : "=r"(gs));

    kprintf("RAX=%x RBX=%x RCX=%x RDX=%x\n", rax, rbx, rcx, rdx);
    kprintf("RSI=%x RDI=%x RBP=%x RSP=%x\n", rsi, rdi, rbp, rsp);
    kprintf("R8 =%x R9 =%x R10=%x R11=%x\n", r8, r9, r10, r11);
    kprintf("R12=%x R13=%x R14=%x R15=%x\n", r12, r13, r14, r15);
    kprintf("RIP=%x RFLAGS=%x\n", rip, rflags);
    kprintf("CS =%x DS =%x SS =%x ES =%x FS =%x GS =%x\n",
            (uint64_t)cs, (uint64_t)ds, (uint64_t)ss,
            (uint64_t)es, (uint64_t)fs, (uint64_t)gs);
    kprintf("CR0=%x CR2=%x CR3=%x CR4=%x\n", cr0, cr2, cr3, cr4);
}

/* --- kbd_test --- */

static void cmd_kbd_test(void) {
    kprintf("Всего скан-кодов с момента старта: %u\n",
            (uint32_t)keyboard_total_scancodes());
    kprintf("Нажмите клавиши и введите 'kbd_test' ещё раз.\n\n");

    uint8_t buf[KBD_SC_LOG_SIZE];
    int n = keyboard_take_scancodes(buf, KBD_SC_LOG_SIZE);

    if (n == 0) {
        kprintf("(пусто — ничего не нажато с прошлого раза)\n");
        return;
    }

    kprintf("Последние %d скан-кодов:\n", n);
    for (int i = 0; i < n; i++) {
        uint8_t sc = buf[i];
        int released = sc & 0x80;
        uint8_t code = sc & 0x7F;
        kprintf("  %x  (код %u, %s)\n", sc, (uint32_t)code,
                released ? "отпущена" : "нажата");
    }
}

static void cmd_kbd_dump(void) {
    uint8_t buf[KBD_SC_LOG_SIZE];
    int n = keyboard_take_scancodes(buf, KBD_SC_LOG_SIZE);
    if (n == 0) {
        kprintf("(пусто)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        kprintf("%x ", (uint32_t)buf[i]);
    }
    kprintf("\n");
}

/* --- fb_test / fb_info --- */

static void cmd_fb_test(void) {
    if (!fb_enabled()) {
        kprintf("framebuffer отключён\n");
        return;
    }

    uint32_t w, h;
    fb_info(&w, &h, 0, 0, 0, 0);

    kprintf("Заливаю экран цветными полосами (BGRA)...\n");

    uint32_t colors[8] = {
        0x00FFFFFF, 0x000000FF, 0x0000FF00, 0x00FF0000,
        0x0000FFFF, 0x00FF00FF, 0x00FFFF00, 0x00000000,
    };

    uint32_t band = h / 8;
    for (int i = 0; i < 8; i++) {
        fb_fill_rect(0, i * band, w, band, colors[i]);
    }

    fb_clear();
    kprintf("Готово. Экран перерисован.\n");
}

static void cmd_fb_info(void) {
    if (!fb_enabled()) {
        kprintf("framebuffer отключён\n");
        return;
    }
    uint32_t w, h, p, f, c, r;
    fb_info(&w, &h, &p, &f, &c, &r);
    kprintf("Размер:  %ux%u\n", w, h);
    kprintf("Pitch:   %u пикселей\n", p);
    kprintf("Формат:  %u\n", f);
    kprintf("Курсор:  колонка=%u, строка=%u\n", c, r);
}

/* --- PMM --- */

static void cmd_pmm_info(void) {
    kprintf("Всего страниц:  %u (%u МБ)\n",
            (uint32_t)pmm_stat_total_pages(),
            (uint32_t)(pmm_stat_total_bytes() / (1024 * 1024)));
    kprintf("Свободно:       %u (%u МБ)\n",
            (uint32_t)pmm_stat_free_pages(),
            (uint32_t)(pmm_stat_free_bytes() / (1024 * 1024)));
    kprintf("Занято:         %u (%u МБ)\n",
            (uint32_t)pmm_stat_used_pages(),
            (uint32_t)((pmm_stat_used_pages() * 4096) / (1024 * 1024)));
}

static void cmd_pmm_regions(void) {
    int n;
    const pmm_region_t *r = pmm_regions(&n);
    kprintf("PMM регионов: %d\n\n", n);
    kprintf("Тип              Адрес              Страниц   Флаги\n");
    kprintf("----------------------------------------------------\n");
    for (int i = 0; i < n; i++) {
        pad_name(mem_type_name(r[i].type), 16);
        kprintf(" %x  %u  ", r[i].base, r[i].pages);
        if (r[i].flags & PMM_REGION_USABLE)   kprintf("USABLE ");
        if (r[i].flags & PMM_REGION_RESERVED) kprintf("RESERVED ");
        if (r[i].flags & PMM_REGION_ACPI)     kprintf("ACPI ");
        kprintf("\n");
    }
}

static void cmd_pmm_alloc(void) {
    void *p = pmm_alloc_page();
    if (!p) {
        kprintf("Не удалось выделить страницу\n");
        return;
    }
    kprintf("Выделено: %p\n", p);
    kprintf("Свободно теперь: %u\n", (uint32_t)pmm_stat_free_pages());
    pmm_free_page(p);
    kprintf("Освобождено. Свободно: %u\n", (uint32_t)pmm_stat_free_pages());
}

static void cmd_pmm_test(void) {
    const int N = 100;
    void *ptrs[100];

    kprintf("Тест: выделяю %d страниц...\n", N);
    uint64_t before = pmm_stat_free_pages();

    for (int i = 0; i < N; i++) {
        ptrs[i] = pmm_alloc_page();
        if (!ptrs[i]) {
            kprintf("[!] Не удалось выделить страницу %d\n", i);
            for (int j = 0; j < i; j++) pmm_free_page(ptrs[j]);
            return;
        }
    }

    kprintf("Выделено. Свободно: %u (было %u)\n",
            (uint32_t)pmm_stat_free_pages(), (uint32_t)before);

    int unique = 1;
    for (int i = 0; i < N && unique; i++) {
        for (int j = i + 1; j < N; j++) {
            if (ptrs[i] == ptrs[j]) { unique = 0; break; }
        }
    }
    kprintf("Уникальность адресов: %s\n", unique ? "OK" : "FAIL");

    for (int i = 0; i < N; i++) pmm_free_page(ptrs[i]);

    kprintf("Освобождено. Свободно: %u\n", (uint32_t)pmm_stat_free_pages());
    kprintf("Результат: %s\n",
            pmm_stat_free_pages() == before ? "OK" : "FAIL");
}

/* --- HEAP --- */

static void cmd_heap_info(void) {
    heap_stats_t s;
    heap_get_stats(&s);
    kprintf("Heap total:   %u байт (%u КБ)\n",
            (uint32_t)s.total_bytes, (uint32_t)(s.total_bytes / 1024));
    kprintf("Heap used:    %u байт (%u КБ)\n",
            (uint32_t)s.used_bytes, (uint32_t)(s.used_bytes / 1024));
    kprintf("Heap free:    %u байт (%u КБ)\n",
            (uint32_t)s.free_bytes, (uint32_t)(s.free_bytes / 1024));
    kprintf("Блоков всего: %u\n", (uint32_t)s.total_blocks);
    kprintf("  занятых:    %u\n", (uint32_t)s.used_blocks);
    kprintf("  свободных:  %u\n", (uint32_t)s.free_blocks);
    kprintf("Всего alloc:  %u\n", (uint32_t)s.alloc_count);
    kprintf("Всего free:   %u\n", (uint32_t)s.free_count);
}

static void cmd_heap_dump(void) {
    heap_dump(40);
}

static void cmd_heap_check(void) {
    if (heap_check()) kprintf("Heap цел.\n");
    else              kprintf("Heap ПОВРЕЖДЁН!\n");
}

static void cmd_heap_alloc(void) {
    void *p = kmalloc(100);
    if (!p) {
        kprintf("kmalloc(100) вернул NULL\n");
        return;
    }
    kprintf("kmalloc(100) = %p\n", p);
    kprintf("Выравнивание: %s\n",
            ((uint64_t)p & 0xF) == 0 ? "OK (16 байт)" : "FAIL");
    kfree(p);
    kprintf("Освобождено.\n");
}

static void cmd_heap_test(void) {
    const int N = 100;
    void *ptrs[100];
    size_t sizes[100];

    kprintf("Тест kmalloc/kfree: %d блоков разного размера...\n", N);

    for (int i = 0; i < N; i++) {
        sizes[i] = 8 + (i * 37) % 512;
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i]) {
            kprintf("[!] kmalloc(%u) = NULL на i=%d\n",
                    (uint32_t)sizes[i], i);
            for (int j = 0; j < i; j++) kfree(ptrs[j]);
            return;
        }
        if (((uint64_t)ptrs[i] & 0xF) != 0) {
            kprintf("[!] Невыровненный указатель: %p\n", ptrs[i]);
            for (int j = 0; j <= i; j++) kfree(ptrs[j]);
            return;
        }
    }

    int unique = 1;
    for (int i = 0; i < N && unique; i++) {
        for (int j = i + 1; j < N; j++) {
            if (ptrs[i] == ptrs[j]) { unique = 0; break; }
        }
    }
    kprintf("Уникальность:  %s\n", unique ? "OK" : "FAIL");

    for (int i = 0; i < N; i++) {
        memset(ptrs[i], (uint8_t)(i & 0xFF), sizes[i]);
    }
    int pattern_ok = 1;
    for (int i = 0; i < N && pattern_ok; i++) {
        uint8_t *p = (uint8_t *)ptrs[i];
        for (size_t k = 0; k < sizes[i]; k++) {
            if (p[k] != (uint8_t)(i & 0xFF)) {
                pattern_ok = 0;
                kprintf("[!] Повреждён блок %d, байт %u\n", i, (uint32_t)k);
                break;
            }
        }
    }
    kprintf("Целостность:   %s\n", pattern_ok ? "OK" : "FAIL");

    for (int i = N - 1; i >= 0; i--) kfree(ptrs[i]);

    int check_ok = heap_check();
    kprintf("heap_check:    %s\n", check_ok ? "OK" : "FAIL");

    heap_stats_t s;
    heap_get_stats(&s);
    kprintf("Свободно после: %u КБ\n", (uint32_t)(s.free_bytes / 1024));

    kprintf("Результат:     %s\n",
            (unique && pattern_ok && check_ok) ? "OK" : "FAIL");
}

/* --- VMM --- */

static void cmd_vmm_info(void) {
    vmm_dump_current();
}

static void cmd_vmm_walk(const char *args) {
    uint64_t addr = parse_hex(args);
    vmm_walk(g_kernel_as, addr);
}

static void cmd_vmm_test(void) {
    kprintf("VMM test: создаём новое address space...\n");

    address_space_t *as = vmm_create_address_space();
    if (!as) {
        kprintf("FAIL: vmm_create_address_space вернул NULL\n");
        return;
    }
    kprintf("Новое AS: %p, PML4 = %p\n", as, as->pml4);

    uint64_t test_virt = 0x40000000ULL;
    void *phys_page = pmm_alloc_page();
    if (!phys_page) {
        kprintf("FAIL: pmm_alloc_page\n");
        return;
    }
    uint64_t test_phys = virt_to_phys(phys_page);
    kprintf("Тестовая страница: phys = %x\n", test_phys);

    if (vmm_map(as, test_virt, test_phys, VMM_KERNEL_RW) != 0) {
        kprintf("FAIL: vmm_map\n");
        pmm_free_page(phys_page);
        return;
    }
    kprintf("Замаплено: %x -> %x\n", test_virt, test_phys);

    uint64_t got = vmm_get_phys(as, test_virt);
    kprintf("vmm_get_phys: %x  %s\n", got,
            (got == test_phys) ? "OK" : "FAIL");

    kprintf("Переключаемся на новое AS...\n");
    vmm_switch_to(as);

    kprintf("Мы всё ещё живы — kernel-часть скопирована корректно.\n");

    vmm_switch_to(g_kernel_as);
    kprintf("Вернулись на kernel AS.\n");

    vmm_unmap(as, test_virt);
    vmm_destroy_address_space(as);
    pmm_free_page(phys_page);

    kprintf("Результат: OK\n");
}

/* --- PCI --- */

static void cmd_pci_info(void) {
    pci_dump();
}

static void cmd_pci_ehci(void) {
    pci_device_t d;

    kprintf("Поиск USB контроллеров...\n\n");

    if (pci_find_ehci(&d) > 0) {
        kprintf("EHCI найден:\n");
        kprintf("  %u:%u.%u  Vendor:Device = %x:%x\n",
                (uint32_t)d.bus, (uint32_t)d.device, (uint32_t)d.function,
                d.vendor_id, d.device_id);
        kprintf("  Rev=%u ProgIF=%x\n",
                (uint32_t)d.revision, (uint32_t)d.prog_if);
    } else {
        kprintf("EHCI не найден.\n");
    }

    kprintf("\n");

    if (pci_find_xhci(&d) > 0) {
        kprintf("xHCI найден:\n");
        kprintf("  %u:%u.%u  Vendor:Device = %x:%x\n",
                (uint32_t)d.bus, (uint32_t)d.device, (uint32_t)d.function,
                d.vendor_id, d.device_id);
    } else {
        kprintf("xHCI не найден.\n");
    }

    kprintf("\n");

    pci_device_t u[8];
    int n = pci_find(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                     PCI_PROGIF_UHCI, u, 8);
    kprintf("UHCI найдено: %d\n", n);
    for (int i = 0; i < n; i++) {
        kprintf("  %u:%u.%u  %x:%x\n",
                (uint32_t)u[i].bus, (uint32_t)u[i].device,
                (uint32_t)u[i].function,
                u[i].vendor_id, u[i].device_id);
    }

    kprintf("\n");

    int m = pci_find(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB,
                     PCI_PROGIF_OHCI, u, 8);
    kprintf("OHCI найдено: %d\n", m);
    for (int i = 0; i < m; i++) {
        kprintf("  %u:%u.%u  %x:%x\n",
                (uint32_t)u[i].bus, (uint32_t)u[i].device,
                (uint32_t)u[i].function,
                u[i].vendor_id, u[i].device_id);
    }
}

/* --- EHCI --- */

static void cmd_ehci_info(void) {
    ehci_dump();
}

/* --- history --- */

static void cmd_history(void) {
    if (g_hist_count == 0) {
        kprintf("История пуста.\n");
        return;
    }
    int start = (g_hist_head - g_hist_count + HISTORY_SIZE * 2) % HISTORY_SIZE;
    for (int i = 0; i < g_hist_count; i++) {
        int idx = (start + i) % HISTORY_SIZE;
        kprintf("  %u: %s\n", (uint32_t)(i + 1), g_history[idx]);
    }
}

/* --- echo / clear / panic --- */

static void cmd_echo(const char *args) { kprintf("%s\n", args); }

static void cmd_clear(void) {
    if (fb_enabled()) fb_clear();
    kprintf("\x1b[2J\x1b[H");
}

static void cmd_panic(void) { panic("намеренная паника из shell — тест"); }

/* ---------- Разбор команды ---------- */

static void execute(const char *line) {
    if (line[0] == 0) return;

    if (str_starts_with(line, "help"))         { cmd_help();         return; }
    if (str_starts_with(line, "ticks"))        { cmd_ticks();        return; }
    if (str_starts_with(line, "uptime"))       { cmd_uptime();       return; }
    if (str_starts_with(line, "time"))         { cmd_time();         return; }
    if (str_starts_with(line, "mem"))          { cmd_mem();          return; }
    if (str_starts_with(line, "regs"))         { cmd_regs();         return; }
    if (str_starts_with(line, "kbd_test"))     { cmd_kbd_test();     return; }
    if (str_starts_with(line, "kbd_dump"))     { cmd_kbd_dump();     return; }
    if (str_starts_with(line, "fb_test"))      { cmd_fb_test();      return; }
    if (str_starts_with(line, "fb_info"))      { cmd_fb_info();      return; }
    if (str_starts_with(line, "pmm_info"))     { cmd_pmm_info();     return; }
    if (str_starts_with(line, "pmm_regions"))  { cmd_pmm_regions();  return; }
    if (str_starts_with(line, "pmm_alloc"))    { cmd_pmm_alloc();    return; }
    if (str_starts_with(line, "pmm_test"))     { cmd_pmm_test();     return; }
    if (str_starts_with(line, "heap_info"))    { cmd_heap_info();    return; }
    if (str_starts_with(line, "heap_dump"))    { cmd_heap_dump();    return; }
    if (str_starts_with(line, "heap_check"))   { cmd_heap_check();   return; }
    if (str_starts_with(line, "heap_alloc"))   { cmd_heap_alloc();   return; }
    if (str_starts_with(line, "heap_test"))    { cmd_heap_test();    return; }
    if (str_starts_with(line, "vmm_info"))     { cmd_vmm_info();     return; }
    if (str_starts_with(line, "vmm_walk "))    { cmd_vmm_walk(line+9); return; }
    if (str_starts_with(line, "vmm_walk"))     { cmd_vmm_walk("0");   return; }
    if (str_starts_with(line, "vmm_test"))     { cmd_vmm_test();     return; }
    if (str_starts_with(line, "pci_info"))     { cmd_pci_info();     return; }
    if (str_starts_with(line, "pci_ehci"))     { cmd_pci_ehci();     return; }
    if (str_starts_with(line, "ehci_info"))    { cmd_ehci_info();    return; }
    if (str_starts_with(line, "history"))      { cmd_history();      return; }
    if (str_starts_with(line, "clear"))        { cmd_clear();        return; }
    if (str_starts_with(line, "panic"))        { cmd_panic();        return; }
    if (str_starts_with(line, "echo "))        { cmd_echo(line + 5); return; }
    if (str_starts_with(line, "echo"))         { kprintf("\n");      return; }

    kprintf("Неизвестная команда: %s\n", line);
    kprintf("Введите 'help' для списка.\n");
}

/* ---------- Основной цикл ---------- */

void shell_run(void) {
    static char line[LINE_MAX];
    int pos = 0;
    int hist_pos = -1;

    kprintf("\n");
    kprintf("Celestis Shell. Введите 'help'.\n\n");

    for (;;) {
        kprintf("> ");
        pos = 0;
        hist_pos = -1;
        line[0] = 0;

        for (;;) {
            int c = keyboard_getchar_blocking();

            if (c == '\r' || c == '\n') {
                line[pos] = 0;
                kprintf("\n");
                if (pos > 0) history_add(line);
                execute(line);
                break;
            }

            if (c == '\b' || c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    kprintf("\b \b");
                }
                continue;
            }

            if (c == 3) {
                kprintf("^C\n");
                break;
            }

            if (c == KBD_KEY_UP) {
                int next = hist_pos + 1;
                const char *h = history_get(next);
                if (h) {
                    while (pos-- > 0) kprintf("\b \b");
                    pos = 0;
                    while (h[pos] && pos < LINE_MAX - 1) {
                        line[pos] = h[pos];
                        kprintf("%c", h[pos]);
                        pos++;
                    }
                    hist_pos = next;
                }
                continue;
            }

            if (c == KBD_KEY_DOWN) {
                if (hist_pos > 0) {
                    int next = hist_pos - 1;
                    const char *h = history_get(next);
                    if (h) {
                        while (pos-- > 0) kprintf("\b \b");
                        pos = 0;
                        while (h[pos] && pos < LINE_MAX - 1) {
                            line[pos] = h[pos];
                            kprintf("%c", h[pos]);
                            pos++;
                        }
                        hist_pos = next;
                    }
                } else if (hist_pos == 0) {
                    while (pos-- > 0) kprintf("\b \b");
                    pos = 0;
                    hist_pos = -1;
                }
                continue;
            }

            if (c < 32 || c > 126) continue;

            if (pos < LINE_MAX - 1) {
                line[pos++] = (char)c;
                kprintf("%c", (char)c);
            }
        }
    }
}