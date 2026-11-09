/* Linker script for ootgz as a Horizon subsdk module (position independent, linked by rtld).
 * Three page-aligned segments, as elf2nso expects: text (RX), rodata (R), data+bss (RW). */
OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(__module_start)

PHDRS
{
  text    PT_LOAD FLAGS(5);
  rodata  PT_LOAD FLAGS(4);
  data    PT_LOAD FLAGS(6);
  dynamic PT_DYNAMIC;
}

SECTIONS
{
  . = 0;

  .text : {
    KEEP(*(.text.crt0))
    *(.text .text.* .gnu.linkonce.t.*)
  } :text
  .plt : { *(.plt .plt.*) } :text

  . = ALIGN(0x1000);

  .rodata : {
    KEEP(*(.rodata.mod0))
    *(.rodata .rodata.* .gnu.linkonce.r.*)
  } :rodata
  .note.gnu.build-id : { *(.note.gnu.build-id) } :rodata
  .hash     : { *(.hash) } :rodata
  .gnu.hash : { *(.gnu.hash) } :rodata
  .dynsym   : { *(.dynsym) } :rodata
  .dynstr   : { *(.dynstr) } :rodata
  .rela.dyn : { *(.rela.dyn .rela.*) } :rodata
  .rela.plt : { *(.rela.plt) } :rodata
  .eh_frame_hdr : {
    HIDDEN(__eh_frame_hdr_start__ = .);
    *(.eh_frame_hdr)
    HIDDEN(__eh_frame_hdr_end__ = .);
  } :rodata
  .eh_frame : { KEEP(*(.eh_frame)) } :rodata

  . = ALIGN(0x1000);

  .data : { *(.data .data.* .gnu.linkonce.d.*) } :data
  .init_array : {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN(__init_array_end = .);
  } :data
  .fini_array : { KEEP(*(.fini_array .fini_array.*)) } :data
  .dynamic : {
    HIDDEN(__dynamic_start__ = .);
    *(.dynamic)
  } :data :dynamic
  .got     : { *(.got) } :data
  .got.plt : { *(.got.plt) } :data

  .bss (NOLOAD) : {
    . = ALIGN(8);
    HIDDEN(__bss_start__ = .);
    *(.bss.module_runtime)
    *(.bss .bss.* .gnu.linkonce.b.*)
    *(COMMON)
    . = ALIGN(8);
    HIDDEN(__bss_end__ = .);
  } :data

  /DISCARD/ : { *(.interp) *(.note.*) *(.comment) }
}
