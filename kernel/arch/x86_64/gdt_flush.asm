bits 64
global gdt_flush

section .text
gdt_flush:
    ; RDI = адрес gdt_ptr
    lgdt [rdi]

    ; Перезагружаем сегментные регистры.
    ; В long mode DS/ES/SS игнорируются, но загрузить их нужно,
    ; чтобы сбросить возможный мусор от UEFI.
    mov ax, 0x10        ; селектор data (индекс 2, RPL=0)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; CS нельзя загрузить через mov — только через far return.
    push 0x08           ; селектор code (индекс 1)
    lea rax, [rel .reload_cs]
    push rax
    retfq
.reload_cs:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits