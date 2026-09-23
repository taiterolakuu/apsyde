#include <efi.h>
#include <efilib.h>
#include <boot_info.h>
#include "elf.h"

EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID fs_guid  = SIMPLE_FILE_SYSTEM_PROTOCOL;

/* GUID для EFI_FILE_INFO — объявляем локально, чтобы не зависеть
 * от имени переменной в конкретной версии gnu-efi. */
static EFI_GUID file_info_guid = {
    0x09576e92, 0x6d3f, 0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}
};

EFI_STATUS elf_load(void *buffer, uint64_t *out_entry);

/* Читает файл целиком в выделенный буфер */
static EFI_STATUS read_file(EFI_FILE_PROTOCOL *root, CHAR16 *path,
                            void **out_buf, UINTN *out_size) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL *file;

    status = uefi_call_wrapper(root->Open, 5, root, &file,
                               path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        Print(L"[!] Не удалось открыть %s: %r\r\n", path, status);
        return status;
    }

    /* Узнаём размер файла */
    EFI_FILE_INFO *info = NULL;
    UINTN info_size = 0;
    status = uefi_call_wrapper(file->GetInfo, 4, file,
                               &file_info_guid, &info_size, NULL);
    if (status != EFI_BUFFER_TOO_SMALL) {
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    status = uefi_call_wrapper(BS->AllocatePool, 3,
                               EfiLoaderData, info_size, (VOID **)&info);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    status = uefi_call_wrapper(file->GetInfo, 4, file,
                               &file_info_guid, &info_size, info);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, info);
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    UINTN size = info->FileSize;
    uefi_call_wrapper(BS->FreePool, 1, info);

    /* Выделяем буфер под файл */
    void *buf = NULL;
    status = uefi_call_wrapper(BS->AllocatePool, 3,
                               EfiLoaderData, size, &buf);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    UINTN read_size = size;
    status = uefi_call_wrapper(file->Read, 3, file, &read_size, buf);
    uefi_call_wrapper(file->Close, 1, file);

    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, buf);
        return status;
    }

    *out_buf = buf;
    *out_size = read_size;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;

    InitializeLib(ImageHandle, SystemTable);
    Print(L"\r\n=== Celestis Bootloader ===\r\n");

    /* --- 1. GOP --- */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    status = uefi_call_wrapper(BS->LocateProtocol, 3,
                               &gop_guid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status)) {
        Print(L"[!] GOP: %r\r\n", status);
        return status;
    }
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *gop_info = gop->Mode->Info;
    Print(L"[+] GOP: %dx%d, base=0x%lx\r\n",
          gop_info->HorizontalResolution, gop_info->VerticalResolution,
          gop->Mode->FrameBufferBase);

    /* --- 2. SimpleFileSystem --- */
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    status = uefi_call_wrapper(BS->LocateProtocol, 3,
                               &fs_guid, NULL, (VOID **)&fs);
    if (EFI_ERROR(status)) {
        Print(L"[!] SimpleFileSystem: %r\r\n", status);
        return status;
    }

    EFI_FILE_PROTOCOL *root = NULL;
    status = uefi_call_wrapper(fs->OpenVolume, 2, fs, &root);
    if (EFI_ERROR(status)) {
        Print(L"[!] OpenVolume: %r\r\n", status);
        return status;
    }

    /* --- 3. Чтение kernel.elf --- */
    void *kernel_buf = NULL;
    UINTN kernel_size = 0;
    status = read_file(root, L"\\kernel.elf", &kernel_buf, &kernel_size);
    if (EFI_ERROR(status)) {
        Print(L"[!] Чтение ядра не удалось\r\n");
        return status;
    }
    Print(L"[+] kernel.elf: %d байт по адресу 0x%lx\r\n",
          kernel_size, (uint64_t)kernel_buf);

    /* --- 4. Парсинг и загрузка ELF --- */
    uint64_t entry = 0;
    status = elf_load(kernel_buf, &entry);
    if (EFI_ERROR(status)) {
        Print(L"[!] ELF load: %r\r\n", status);
        return status;
    }
    Print(L"[+] Точка входа ядра: 0x%lx\r\n", entry);

    /* --- 5. Финальная карта памяти --- */
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    EFI_MEMORY_DESCRIPTOR *map = NULL;

    uefi_call_wrapper(BS->GetMemoryMap, 5,
                      &map_size, map, &map_key, &desc_size, &desc_ver);
    map_size += 2 * desc_size;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, map_size,
                               (VOID **)&map);
    if (EFI_ERROR(status)) {
        Print(L"[!] AllocatePool(map): %r\r\n", status);
        return status;
    }
    status = uefi_call_wrapper(BS->GetMemoryMap, 5,
                               &map_size, map, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        Print(L"[!] GetMemoryMap: %r\r\n", status);
        return status;
    }
    Print(L"[+] Карта памяти: %d дескрипторов\r\n", map_size / desc_size);

    /* Заполняем boot_info */
    boot_info_t boot_info = {0};
    boot_info.magic                 = BOOT_INFO_MAGIC;
    boot_info.framebuffer_base      = gop->Mode->FrameBufferBase;
    boot_info.framebuffer_size      = (uint32_t)gop->Mode->FrameBufferSize;
    boot_info.horizontal_resolution = gop_info->HorizontalResolution;
    boot_info.vertical_resolution   = gop_info->VerticalResolution;
    boot_info.pixels_per_scanline   = gop_info->PixelsPerScanLine;
    boot_info.pixel_format          = gop_info->PixelFormat;
    boot_info.memory_map            = map;
    boot_info.memory_map_size       = map_size;
    boot_info.descriptor_size       = desc_size;

    /* --- 6. Выход из Boot Services --- */
    Print(L"[*] ExitBootServices...\r\n");
    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        /* Карта могла измениться — берём заново и пробуем ещё раз */
        Print(L"[!] Первая попытка: %r, повтор...\r\n", status);
        uefi_call_wrapper(BS->GetMemoryMap, 5,
                          &map_size, map, &map_key, &desc_size, &desc_ver);
        status = uefi_call_wrapper(BS->ExitBootServices, 2,
                                   ImageHandle, map_key);
        if (EFI_ERROR(status)) {
            Print(L"[!] ExitBootServices провалился: %r\r\n", status);
            return status;
        }
    }

    /* --- 7. Прыжок в ядро ---
     * После ExitBootServices нельзя вызывать Print! */
    typedef void (*kernel_entry_t)(boot_info_t *);
    kernel_entry_t kernel = (kernel_entry_t)entry;
    kernel(&boot_info);

    /* Сюда никогда не попадём */
    return EFI_SUCCESS;
}