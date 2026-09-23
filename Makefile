CC := gcc
LD := ld
OBJCOPY := objcopy
NASM := nasm

GNUEFI_DIR := /usr/include/efi
GNUEFI_LIB := /usr/lib

BOOT_CFLAGS := -I$(GNUEFI_DIR) -I$(GNUEFI_DIR)/x86_64 -I$(GNUEFI_DIR)/protocol \
               -Iinclude -Ibootloader/src \
               -Wall -Wextra -ffreestanding -fno-stack-protector \
               -fpic -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
               -fno-builtin

KERNEL_CFLAGS := -Iinclude -Ikernel \
                 -Wall -Wextra -ffreestanding -fno-stack-protector \
                 -fno-pic -mno-red-zone -mno-sse -mno-sse2 -fno-builtin \
                 -nostdlib -nostartfiles

KERNEL_C_SRC := kernel/main.c \
                kernel/serial.c \
                kernel/lib/kprintf.c \
                kernel/lib/string.c \
                kernel/lib/panic.c \
                kernel/arch/x86_64/gdt.c \
                kernel/arch/x86_64/idt.c \
                kernel/intr/pic.c \
                kernel/intr/pit.c \
                kernel/intr/irq.c \
                kernel/drivers/keyboard.c \
                kernel/shell/shell.c \
                kernel/fb/fb.c \
                kernel/fb/font8x16.c \
                kernel/mm/pmm.c \
                kernel/mm/heap.c \
                kernel/mm/kmalloc.c \
                kernel/mm/vmm.c

KERNEL_ASM_SRC := kernel/boot.asm \
                  kernel/arch/x86_64/gdt_flush.asm \
                  kernel/arch/x86_64/isr.asm

KERNEL_OBJ := $(patsubst %.c,build/%.o,$(KERNEL_C_SRC)) \
              $(patsubst %.asm,build/%.o,$(KERNEL_ASM_SRC))

OVMF ?= /usr/share/OVMF/OVMF_CODE_4M.fd

all: build/bootx64.efi build/kernel.elf

# --- Загрузчик ---
build/main.o: bootloader/src/main.c bootloader/src/elf.h include/boot_info.h
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

build/elf.o: bootloader/src/elf.c bootloader/src/elf.h
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

build/bootx64.so: build/main.o build/elf.o
	$(LD) -nostdlib -znocombreloc -T $(GNUEFI_LIB)/elf_x86_64_efi.lds \
	      -shared -Bsymbolic -L$(GNUEFI_LIB) \
	      $(GNUEFI_LIB)/crt0-efi-x86_64.o build/main.o build/elf.o \
	      -l:libgnuefi.a -l:libefi.a -o $@

build/bootx64.efi: build/bootx64.so
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .reloc \
	           --target=efi-app-x86_64 build/bootx64.so $@

# --- Ядро: C ---
build/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

# --- Ядро: ASM ---
build/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

build/kernel.elf: $(KERNEL_OBJ) linker.ld
	$(LD) -nostdlib -T linker.ld -o $@ $(KERNEL_OBJ)

# --- Упаковка ESP ---
image: all
	mkdir -p build/esp/EFI/BOOT
	cp build/bootx64.efi build/esp/EFI/BOOT/BOOTX64.EFI
	cp build/kernel.elf build/esp/kernel.elf
	@echo "ESP готов: build/esp/"

# --- Запуск: интерактивный в терминале (без графики) ---
run: image
	qemu-system-x86_64 \
	    -machine q35 \
	    -m 1024 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive format=raw,file=fat:rw:build/esp \
	    -serial stdio \
	    -monitor none \
	    -display none

# --- Запуск: графическое окно ---
run-gtk: image
	qemu-system-x86_64 \
	    -machine q35 \
	    -m 1024 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive format=raw,file=fat:rw:build/esp \
	    -serial file:build/serial.log \
	    -display gtk

# --- Запуск: фоновый с логом в файл ---
run-bg: image
	rm -f build/serial.log
	qemu-system-x86_64 \
	    -machine q35 \
	    -m 1024 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive format=raw,file=fat:rw:build/esp \
	    -serial file:build/serial.log \
	    -monitor none \
	    -display none \
	    -daemonize
	@sleep 2
	@echo "QEMU запущен. Смотри: make logs"

logs:
	tail -f build/serial.log

kill:
	@pkill -f "qemu-system-x86_64.*q35" && echo "QEMU убит" || echo "QEMU не найден"

clean:
	rm -rf build/*
	rmdir build/esp/EFI/BOOT build/esp/EFI build/esp 2>/dev/null || true

.PHONY: all image run run-gtk run-bg logs kill clean