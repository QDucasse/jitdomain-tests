OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
  /* LOAD_ADDR comes from the platform (--defsym) */
  . = LOAD_ADDR;
  .text.init : { *(.text.init) }
  . = ALIGN(0x1000);
  .tohost : { *(.tohost) }
  . = ALIGN(0x1000);
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  .data : { *(.data .data.* .sdata .sdata.*) }
  .bss : {
    __bss_start = .;
    *(.bss .bss.* .sbss .sbss.* COMMON)
    __bss_end = .;
  }

  /* Eight 4 KiB test pages, then the two DRS pages: KEYSRC=pmp covers page 7
     and the DRS with one PMP entry (keysrc-pmp.c) */
  . = ALIGN(0x1000);
  .pages (NOLOAD) : {
    pages = .;
    . += 8 * 0x1000;
    drs_base = .;
    . += 2 * 0x1000;
    drs_top = .;
  }

  .stack (NOLOAD) : {
    . = ALIGN(16);
    . += 0x4000;
    stack_top = .;
    . += 0x2000;
    trap_stack_top = .;
  }
  _end = .;
}
