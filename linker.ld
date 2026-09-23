ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + X */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    . = 0x100000;

    __kernel_start = .;

    .text : {
        *(.text)
        *(.text.*)
    } :text

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } :text

    .data : {
        *(.data)
        *(.data.*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss)
        *(.bss.*)

        /* Стек ядра: 16 КБ */
        . = ALIGN(16);
        __stack_bottom = .;
        . += 0x4000;
        __stack_top = .;
    } :data

    __kernel_end = .;
}