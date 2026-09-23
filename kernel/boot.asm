bits 64

global _start
extern kernel_main
extern __stack_top

section .text
_start:
    ; Загрузчик передал нам boot_info в RDI (первый аргумент SysV ABI).
    mov r12, rdi

    ; Устанавливаем свой стек на __stack_top из linker.ld
    mov rsp, __stack_top

    ; Выравниваем RSP на 16 байт (требование SysV ABI перед вызовом)
    and rsp, -16

    ; Обнуляем RBP — конец цепочки кадров для отладчика
    xor rbp, rbp

    ; Передаём boot_info как первый аргумент kernel_main
    mov rdi, r12

    ; Вызываем C-код ядра
    call kernel_main

    ; Если kernel_main вернётся (не должен) — halt
.halt:
    cli
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits