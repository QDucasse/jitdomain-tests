OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
  . = 0x80000000;
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

  /* Eight 4 KiB test pages; page k is tagged with key k (PMP entry k, Track A) */
  . = ALIGN(0x1000);
  .pages (NOLOAD) : { pages = .; . += 8 * 0x1000; }

  /* Two DRS pages, tagged through PMP entry 8 (an 8 KiB NAPOT region) */
  . = ALIGN(0x2000);
  .drs (NOLOAD) : { drs_base = .; . += 2 * 0x1000; drs_top = .; }

  .stack (NOLOAD) : {
    . = ALIGN(16);
    . += 0x4000;
    stack_top = .;
    . += 0x2000;
    trap_stack_top = .;
  }
  _end = .;
}
