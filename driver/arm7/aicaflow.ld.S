#include "aicaflow/protocol.h"
OUTPUT_ARCH(arm)
ENTRY(reset)
SECTIONS {
    .vectors 0 : { KEEP(*(.vectors)) }
    ASSERT(SIZEOF(.vectors) == AFX_FIRMWARE_INFO_OFFSET, "vector table size")
    .manifest AFX_FIRMWARE_INFO_OFFSET : {
        LONG(AFX_FIRMWARE_MAGIC) LONG(AFX_ABI_VERSION) LONG(AFX_LAYOUT_ID)
        LONG(__load_end) LONG(__asset_base) LONG(AFX_ASSET_MAX)
        LONG(__private_end) LONG(AFX_STACK_BASE)
    }
    .text : { *(.text*) *(.rodata*) . = ALIGN(4); }
    __code_end = .;
    .shared AFX_CONTROL_BASE (NOLOAD) : { . += AFX_PRIVATE_BASE - AFX_CONTROL_BASE; }
    .data AFX_PRIVATE_BASE : AT(__code_end) {
        __data_start = .; *(.data*) . = ALIGN(4); __data_end = .;
    }
    __data_load = LOADADDR(.data);
    __load_end = __data_load + SIZEOF(.data);
    __asset_base = ALIGN(__load_end, AFX_UPLOAD_ALIGN);
    .bss (NOLOAD) : {
        __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); __bss_end = .;
    }
    __private_end = .;
    .stacks AFX_STACK_BASE (NOLOAD) : { . += AFX_CLOCK_BASE - AFX_STACK_BASE; }
    .clock AFX_CLOCK_BASE (NOLOAD) : { . += AFX_QUEUE_ADDR - AFX_CLOCK_BASE; }
    .queue AFX_QUEUE_ADDR (NOLOAD) : { . += AFX_AICA_RAM_SIZE - AFX_QUEUE_ADDR; }
    /DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
    ASSERT(__asset_base < AFX_ASSET_MAX, "firmware overlaps asset arena")
    ASSERT(__private_end <= AFX_STACK_BASE, "runtime BSS/data overlaps stacks")
}
