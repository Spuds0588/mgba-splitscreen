OUTPUT_FORMAT("elf32-littlearm")
ENTRY(_start)

MEMORY {
    rom   (rx) : ORIGIN = 0x08000000, LENGTH = 16M
    ewram (rw) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rw) : ORIGIN = 0x03000000, LENGTH = 32K
}

SECTIONS {
    .text : {
        *(.text*)
    } > rom

    .rodata : {
        *(.rodata*)
    } > rom

    .data : {
        *(.data*)
    } > ewram AT > rom

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > ewram

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.ARM.*)
    }
}
