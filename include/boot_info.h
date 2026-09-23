#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>

#define BOOT_INFO_MAGIC 0x43454C4553544953ULL  /* "CELESTIS" */

/* Описание одного региона физической памяти.
 * Формат совпадает с UEFI EFI_MEMORY_DESCRIPTOR (64-битный режим),
 * но имеет другое имя, чтобы не конфликтовать с gnu-efi.
 *
 * Используется ТОЛЬКО в ядре. Загрузчик по-прежнему работает с
 * EFI_MEMORY_DESCRIPTOR из <efi.h> — бинарный формат совпадает. */
typedef struct {
    uint32_t Type;
    uint32_t _pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} mem_desc_t;

/* Типы памяти UEFI */
#define EFI_RESERVED_MEMORY_TYPE          0
#define EFI_LOADER_CODE                   1
#define EFI_LOADER_DATA                   2
#define EFI_BOOT_SERVICES_CODE            3
#define EFI_BOOT_SERVICES_DATA            4
#define EFI_RUNTIME_SERVICES_CODE         5
#define EFI_RUNTIME_SERVICES_DATA         6
#define EFI_CONVENTIONAL_MEMORY           7
#define EFI_UNUSABLE_MEMORY               8
#define EFI_ACPI_RECLAIM_MEMORY           9
#define EFI_ACPI_MEMORY_NVS              10
#define EFI_MEMORY_MAPPED_IO             11
#define EFI_MEMORY_MAPPED_IO_PORT_SPACE  12
#define EFI_PAL_CODE                     13
#define EFI_PERSISTENT_MEMORY            14

typedef struct {
    uint64_t magic;
    uint64_t framebuffer_base;
    uint32_t framebuffer_size;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixels_per_scanline;
    uint32_t pixel_format;
    uint32_t _pad;

    void    *memory_map;
    uint64_t memory_map_size;
    uint64_t descriptor_size;
} boot_info_t;

#endif