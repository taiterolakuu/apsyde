#include <efi.h>
#include <efilib.h>
#include "elf.h"

EFI_STATUS elf_load(void *buffer, uint64_t *out_entry) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)buffer;

    if (*(uint32_t *)ehdr->e_ident != ELF_MAGIC) return EFI_LOAD_ERROR;
    if (ehdr->e_ident[4] != ELFCLASS64)         return EFI_LOAD_ERROR;
    if (ehdr->e_type != ET_EXEC)                return EFI_LOAD_ERROR;

    Elf64_Phdr *phdr = (Elf64_Phdr *)((uint8_t *)buffer + ehdr->e_phoff);

    /* --- Проход 1: определить общий диапазон --- */
    uint64_t min_addr = UINT64_MAX;
    uint64_t max_addr = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        if (phdr[i].p_paddr < min_addr) min_addr = phdr[i].p_paddr;
        uint64_t end = phdr[i].p_paddr + phdr[i].p_memsz;
        if (end > max_addr) max_addr = end;
    }

    if (min_addr == UINT64_MAX) {
        Print(L"[!] ELF: нет PT_LOAD-сегментов\r\n");
        return EFI_LOAD_ERROR;
    }

    /* Выравниваем границы до 4 КБ */
    uint64_t seg_start = min_addr & ~0xFFFULL;
    uint64_t seg_end   = (max_addr + 0xFFF) & ~0xFFFULL;
    UINTN    total_pages = (seg_end - seg_start) / 4096;

    /* --- Проход 2: выделить весь диапазон одной операцией --- */
    EFI_PHYSICAL_ADDRESS addr = seg_start;
    EFI_STATUS status = uefi_call_wrapper(BS->AllocatePages, 4,
                                          AllocateAddress, EfiLoaderData,
                                          total_pages, &addr);
    if (EFI_ERROR(status)) {
        Print(L"[!] AllocatePages: %r для 0x%lx-0x%lx (%d pages)\r\n",
              status, seg_start, seg_end, total_pages);
        return status;
    }

    Print(L"[+] Область ядра: 0x%lx-0x%lx (%d страниц)\r\n",
          seg_start, seg_end, total_pages);

    /* --- Проход 3: копировать сегменты --- */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;

        uint8_t *dst = (uint8_t *)(uintptr_t)phdr[i].p_paddr;
        uint8_t *src = (uint8_t *)buffer + phdr[i].p_offset;

        /* Копируем filesz байт */
        for (uint64_t b = 0; b < phdr[i].p_filesz; b++) dst[b] = src[b];
        /* Обнуляем bss */
        for (uint64_t b = phdr[i].p_filesz; b < phdr[i].p_memsz; b++) dst[b] = 0;

        Print(L"[+] Segment %d: paddr=0x%lx, filesz=%u, memsz=%u\r\n",
              i, phdr[i].p_paddr, phdr[i].p_filesz, phdr[i].p_memsz);
    }

    *out_entry = ehdr->e_entry;
    return EFI_SUCCESS;
}